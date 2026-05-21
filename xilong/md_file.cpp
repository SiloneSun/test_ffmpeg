#include "md_file.h"
#include "t_log.h"
#include "json11.hpp"
#include <iostream>
#include <string>
#include <ostream>
#include <libavutil/log.h>
#include "utils.h"

extern "C" {
#include <libavutil/pixdesc.h>  // /home/sunxilong/work/mycode/ffmpeg-snapshot-git/ffmpeg/libavutil/pixdesc.c
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h> // av_image_alloc / av_image_fill_arrays
#include <libavutil/opt.h>      // av_opt_set_*
#include <libswscale/swscale.h> // sws_getContext / sws_scale
#include <libswresample/swresample.h> // swr_alloc_set_opts2 / swr_convert
#include <SDL2/SDL.h>           // SDL 窗口/渲染/音频
#include <SDL2/SDL_ttf.h>       // SDL 字体渲染
}

using namespace sunxilong;

std::shared_ptr<md_file> md_file::get(std::string file_name)
{
    std::shared_ptr<md_file> instance = std::make_shared<md_file>(file_name);
    if(instance->init() != 0 )
    {
        LOGD("md_file init error");
        return nullptr;
    }
    return instance;
}

md_file::md_file(std::string file_name) :
    m_ff_version(-1),
    m_file_name(file_name),
    m_fmtCtx(empty_obj_t{})   // 不分配，open_file 时才分配
{
}

md_file::~md_file()
{
    deinit();
}

std::int16_t md_file::init()
{
    m_ff_version = avformat_version();
    av_log_set_level(AV_LOG_ERROR);
    LOGD("FFmpeg avformat version: %d: %d.%d", (m_ff_version >> 16) & 0xFF, (m_ff_version >> 8) & 0xFF, m_ff_version & 0xFF);

    // m_fmtCtx 已在构造函数中通过 empty_obj_t 置空
    // 内存分配推迟到 open_file() 中进行

    return 0;
}

std::int16_t md_file::deinit()
{
    return 0;
}

std::int16_t md_file::open_file()
{
    // 先主动分配新的 AVFormatContext，close 后再 open 也能保证 m_ptr 有效
    m_fmtCtx = md_format_ctx{};
    if (!m_fmtCtx) {
        LOGD("avformat_alloc_context failed");
        return -1;
    }
    if (avformat_open_input(m_fmtCtx.ptr(), m_file_name.c_str(), nullptr, nullptr) != 0) {
        LOGD("open failed: %s", m_file_name.c_str());
        m_fmtCtx.reset();
        return -1;
    }
    // 获取流信息
    if (avformat_find_stream_info(m_fmtCtx.get(), nullptr) < 0) {
        LOGD("get stream info failed");
        m_fmtCtx.reset();
        return -1;
    }

    LOGD("open file: %s", m_file_name.c_str());
    LOGD("file path: %s open success", m_fmtCtx->url);
    LOGD("file duration: %llds", (long long)(m_fmtCtx->duration / AV_TIME_BASE));

    // 打印每条流的 index 和类型
    for (unsigned int i = 0; i < m_fmtCtx->nb_streams; i++) {
        AVStream* stream = m_fmtCtx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_index = i;
        }else if(stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_index = i;
        }else if(stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            subtitle_stream_index = i;
        }
        const char* type_str = media_type_str(stream->codecpar->codec_type);
        const char* codec_name = avcodec_get_name(stream->codecpar->codec_id);
        LOGD("stream[%d]: %s (%s)", stream->index, type_str, codec_name);

        LOGD("nb_frames: %d", stream->nb_frames);
        AVCodecParameters* codecpar = stream->codecpar;
        LOGD("width: %d, height: %d, framerate: %d/%d", codecpar->width, codecpar->height,
            stream->avg_frame_rate.num, stream->avg_frame_rate.den);

        bool is_attached_pic = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        LOGD("disposition: %d, is_attached_pic=%d", stream->disposition, is_attached_pic);
        LOGD("attached_pic: %p, size=%d", stream->attached_pic.data, stream->attached_pic.size);
    }

    // 使用 json11 构建每个 stream 的 JSON 并打印
    std::string json_output = build_streams_array(m_fmtCtx.get());
    LOGD("========== streams json ==========");
    std::cout << json_output << std::endl;
    LOGD("========== end ==========");
    return 0;
}

std::int16_t md_file::close_file()
{
    LOGD("========== video packet statistics ==========");
    LOGD("total video packets: %d", video_pkt_count);
    LOGD("  -> keyframe:     %d", keyframe_count);
    LOGD("  -> non-keyframe: %d", non_keyframe_count);

    // avformat_close_input 自动关闭文件并释放资源
    m_fmtCtx.reset();

    // 重置流索引和统计信息，支持重复 open/close
    video_stream_index = -1;
    audio_stream_index = -1;
    subtitle_stream_index = -1;
    video_pkt_count = 0;
    audio_pkt_count = 0;
    keyframe_count = 0;
    non_keyframe_count = 0;

    return 0;
}

// 辅助函数：将 RGB24 数据保存为 BMP 文件
// BMP格式：文件头(14) + DIB头(40) + 像素数据(从下到上, BGR排列)
static void save_rgb_to_bmp(const char *filename,
                            const uint8_t *rgb_data, int w, int h)
{
    int row_size = (w * 3 + 3) & ~3;  // 每行对齐到4字节
    int data_size = row_size * h;
    int file_size = 14 + 40 + data_size;

    FILE *f = fopen(filename, "wb");
    if (!f) return;

    // BMP file header (14 bytes)
    uint8_t header[14] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size>>8),
        (uint8_t)(file_size>>16), (uint8_t)(file_size>>24),
        0,0, 0,0,
        14+40,0,0,0
    };
    fwrite(header, 1, 14, f);

    // DIB header (40 bytes)
    uint8_t dib[40] = {
        40,0,0,0,
        (uint8_t)w, (uint8_t)(w>>8), (uint8_t)(w>>16), (uint8_t)(w>>24),
        (uint8_t)h, (uint8_t)(h>>8), (uint8_t)(h>>16), (uint8_t)(h>>24),
        1,0,
        24,0,
        0,0,0,0,
        0,0,0,0,
        0,0,0,0,
        0,0,0,0,
        0,0,0,0,
        0,0,0,0
    };
    fwrite(dib, 1, 40, f);

    // 像素数据（BMP 是 BGR 从下到上）
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *row = rgb_data + y * w * 3;
        for (int x = 0; x < w; x++) {
            uint8_t r = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            fputc(b, f);  // B
            fputc(g, f);  // G
            fputc(r, f);  // R
        }
        // 补齐对齐字节
        for (int p = w * 3; p < row_size; p++) {
            fputc(0, f);
        }
    }
    fclose(f);
    LOGD("snapshot saved: %s (%dx%d)", filename, w, h);
}

// 辅助函数：在 SDL 渲染器上渲染一行文字（x,y 为左上角坐标，返回下一行 y 坐标）
static int render_text_line(SDL_Renderer *ren, TTF_Font *font, int x, int y,
                            const char *text, SDL_Color color)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return y;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    if (tex) {
        SDL_Rect dst = { x, y, surf->w, surf->h };
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    int h = surf->h;
    SDL_FreeSurface(surf);
    return y + h + 2;  // 行距 2px
}

std::int16_t md_file::play()
{
    // ========== 使用 RAII 类型别名 ==========
    md_codec_ctx decCtx;
    md_codec_ctx audio_dec_ctx;
    md_frame frame;
    md_frame audio_frame;

    if (video_stream_index >= 0) {
        const AVCodec *decoder = avcodec_find_decoder(m_fmtCtx->streams[video_stream_index]->codecpar->codec_id);
        if (!decoder) {
            LOGD("avcodec_find_decoder failed");
            return -1;
        }
        LOGD("find decoder: %s", decoder->name);

        if (avcodec_parameters_to_context(decCtx.get(), m_fmtCtx->streams[video_stream_index]->codecpar) < 0) {
            LOGD("avcodec_parameters_to_context failed");
            return -1;
        }

        if (avcodec_open2(decCtx.get(), decoder, nullptr) < 0) {
            LOGD("avcodec_open2 failed");
            return -1;
        }
    }

    // ============================================================
    // 先初始化 SDL 视频+音频子系统（后面的视频初始化 + 音频设备打开都需要它）
    // ============================================================
    bool sdl_audio_ok = false;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        LOGD("SDL_Init failed: %s", SDL_GetError());
    } else {
        sdl_audio_ok = true;
    }

    // ============================================================
    // 音频解码器 + 重采样 + SDL 音频设备初始化
    // ============================================================
    SwrContext *swr_ctx = nullptr;
    SDL_AudioDeviceID audio_dev_id = 0;
    int audio_dst_sample_rate = 0;
    int audio_dst_channels = 0;
    AVSampleFormat audio_dst_fmt = AV_SAMPLE_FMT_S16;

    if (audio_stream_index >= 0) {
        const AVCodec *decoder = avcodec_find_decoder(m_fmtCtx->streams[audio_stream_index]->codecpar->codec_id);
        if (!decoder) {
            LOGD("avcodec_find_decoder failed");
            return -1;
        }
        LOGD("find decoder: %s", decoder->name);

        if (avcodec_parameters_to_context(audio_dec_ctx.get(), m_fmtCtx->streams[audio_stream_index]->codecpar) < 0) {
            LOGD("avcodec_parameters_to_context failed");
            return -1;
        }

        if (avcodec_open2(audio_dec_ctx.get(), decoder, nullptr) < 0) {
            LOGD("avcodec_open2 failed");
            return -1;
        }

        // --- 获取音频解码参数 ---
        AVSampleFormat src_fmt = audio_dec_ctx->sample_fmt;
        int src_sr = audio_dec_ctx->sample_rate;
        AVChannelLayout *src_ch_layout = &audio_dec_ctx->ch_layout;

        LOGD("audio: fmt=%d, sr=%d, ch=%d",
             src_fmt, src_sr, src_ch_layout->nb_channels);

        // 目标格式：16-bit signed interleaved，采样率保持不变
        audio_dst_sample_rate = src_sr;
        audio_dst_channels = src_ch_layout->nb_channels;

        AVChannelLayout dst_ch_layout;
        av_channel_layout_default(&dst_ch_layout, audio_dst_channels);

        // --- 初始化重采样器 ---
        swr_ctx = swr_alloc();
        if (!swr_ctx) {
            LOGD("swr_alloc failed");
        } else {
            av_opt_set_chlayout(swr_ctx, "in_chlayout",  src_ch_layout, 0);
            av_opt_set_int(swr_ctx,        "in_sample_rate",  src_sr,        0);
            av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",   src_fmt,       0);
            av_opt_set_chlayout(swr_ctx, "out_chlayout", &dst_ch_layout, 0);
            av_opt_set_int(swr_ctx,        "out_sample_rate", audio_dst_sample_rate, 0);
            av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",  audio_dst_fmt, 0);
            if (swr_init(swr_ctx) < 0) {
                LOGD("swr_init failed");
                swr_free(&swr_ctx);
                swr_ctx = nullptr;
            } else {
                LOGD("swr_init ok: in=%d/%d/%d → out=%d/%d/%d",
                     src_fmt, src_sr, src_ch_layout->nb_channels,
                     audio_dst_fmt, audio_dst_sample_rate, audio_dst_channels);
            }
        }

        // --- 打开 SDL 音频设备（使用 SDL_QueueAudio 推送数据，不需要回调） ---
        if (sdl_audio_ok) {
            SDL_AudioSpec desired, obtained;
            SDL_zero(desired);
            desired.freq     = audio_dst_sample_rate;
            desired.format   = AUDIO_S16SYS;
            desired.channels = audio_dst_channels;
            desired.samples  = 4096;   // SDL 内部缓冲区大小
            desired.callback = NULL;   // 不使用回调，直接用 SDL_QueueAudio

            audio_dev_id = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained,
                                               SDL_AUDIO_ALLOW_ANY_CHANGE);
            if (audio_dev_id == 0) {
                LOGD("SDL_OpenAudioDevice failed: %s", SDL_GetError());
            } else {
                LOGD("SDL audio device opened: freq=%d, fmt=%d, ch=%d",
                     obtained.freq, obtained.format, obtained.channels);
                SDL_PauseAudioDevice(audio_dev_id, 0); // 开始播放
            }
        } else {
            LOGD("SDL audio subsystem not available, skip audio device open");
        }
    }

    // ============================================================
    // 1. 获取视频参数
    // ============================================================
    int vid_w = m_fmtCtx->streams[video_stream_index]->codecpar->width;
    int vid_h = m_fmtCtx->streams[video_stream_index]->codecpar->height;

    // 帧率
    AVRational rate = m_fmtCtx->streams[video_stream_index]->avg_frame_rate;
    if (rate.num == 0 || rate.den == 0) {
        rate = m_fmtCtx->streams[video_stream_index]->r_frame_rate;
    }
    int frame_delay_ms = av_rescale_q(1, av_inv_q(rate), (AVRational){1, 1000});
    double fps = (double)rate.num / rate.den;

    // 获取编解码器名称
    const AVCodec *video_codec = avcodec_find_decoder(
        m_fmtCtx->streams[video_stream_index]->codecpar->codec_id);
    const char *codec_name = video_codec ? video_codec->name : "unknown";

    // ============================================================
    // 2. 初始化 SDL + TTF（窗口尺寸基于缩放后的分辨率）
    // ============================================================
    const int OUT_W = 1280;
    const int OUT_H = 720;
    const int PANEL_W = 320;
    int win_w = OUT_W + PANEL_W;
    int win_h = (OUT_H > 480) ? OUT_H : 480;

    SDL_Window   *sdl_win   = nullptr;
    SDL_Renderer *sdl_ren   = nullptr;
    SDL_Texture  *sdl_tex   = nullptr;
    TTF_Font     *sdl_font  = nullptr;
    bool          sdl_ok    = false;

    if (TTF_Init() < 0) {
        LOGD("TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
    } else {
        sdl_font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16);
        if (!sdl_font) {
            LOGD("TTF_OpenFont failed: %s", TTF_GetError());
            TTF_Quit();
            SDL_Quit();
        } else {
            sdl_win = SDL_CreateWindow("FFmpeg Player - [sxl]",
                                       SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED,
                                       win_w, win_h,
                                       SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
            if (!sdl_win) {
                LOGD("SDL_CreateWindow failed: %s", SDL_GetError());
                TTF_CloseFont(sdl_font); TTF_Quit(); SDL_Quit();
            } else {
                sdl_ren = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_ACCELERATED);
                if (!sdl_ren) {
                    LOGD("SDL_CreateRenderer failed: %s", SDL_GetError());
                    SDL_DestroyWindow(sdl_win);
                    TTF_CloseFont(sdl_font); TTF_Quit(); SDL_Quit();
                } else {
                    sdl_tex = SDL_CreateTexture(sdl_ren,
                                                SDL_PIXELFORMAT_IYUV,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                OUT_W, OUT_H);
                    if (!sdl_tex) {
                        LOGD("SDL_CreateTexture failed: %s", SDL_GetError());
                        SDL_DestroyRenderer(sdl_ren); SDL_DestroyWindow(sdl_win);
                        TTF_CloseFont(sdl_font); TTF_Quit(); SDL_Quit();
                    } else {
                        sdl_ok = true;
                    }
                }
            }
        }
    }

    if (!sdl_ok) {
        LOGD("SDL/TTF init failed, will decode without display");
    }

    // ============================================================
    // 3. 解码主循环 + 显示 + 音频播放
    // ============================================================
    md_packet pkt;
    int m_width = 0;
    int m_height = 0;
    uint32_t last_ticks = SDL_GetTicks();
    bool quit = false;

    // ============================================================
    // 4. 初始化 swscale：将解码帧缩放到 1280x720 YUV420P（用于显示）
    // ============================================================
    // sws：YUV420P → YUV420P 缩放（用于 SDL 显示）
    struct SwsContext *sws_ctx = sws_getContext(
        vid_w, vid_h, AV_PIX_FMT_YUV420P,     // 源：原始尺寸 YUV420P
        OUT_W,  OUT_H,  AV_PIX_FMT_YUV420P,    // 目标：1280x720 YUV420P
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        LOGD("sws_getContext failed");
    }

    // 为缩放的 YUV420P 帧分配内存（用于 SDL 显示）
    uint8_t *scale_yuv_data[4] = {NULL};
    int      scale_yuv_linesize[4] = {0};
    av_image_alloc(scale_yuv_data, scale_yuv_linesize,
                   OUT_W, OUT_H, AV_PIX_FMT_YUV420P, 1);

    // sws：YUV420P → RGB24（用于 BMP 保存）
    struct SwsContext *sws_rgb_ctx = sws_getContext(
        vid_w, vid_h, AV_PIX_FMT_YUV420P,
        OUT_W,  OUT_H,  AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t *scale_rgb_data[4] = {NULL};
    int      scale_rgb_linesize[4] = {0};
    av_image_alloc(scale_rgb_data, scale_rgb_linesize,
                   OUT_W, OUT_H, AV_PIX_FMT_RGB24, 1);

    // 统计用
    uint32_t fps_frame_count = 0;
    uint32_t fps_last_tick   = SDL_GetTicks();
    double   realtime_fps    = 0.0;
    uint64_t total_bytes     = 0;      // 累计读取的字节数
    uint32_t bitrate_tick    = SDL_GetTicks();
    uint64_t bitrate_bytes   = 0;      // 当前计时段内的字节数
    double   realtime_bitrate = 0.0;   // 实时码率 (kbps)

    while (!quit) {

        // --- 从文件读取一个 packet ---
        int read_ret = av_read_frame(m_fmtCtx.get(), pkt.get());
        if (read_ret < 0) {
            // 文件结束或出错 → seek 回开头重新播放（循环播放）
            LOGD("end of file, seek to beginning for loop play");
            avcodec_flush_buffers(decCtx.get());
            if (audio_stream_index >= 0) {
                avcodec_flush_buffers(audio_dec_ctx.get());
            }
            av_seek_frame(m_fmtCtx.get(), -1, 0, AVSEEK_FLAG_BACKWARD);
            // 重置统计信息
            video_pkt_count = 0;
            audio_pkt_count = 0;
            keyframe_count = 0;
            non_keyframe_count = 0;
            // 帧率统计也重置
            fps_frame_count = 0;
            fps_last_tick = SDL_GetTicks();
            realtime_fps = 0.0;
            // 码率统计重置
            total_bytes = 0;
            bitrate_bytes = 0;
            bitrate_tick = SDL_GetTicks();
            realtime_bitrate = 0.0;
            av_packet_unref(pkt.get());
            continue;
        }

        // --- 事件处理 ---
        if (sdl_ok) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    quit = true;
                } else if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        quit = true;
                    }
                }
            }
        }
        if (quit) break;

        // --- 累计字节数（用于码率统计） ---
        total_bytes     += pkt.get()->size;
        bitrate_bytes   += pkt.get()->size;

        if (pkt->stream_index == video_stream_index) {
            video_pkt_count++;

            if (pkt->flags & AV_PKT_FLAG_KEY) {
                keyframe_count++;
            } else {
                non_keyframe_count++;
            }

            if (avcodec_send_packet(decCtx.get(), pkt.get()) < 0) {
                LOGD("avcodec_send_packet failed");
                av_packet_unref(pkt.get());
                continue;
            }

            int ret = avcodec_receive_frame(decCtx.get(), frame.get());
            if (ret == 0) {
                if (m_width != frame->width || m_height != frame->height) {
                    m_width = frame->width;
                    m_height = frame->height;
                }

                // --- 使用 sws_scale 将解码帧缩放到 1280x720 YUV420P（用于显示） ---
                if (sws_ctx && scale_yuv_data[0]) {
                    sws_scale(sws_ctx,
                              frame->data, frame->linesize,   // 源 YUV420P
                              0, vid_h,
                              scale_yuv_data, scale_yuv_linesize);  // 目标 YUV420P
                }

                // --- 缩放到 RGB24（用于 BMP 保存，仅第一帧） ---
                if (sws_rgb_ctx && scale_rgb_data[0]) {
                    sws_scale(sws_rgb_ctx,
                              frame->data, frame->linesize,
                              0, vid_h,
                              scale_rgb_data, scale_rgb_linesize);
                    static int snap_count = 0;
                    if (snap_count == 0) {
                        char fname[64];
                        snprintf(fname, sizeof(fname), "snap_%03d.bmp", snap_count);
                        save_rgb_to_bmp(fname, scale_rgb_data[0], OUT_W, OUT_H);
                        snap_count++;
                    }
                }

                // --- 帧率 & 码率统计（每秒更新一次） ---
                fps_frame_count++;
                uint32_t now_tick = SDL_GetTicks();
                if (now_tick - fps_last_tick >= 1000) {
                    realtime_fps = fps_frame_count * 1000.0 / (now_tick - fps_last_tick);
                    fps_frame_count = 0;
                    fps_last_tick = now_tick;

                    // 实时码率 = 计时段字节数 * 8 / 1000 / 秒数 (kbps)
                    double secs = (now_tick - bitrate_tick) / 1000.0;
                    if (secs > 0) {
                        realtime_bitrate = (bitrate_bytes * 8.0) / 1000.0 / secs;
                    }
                    bitrate_bytes = 0;
                    bitrate_tick = now_tick;
                }

                // --- 延时控制 ---
                if (sdl_ok) {
                    uint32_t now = SDL_GetTicks();
                    int elapsed = now - last_ticks;
                    if (elapsed < frame_delay_ms) {
                        SDL_Delay(frame_delay_ms - elapsed);
                    }
                    last_ticks = SDL_GetTicks();

                    // 渲染左半：缩放后的视频画面（1280x720 铺满左侧区域）
                    SDL_UpdateYUVTexture(sdl_tex, NULL,
                                         scale_yuv_data[0], scale_yuv_linesize[0],
                                         scale_yuv_data[1], scale_yuv_linesize[1],
                                         scale_yuv_data[2], scale_yuv_linesize[2]);

                    // 清空整个窗口为黑色
                    SDL_SetRenderDrawColor(sdl_ren, 0, 0, 0, 255);
                    SDL_RenderClear(sdl_ren);

                    // 左边渲染缩放后的视频（铺满左侧区域）
                    SDL_Rect vid_rect = { 0, 0, OUT_W, OUT_H };
                    SDL_RenderCopy(sdl_ren, sdl_tex, NULL, &vid_rect);

                    // 右边：统计面板（深色背景）
                    SDL_Rect panel_rect = { OUT_W, 0, win_w - OUT_W, win_h };
                    SDL_SetRenderDrawColor(sdl_ren, 30, 30, 40, 255);
                    SDL_RenderFillRect(sdl_ren, &panel_rect);

                    // 绘制分隔线
                    SDL_SetRenderDrawColor(sdl_ren, 80, 80, 100, 255);
                    SDL_RenderDrawLine(sdl_ren, vid_w, 0, vid_w, win_h);

                    // 文字颜色
                    SDL_Color white  = { 220, 220, 220, 255 };
                    SDL_Color yellow = { 255, 220, 80,  255 };
                    SDL_Color green  = { 100, 255, 100, 255 };

                    int px = OUT_W + 10;  // 面板左边缘
                    int py = 10;

                    py = render_text_line(sdl_ren, sdl_font, px, py,
                                          "[ File Info ]", yellow);
                    {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Name: %s", m_file_name.c_str());
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Size(src): %dx%d", vid_w, vid_h);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Codec: %s", codec_name);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "FPS(target): %.2f", fps);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Duration: %llds",
                                 (long long)(m_fmtCtx->duration / AV_TIME_BASE));
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }

                    py += 8;
                    py = render_text_line(sdl_ren, sdl_font, px, py,
                                          "[ Real-time Stats ]", yellow);

                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "FPS(now): %.1f", realtime_fps);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, green);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Video Pkts: %d", video_pkt_count);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Audio Pkts: %d", audio_pkt_count);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Keyframes: %d", keyframe_count);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Non-KF: %d", non_keyframe_count);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        int64_t file_bitrate = m_fmtCtx->bit_rate;
                        if (file_bitrate > 0) {
                            snprintf(buf, sizeof(buf), "Bitrate(file): %.0f kbps",
                                     (double)file_bitrate / 1000.0);
                        } else {
                            snprintf(buf, sizeof(buf), "Bitrate(file): N/A");
                        }
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Bitrate(now): %.0f kbps", realtime_bitrate);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, green);
                    }
                    {
                        char buf[128];
                        const char *fmt_name = av_get_pix_fmt_name((AVPixelFormat)frame->format);
                        snprintf(buf, sizeof(buf), "Format: %s", fmt_name ? fmt_name : "unknown");
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Total bytes: %lld",
                                 (long long)total_bytes);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
                    }
                    {
                        // 显示缩放输出信息
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Scale out: %dx%d", OUT_W, OUT_H);
                        py = render_text_line(sdl_ren, sdl_font, px, py, buf, green);
                    }

                    SDL_RenderPresent(sdl_ren);
                }

            } else if (ret == AVERROR(EAGAIN)) {
                // 需要更多数据
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
            }

        } else if (pkt->stream_index == audio_stream_index) {
            audio_pkt_count++;
            if (avcodec_send_packet(audio_dec_ctx.get(), pkt.get()) < 0) {
                LOGD("avcodec_send_packet failed");
                av_packet_unref(pkt.get());
                continue;
            }
            int ret = avcodec_receive_frame(audio_dec_ctx.get(), audio_frame.get());
            if (ret == 0) {
                // --- 将解码后的 PCM 重采样为 S16 并送入 SDL 音频队列播放 ---
                if (swr_ctx && audio_dev_id > 0) {
                    int dst_nb_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
                    if (dst_nb_samples > 0) {
                        uint8_t *dst_data[8] = {NULL};
                        int dst_linesize[8] = {0};
                        int dst_buf_size = av_samples_alloc(dst_data, dst_linesize,
                                                            audio_dst_channels,
                                                            dst_nb_samples,
                                                            audio_dst_fmt, 0);
                        if (dst_data[0]) {
                            const uint8_t *in_data[8];
                            for (int i = 0; i < 8; i++) {
                                in_data[i] = audio_frame->data[i];
                            }
                            int actual_samples = swr_convert(swr_ctx,
                                                             (uint8_t**)dst_data, dst_nb_samples,
                                                             in_data,
                                                             audio_frame->nb_samples);
                            if (actual_samples > 0) {
                                int out_size = av_samples_get_buffer_size(dst_linesize,
                                                                          audio_dst_channels,
                                                                          actual_samples,
                                                                          audio_dst_fmt, 0);
                                if (out_size > 0) {
                                    // 避免队列积压太多数据（超过 ~0.5 秒的数据量则等待）
                                    int max_queued = audio_dst_sample_rate * audio_dst_channels * 2 / 2;
                                    while (SDL_GetQueuedAudioSize(audio_dev_id) > (Uint32)max_queued) {
                                        SDL_Delay(5);
                                    }
                                    SDL_QueueAudio(audio_dev_id, dst_data[0], out_size);
                                }
                            }
                            av_freep(&dst_data[0]);
                        }
                    }
                }
            } else if (ret == AVERROR(EAGAIN)) {
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
            }
        } else if (pkt->stream_index == subtitle_stream_index) {
        } else {
        }

        av_packet_unref(pkt.get());
    }

    // ============================================================
    // 4. 冲刷解码器
    // ============================================================
    if (!quit && video_stream_index >= 0) {
        avcodec_send_packet(decCtx.get(), nullptr);
        while (avcodec_receive_frame(decCtx.get(), frame.get()) == 0) {
            // 冲刷帧不显示
        }
    }

    if (!quit && audio_stream_index >= 0) {
        avcodec_send_packet(audio_dec_ctx.get(), nullptr);
        while (avcodec_receive_frame(audio_dec_ctx.get(), audio_frame.get()) == 0) {
            // 冲刷帧——也送到 SDL 播放
            if (swr_ctx && audio_dev_id > 0) {
                int dst_nb_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
                if (dst_nb_samples > 0) {
                    uint8_t *dst_data = nullptr;
                    int dst_buf_size = av_samples_alloc(&dst_data, NULL,
                                                        audio_dst_channels,
                                                        dst_nb_samples,
                                                        audio_dst_fmt, 0);
                    if (dst_data) {
                        int actual_samples = swr_convert(swr_ctx, &dst_data, dst_nb_samples,
                                                         (const uint8_t**)audio_frame->data,
                                                         audio_frame->nb_samples);
                        if (actual_samples > 0) {
                            int out_size = av_samples_get_buffer_size(NULL,
                                                                      audio_dst_channels,
                                                                      actual_samples,
                                                                      audio_dst_fmt, 0);
                            if (out_size > 0) {
                                SDL_QueueAudio(audio_dev_id, dst_data, out_size);
                            }
                        }
                        av_freep(&dst_data);
                    }
                }
            }
        }
    }

    // ============================================================
    // 5. 等待音频播放完毕
    // ============================================================
    if (audio_dev_id > 0) {
        // 等待直到音频队列耗尽，或超时 3 秒
        uint32_t wait_start = SDL_GetTicks();
        while (SDL_GetQueuedAudioSize(audio_dev_id) > 0) {
            SDL_Delay(10);
            if (SDL_GetTicks() - wait_start > 3000) break;
        }
    }

    // ============================================================
    // 6. 清理 swscale & SDL & swr & audio
    // ============================================================
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (sws_rgb_ctx) sws_freeContext(sws_rgb_ctx);
    if (scale_yuv_data[0]) av_freep(&scale_yuv_data[0]);
    if (scale_rgb_data[0]) av_freep(&scale_rgb_data[0]);

    if (audio_dev_id > 0) {
        SDL_CloseAudioDevice(audio_dev_id);
    }
    if (swr_ctx) swr_free(&swr_ctx);

    if (sdl_tex) SDL_DestroyTexture(sdl_tex);
    if (sdl_ren) SDL_DestroyRenderer(sdl_ren);
    if (sdl_win) SDL_DestroyWindow(sdl_win);
    if (sdl_font) TTF_CloseFont(sdl_font);
    if (sdl_ok) { TTF_Quit(); SDL_Quit(); }

    return 0;
}

bool md_file::end_of_file()
{
    return false;
}