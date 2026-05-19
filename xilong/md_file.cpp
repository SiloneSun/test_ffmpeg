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
#include <SDL2/SDL.h>           // SDL 窗口/渲染
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
    // 2. 初始化 SDL + TTF
    // ============================================================
    const int PANEL_W = 320;
    int win_w = vid_w + PANEL_W;
    int win_h = (vid_h > 480) ? vid_h : 480;

    SDL_Window   *sdl_win   = nullptr;
    SDL_Renderer *sdl_ren   = nullptr;
    SDL_Texture  *sdl_tex   = nullptr;
    TTF_Font     *sdl_font  = nullptr;
    bool          sdl_ok    = false;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        LOGD("SDL_Init failed: %s", SDL_GetError());
    } else if (TTF_Init() < 0) {
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
                                                vid_w, vid_h);
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
    // 3. 解码主循环 + 显示
    // ============================================================
    md_packet pkt;
    int m_width = 0;
    int m_height = 0;
    uint32_t last_ticks = SDL_GetTicks();
    bool quit = false;

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

                    // 渲染左半：视频画面
                    SDL_UpdateYUVTexture(sdl_tex, NULL,
                                         frame->data[0], frame->linesize[0],
                                         frame->data[1], frame->linesize[1],
                                         frame->data[2], frame->linesize[2]);

                    // 清空整个窗口为黑色
                    SDL_SetRenderDrawColor(sdl_ren, 0, 0, 0, 255);
                    SDL_RenderClear(sdl_ren);

                    // 左边渲染视频（保持比例，居中显示在左侧区域）
                    SDL_Rect vid_rect;
                    double scale = (double)vid_h / win_h;
                    int draw_w = vid_w / scale;
                    int draw_h = win_h;
                    if (draw_w > vid_w) {
                        draw_w = vid_w;
                        draw_h = vid_h;
                    }
                    vid_rect.x = 0;
                    vid_rect.y = (win_h - draw_h) / 2;
                    vid_rect.w = draw_w < vid_w ? draw_w : vid_w;
                    vid_rect.h = draw_h;
                    SDL_RenderCopy(sdl_ren, sdl_tex, NULL, &vid_rect);

                    // 右边：统计面板（深色背景）
                    SDL_Rect panel_rect = { vid_w, 0, win_w - vid_w, win_h };
                    SDL_SetRenderDrawColor(sdl_ren, 30, 30, 40, 255);
                    SDL_RenderFillRect(sdl_ren, &panel_rect);

                    // 绘制分隔线
                    SDL_SetRenderDrawColor(sdl_ren, 80, 80, 100, 255);
                    SDL_RenderDrawLine(sdl_ren, vid_w, 0, vid_w, win_h);

                    // 文字颜色
                    SDL_Color white  = { 220, 220, 220, 255 };
                    SDL_Color yellow = { 255, 220, 80,  255 };
                    SDL_Color green  = { 100, 255, 100, 255 };

                    int px = vid_w + 10;  // 面板左边缘
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
                        snprintf(buf, sizeof(buf), "Size: %dx%d", vid_w, vid_h);
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
                        // 视频流的平均码率（来自文件头信息）
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
                // 不打印音频帧信息，减少日志
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
        }
    }

    // ============================================================
    // 5. 清理 SDL
    // ============================================================
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