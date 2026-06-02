#include "md_file.h"
#include "t_log.h"
#include "json11.hpp"
#include <iostream>
#include <string>
#include <ostream>
#include <cstring>
#include <utility>
#include <vector>
#include <libavutil/log.h>
#include "utils.h"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
}

using namespace sunxilong;

// ── 在视频帧实际显示后记录时间，并与音频实际播放位置做对比 ──
// 使用墙钟时间域比较：render_wall_us 是 av_gettime_relative() 值，
// audio 侧也通过时钟基准 (m_audio_clock_ref_{wall,pts}) 映射到墙钟，
// 故 av_diff 反映真实的音画显示时间差（而非 PTS 理论值）。
void md_file::record_video_play_pts(int64_t pts_us, int64_t render_wall_us, uint32_t audio_dev_id)
{
    std::lock_guard<std::mutex> lock(m_ts_mtx);
    m_last_video_pts_us = pts_us;

    if (audio_dev_id == 0 || m_audio_total_pushed_bytes == 0 || m_audio_sr <= 0 || m_audio_channels <= 0)
        return;
    if (m_audio_clock_ref_pts == AV_NOPTS_VALUE || m_audio_clock_ref_wall == 0)
        return;

    Uint32 queued = SDL_GetQueuedAudioSize(audio_dev_id);
    if (m_audio_total_pushed_bytes < queued)
        return;

    // 计算队列中剩余数据对应的时间（微秒）
    // S16: 每样本 2 字节 × channels
    uint64_t queued_bytes = queued;
    int64_t queued_us = (int64_t)(queued_bytes * 1000000LL / (m_audio_channels * 2 * m_audio_sr));

    // 当前音频实际播放位置（媒体时间 PTS 域）
    int64_t actual_audio_pts_us = (int64_t)m_audio_source_pts_us - queued_us;

    // 将音频位置映射到墙钟时间域（统一时间基准，消除 PTS 与实时之间的偏移）
    int64_t audio_wall_us = m_audio_clock_ref_wall + (actual_audio_pts_us - m_audio_clock_ref_pts);

    int64_t av_diff = render_wall_us - audio_wall_us;

    // 滑动平均：消除单帧音频队列离散采样带来的锯齿伪影
    constexpr int FILTER_TAPS = 5;
    static int64_t diff_history[FILTER_TAPS] = {};
    static int diff_idx = 0;
    diff_history[diff_idx % FILTER_TAPS] = av_diff;
    diff_idx++;
    int64_t smoothed_diff = av_diff;
    if (diff_idx >= FILTER_TAPS) {
        int64_t sum = 0;
        for (int i = 0; i < FILTER_TAPS; i++) sum += diff_history[i];
        smoothed_diff = sum / FILTER_TAPS;
    }

    // LOGD("[sync] video-audio diff: %6ld us (%5.1f ms) smoothed=%6ld us (%5.1f ms) queue=%u bytes (%ld us) rw=%ld",
    //      (long)av_diff, av_diff / 1000.0,
    //      (long)smoothed_diff, smoothed_diff / 1000.0,
    //      queued, (long)queued_us, (long)(render_wall_us / 1000));
}

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
    m_fmtCtx(empty_obj_t{})
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
    return 0;
}

std::int16_t md_file::deinit()
{
    return 0;
}

std::int16_t md_file::open_file()
{
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
    if (avformat_find_stream_info(m_fmtCtx.get(), nullptr) < 0) {
        LOGD("get stream info failed");
        m_fmtCtx.reset();
        return -1;
    }

    LOGD("open file: %s", m_file_name.c_str());
    LOGD("file path: %s open success", m_fmtCtx->url);
    LOGD("file duration: %llds", (long long)(m_fmtCtx->duration / AV_TIME_BASE));

    for (unsigned int i = 0; i < m_fmtCtx->nb_streams; i++) {
        AVStream* stream = m_fmtCtx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            video_stream_index = i;
        else if(stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audio_stream_index = i;
        else if(stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            subtitle_stream_index = i;

        const char* type_str = media_type_str(stream->codecpar->codec_type);
        const char* codec_name = avcodec_get_name(stream->codecpar->codec_id);
        LOGD("stream[%d]: %s (%s)", stream->index, type_str, codec_name);
        LOGD("nb_frames: %lld", (long long)stream->nb_frames);
    }

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

    m_fmtCtx.reset();
    video_stream_index = -1;
    audio_stream_index = -1;
    subtitle_stream_index = -1;
    video_pkt_count = 0;
    audio_pkt_count = 0;
    keyframe_count = 0;
    non_keyframe_count = 0;
    return 0;
}

static void save_rgb_to_bmp(const char *filename, const uint8_t *rgb_data, int w, int h)
{
    int row_size = (w * 3 + 3) & ~3;
    int data_size = row_size * h;
    int file_size = 14 + 40 + data_size;

    FILE *f = fopen(filename, "wb");
    if (!f) return;

    uint8_t header[14] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size>>8),
        (uint8_t)(file_size>>16), (uint8_t)(file_size>>24),
        0,0, 0,0,
        14+40,0,0,0
    };
    fwrite(header, 1, 14, f);

    uint8_t dib[40] = {
        40,0,0,0,
        (uint8_t)w, (uint8_t)(w>>8), (uint8_t)(w>>16), (uint8_t)(w>>24),
        (uint8_t)h, (uint8_t)(h>>8), (uint8_t)(h>>16), (uint8_t)(h>>24),
        1,0, 24,0,
        0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0
    };
    fwrite(dib, 1, 40, f);

    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *row = rgb_data + y * w * 3;
        for (int x = 0; x < w; x++) {
            uint8_t r = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            fputc(b, f); fputc(g, f); fputc(r, f);
        }
        for (int p = w * 3; p < row_size; p++) fputc(0, f);
    }
    fclose(f);
    LOGD("snapshot saved: %s (%dx%d)", filename, w, h);
}

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
    return y + h + 2;
}

struct cached_video_frame {
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    int64_t pts_us = AV_NOPTS_VALUE;
    int frame_no = 0;
    std::string frame_type;
    bool key_frame = false;
};

struct video_frame_index {
    int frame_no = 0;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t pts_us = AV_NOPTS_VALUE;
    int64_t pkt_pos = -1;
    std::string frame_type;
    bool key_frame = false;
};

static const char *picture_type_str(AVPictureType pict_type, bool key_frame)
{
    switch (pict_type) {
    case AV_PICTURE_TYPE_I:
        return key_frame ? "IDR" : "I";
    case AV_PICTURE_TYPE_P:
        return "P";
    case AV_PICTURE_TYPE_B:
        return "B";
    case AV_PICTURE_TYPE_S:
        return "S";
    case AV_PICTURE_TYPE_SI:
        return "SI";
    case AV_PICTURE_TYPE_SP:
        return "SP";
    case AV_PICTURE_TYPE_BI:
        return "BI";
    default:
        return "?";
    }
}

std::int16_t md_file::play(bool loop)
{
    md_codec_ctx decCtx;
    md_codec_ctx audio_dec_ctx;
    md_frame frame;
    md_frame audio_frame;
    SDL_AudioDeviceID audio_dev_id = 0;

    if (video_stream_index >= 0) {
        const AVCodec *decoder = avcodec_find_decoder(m_fmtCtx->streams[video_stream_index]->codecpar->codec_id);
        if (!decoder) { LOGD("avcodec_find_decoder failed"); return -1; }
        LOGD("find decoder: %s", decoder->name);
        if (avcodec_parameters_to_context(decCtx.get(), m_fmtCtx->streams[video_stream_index]->codecpar) < 0) { LOGD("avcodec_parameters_to_context failed"); return -1; }
        if (avcodec_open2(decCtx.get(), decoder, nullptr) < 0) { LOGD("avcodec_open2 failed"); return -1; }
        LOGD("video time_base: stream=%d/%d  codec=%d/%d",
             m_fmtCtx->streams[video_stream_index]->time_base.num,
             m_fmtCtx->streams[video_stream_index]->time_base.den,
             decCtx->time_base.num, decCtx->time_base.den);
    }

    bool sdl_audio_ok = false;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        LOGD("SDL_Init failed: %s", SDL_GetError());
    } else {
        sdl_audio_ok = true;
    }

    // ── 音频解码器 + 重采样 + SDL 音频设备 ──
    SwrContext *swr_ctx = nullptr;
    int audio_sr = 0;
    int audio_ch = 0;
    AVSampleFormat audio_dst_fmt = AV_SAMPLE_FMT_S16;
    int audio_dst_sample_rate = 0;
    int audio_dst_channels = 0;

    m_audio_total_pushed_bytes = 0;
    m_audio_source_pts_us = AV_NOPTS_VALUE;
    m_audio_clock_ref_wall = 0;
    m_audio_clock_ref_pts = AV_NOPTS_VALUE;

    if (audio_stream_index >= 0) {
        const AVCodec *decoder = avcodec_find_decoder(m_fmtCtx->streams[audio_stream_index]->codecpar->codec_id);
        if (!decoder) { LOGD("avcodec_find_decoder failed"); return -1; }
        LOGD("find decoder: %s", decoder->name);
        if (avcodec_parameters_to_context(audio_dec_ctx.get(), m_fmtCtx->streams[audio_stream_index]->codecpar) < 0) { LOGD("avcodec_parameters_to_context failed"); return -1; }
        if (avcodec_open2(audio_dec_ctx.get(), decoder, nullptr) < 0) { LOGD("avcodec_open2 failed"); return -1; }
        LOGD("audio time_base: stream=%d/%d  codec=%d/%d",
             m_fmtCtx->streams[audio_stream_index]->time_base.num,
             m_fmtCtx->streams[audio_stream_index]->time_base.den,
             audio_dec_ctx->time_base.num, audio_dec_ctx->time_base.den);

        audio_sr = audio_dec_ctx->sample_rate;
        audio_ch = audio_dec_ctx->ch_layout.nb_channels;
        audio_dst_sample_rate = audio_sr;
        audio_dst_channels = audio_ch;
        m_audio_sr = audio_sr;
        m_audio_channels = audio_ch;

        AVChannelLayout src_ch_layout = audio_dec_ctx->ch_layout;
        AVChannelLayout dst_ch_layout;
        av_channel_layout_default(&dst_ch_layout, audio_dst_channels);

        swr_ctx = swr_alloc();
        if (!swr_ctx) { LOGD("swr_alloc failed"); }
        else {
            av_opt_set_chlayout(swr_ctx, "in_chlayout",  &src_ch_layout, 0);
            av_opt_set_int(swr_ctx, "in_sample_rate",  audio_sr, 0);
            av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",  audio_dec_ctx->sample_fmt, 0);
            av_opt_set_chlayout(swr_ctx, "out_chlayout", &dst_ch_layout, 0);
            av_opt_set_int(swr_ctx, "out_sample_rate", audio_dst_sample_rate, 0);
            av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", audio_dst_fmt, 0);
            if (swr_init(swr_ctx) < 0) { LOGD("swr_init failed"); swr_free(&swr_ctx); swr_ctx = nullptr; }
            else LOGD("swr_init ok: sr=%d ch=%d", audio_sr, audio_ch);
        }

        if (sdl_audio_ok) {
            SDL_AudioSpec desired, obtained;
            SDL_zero(desired);
            desired.freq     = audio_dst_sample_rate;
            desired.format   = AUDIO_S16SYS;
            desired.channels = audio_dst_channels;
            desired.samples  = 4096;
            desired.callback = NULL;

            audio_dev_id = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
            if (audio_dev_id == 0) LOGD("SDL_OpenAudioDevice failed: %s", SDL_GetError());
            else {
                LOGD("SDL audio device opened: freq=%d, fmt=%d, ch=%d", obtained.freq, obtained.format, obtained.channels);
                SDL_PauseAudioDevice(audio_dev_id, 0);
            }
        }
    }

    // ── 视频参数 ──
    int vid_w = 0, vid_h = 0;
    int frame_delay_ms = 40;
    double fps = 25.0;
    const char *codec_name = "unknown";

    if (video_stream_index >= 0) {
        vid_w = m_fmtCtx->streams[video_stream_index]->codecpar->width;
        vid_h = m_fmtCtx->streams[video_stream_index]->codecpar->height;
        AVRational rate = m_fmtCtx->streams[video_stream_index]->avg_frame_rate;
        if (rate.num == 0 || rate.den == 0)
            rate = m_fmtCtx->streams[video_stream_index]->r_frame_rate;
        frame_delay_ms = av_rescale_q(1, av_inv_q(rate), (AVRational){1, 1000});
        fps = (double)rate.num / rate.den;
        const AVCodec *vc = avcodec_find_decoder(m_fmtCtx->streams[video_stream_index]->codecpar->codec_id);
        codec_name = vc ? vc->name : "unknown";
    }

    // ── SDL 窗口 ──
    const int OUT_W = 1280, OUT_H = 720, PANEL_W = 320;
    int win_w = OUT_W + PANEL_W, win_h = (OUT_H > 480) ? OUT_H : 480;
    SDL_Window   *sdl_win   = nullptr;
    SDL_Renderer *sdl_ren   = nullptr;
    SDL_Texture  *sdl_tex   = nullptr;
    TTF_Font     *sdl_font  = nullptr;
    bool          sdl_ok    = false;

    if (TTF_Init() < 0) { LOGD("TTF_Init failed: %s", TTF_GetError()); SDL_Quit(); }
    else {
        sdl_font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16);
        if (!sdl_font) { LOGD("TTF_OpenFont failed: %s", TTF_GetError()); TTF_Quit(); SDL_Quit(); }
        else {
            sdl_win = SDL_CreateWindow("FFmpeg Player - [sxl]", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
            if (!sdl_win) { LOGD("SDL_CreateWindow failed: %s", SDL_GetError()); TTF_CloseFont(sdl_font); TTF_Quit(); SDL_Quit(); }
            else {
                sdl_ren = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_ACCELERATED);
                if (!sdl_ren) { LOGD("SDL_CreateRenderer failed: %s", SDL_GetError()); SDL_DestroyWindow(sdl_win); TTF_CloseFont(sdl_font); TTF_Quit(); SDL_Quit(); }
                else {
                    sdl_tex = SDL_CreateTexture(sdl_ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, OUT_W, OUT_H);
                    if (!sdl_tex) { LOGD("SDL_CreateTexture failed: %s", SDL_GetError()); SDL_DestroyRenderer(sdl_ren); SDL_DestroyWindow(sdl_win); TTF_CloseFont(sdl_font); TTF_Quit(); SDL_Quit(); }
                    else sdl_ok = true;
                }
            }
        }
    }
    if (!sdl_ok) LOGD("SDL/TTF init failed, will decode without display");

    // ── swscale ──
    struct SwsContext *sws_ctx = sws_getContext(vid_w, vid_h, AV_PIX_FMT_YUV420P, OUT_W, OUT_H, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) LOGD("sws_getContext failed");

    uint8_t *scale_yuv_data[4] = {NULL};
    int scale_yuv_linesize[4] = {0};
    av_image_alloc(scale_yuv_data, scale_yuv_linesize, OUT_W, OUT_H, AV_PIX_FMT_YUV420P, 1);

    struct SwsContext *sws_rgb_ctx = sws_getContext(vid_w, vid_h, AV_PIX_FMT_YUV420P, OUT_W, OUT_H, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t *scale_rgb_data[4] = {NULL};
    int scale_rgb_linesize[4] = {0};
    av_image_alloc(scale_rgb_data, scale_rgb_linesize, OUT_W, OUT_H, AV_PIX_FMT_RGB24, 1);

    // ── 主循环 ──
    md_packet pkt;
    int m_width = 0, m_height = 0;
    uint32_t last_ticks = SDL_GetTicks();
    bool quit = false;
    uint32_t fps_frame_count = 0, fps_last_tick = SDL_GetTicks();
    double realtime_fps = 0.0;
    int decoded_video_frame_count = 0;
    bool paused = false;
    bool step_next_frame = false;
    bool hold_prev_frame = false;
    bool hold_next_frame = false;
    uint32_t hold_next_tick = 0;
    std::vector<cached_video_frame> frame_cache;
    std::vector<video_frame_index> frame_index_list;
    int frame_cache_start = 0;
    int frame_cache_count = 0;
    int frame_cache_index = -1;
    const int MAX_CACHED_FRAMES = 45;
    bool snapshot_saved = false;
    const bool DEBUG_FRAME_LOGS = false;

    frame_cache.resize(MAX_CACHED_FRAMES);

    video_pkt_count = 0; audio_pkt_count = 0; keyframe_count = 0; non_keyframe_count = 0;

    auto copy_plane = [](std::vector<uint8_t>& dst, const uint8_t *src, int src_linesize, int w, int h) {
        dst.resize(w * h);
        for (int y = 0; y < h; y++)
            memcpy(dst.data() + y * w, src + y * src_linesize, w);
    };

    auto cache_ref = [&](int index) -> cached_video_frame& {
        int pos = (frame_cache_start + index) % MAX_CACHED_FRAMES;
        return frame_cache[pos];
    };

    auto cache_current_frame = [&](int64_t pts_us, int frame_no, const char *frame_type, bool key_frame) {
        if (!scale_yuv_data[0])
            return;

        if (frame_cache_index + 1 < frame_cache_count)
            frame_cache_count = frame_cache_index + 1;

        int write_index = 0;
        if (frame_cache_count < MAX_CACHED_FRAMES) {
            write_index = (frame_cache_start + frame_cache_count) % MAX_CACHED_FRAMES;
            frame_cache_count++;
        } else {
            write_index = frame_cache_start;
            frame_cache_start = (frame_cache_start + 1) % MAX_CACHED_FRAMES;
        }

        cached_video_frame& cached = frame_cache[write_index];
        copy_plane(cached.y, scale_yuv_data[0], scale_yuv_linesize[0], OUT_W, OUT_H);
        copy_plane(cached.u, scale_yuv_data[1], scale_yuv_linesize[1], OUT_W / 2, OUT_H / 2);
        copy_plane(cached.v, scale_yuv_data[2], scale_yuv_linesize[2], OUT_W / 2, OUT_H / 2);
        cached.pts_us = pts_us;
        cached.frame_no = frame_no;
        cached.frame_type = frame_type;
        cached.key_frame = key_frame;

        frame_cache_index = frame_cache_count - 1;
    };

    auto find_index_by_frame_no = [&](int frame_no) -> video_frame_index* {
        if (frame_no <= 0 || frame_no > (int)frame_index_list.size())
            return nullptr;
        if (frame_index_list[frame_no - 1].frame_no != frame_no)
            return nullptr;
        return &frame_index_list[frame_no - 1];
    };

    auto find_index_by_pts = [&](int64_t pts) -> video_frame_index* {
        if (pts == AV_NOPTS_VALUE)
            return nullptr;
        for (auto& item : frame_index_list) {
            if (item.pts == pts)
                return &item;
        }
        return nullptr;
    };

    auto record_frame_index = [&](int frame_no, int64_t pts, int64_t pts_us,
                                  int64_t pkt_pos, const char *frame_type,
                                  bool key_frame) {
        if (frame_no <= 0)
            return;
        if ((int)frame_index_list.size() < frame_no)
            frame_index_list.resize(frame_no);

        video_frame_index& item = frame_index_list[frame_no - 1];
        item.frame_no = frame_no;
        item.pts = pts;
        item.pts_us = pts_us;
        item.pkt_pos = pkt_pos;
        item.frame_type = frame_type;
        item.key_frame = key_frame;
    };

    auto render_cached_frame = [&](const cached_video_frame& cached, bool count_sync) {
        if (!sdl_ok)
            return;

        SDL_UpdateYUVTexture(sdl_tex, NULL,
                             cached.y.data(), OUT_W,
                             cached.u.data(), OUT_W / 2,
                             cached.v.data(), OUT_W / 2);

        SDL_SetRenderDrawColor(sdl_ren, 0, 0, 0, 255);
        SDL_RenderClear(sdl_ren);
        SDL_Rect vid_rect = { 0, 0, OUT_W, OUT_H };
        SDL_RenderCopy(sdl_ren, sdl_tex, NULL, &vid_rect);

        if (count_sync)
            record_video_play_pts(cached.pts_us, av_gettime_relative(), audio_dev_id);

        char buf[256];
        SDL_Color white = {220,220,220,255}, yellow = {255,220,80,255}, green = {100,255,100,255};
        SDL_SetRenderDrawBlendMode(sdl_ren, SDL_BLENDMODE_BLEND);
        SDL_Rect overlay_rect = { 12, 12, 360, 82 };
        SDL_SetRenderDrawColor(sdl_ren, 0, 0, 0, 170);
        SDL_RenderFillRect(sdl_ren, &overlay_rect);
        int oy = 20;
        snprintf(buf, sizeof(buf), "Frame: #%d  Type: %s%s",
                 cached.frame_no,
                 cached.frame_type.c_str(),
                 cached.key_frame ? "  KEY" : "");
        oy = render_text_line(sdl_ren, sdl_font, 24, oy, buf, yellow);
        if (cached.pts_us == AV_NOPTS_VALUE)
            snprintf(buf, sizeof(buf), "PTS: NOPTS");
        else
            snprintf(buf, sizeof(buf), "PTS: %lld us  %.3f s",
                     (long long)cached.pts_us, cached.pts_us / 1000000.0);
        render_text_line(sdl_ren, sdl_font, 24, oy, buf, white);
        SDL_SetRenderDrawBlendMode(sdl_ren, SDL_BLENDMODE_NONE);

        // ── 右侧面板 ──
        SDL_Rect panel_rect = { OUT_W, 0, win_w - OUT_W, win_h };
        SDL_SetRenderDrawColor(sdl_ren, 30, 30, 40, 255);
        SDL_RenderFillRect(sdl_ren, &panel_rect);
        SDL_SetRenderDrawColor(sdl_ren, 80, 80, 100, 255);
        SDL_RenderDrawLine(sdl_ren, OUT_W, 0, OUT_W, win_h);

        int px = OUT_W + 10, py = 10;

        py = render_text_line(sdl_ren, sdl_font, px, py, "[ File Info ]", yellow);
        snprintf(buf, sizeof(buf), "Name: %s", m_file_name.c_str()); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "Size: %dx%d", vid_w, vid_h); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "Codec: %s", codec_name); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "FPS(target): %.2f", fps); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "Duration: %llds", (long long)(m_fmtCtx->duration / AV_TIME_BASE)); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);

        py += 8;
        py = render_text_line(sdl_ren, sdl_font, px, py, "[ Real-time Stats ]", yellow);
        snprintf(buf, sizeof(buf), "FPS(now): %.1f", realtime_fps); py = render_text_line(sdl_ren, sdl_font, px, py, buf, green);
        snprintf(buf, sizeof(buf), "Video Pkts: %d", video_pkt_count); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "Audio Pkts: %d", audio_pkt_count); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "Keyframes: %d", keyframe_count); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "State: %s", paused ? "paused" : "playing"); py = render_text_line(sdl_ren, sdl_font, px, py, buf, yellow);

        py += 8;
        py = render_text_line(sdl_ren, sdl_font, px, py, "[ Current Frame ]", yellow);
        snprintf(buf, sizeof(buf), "Frame No: %d", cached.frame_no); py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "Cache Pos: %d/%d",
                 frame_cache_index + 1, frame_cache_count);
        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        snprintf(buf, sizeof(buf), "Type: %s%s",
                 cached.frame_type.c_str(), cached.key_frame ? " (KEY)" : "");
        py = render_text_line(sdl_ren, sdl_font, px, py, buf, green);
        if (cached.pts_us == AV_NOPTS_VALUE)
            snprintf(buf, sizeof(buf), "PTS: NOPTS");
        else
            snprintf(buf, sizeof(buf), "PTS: %lld us", (long long)cached.pts_us);
        py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        if (cached.pts_us != AV_NOPTS_VALUE) {
            snprintf(buf, sizeof(buf), "PTS(sec): %.3f", cached.pts_us / 1000000.0);
            py = render_text_line(sdl_ren, sdl_font, px, py, buf, white);
        }

        SDL_RenderPresent(sdl_ren);
    };

    auto rebuild_cache_to_frame = [&](int target_frame_no) -> bool {
        video_frame_index *target_index = find_index_by_frame_no(target_frame_no);
        if (!target_index || target_index->pts == AV_NOPTS_VALUE)
            return false;

        video_frame_index *seek_index = target_index;
        for (int i = target_frame_no; i >= 1; i--) {
            video_frame_index *candidate = find_index_by_frame_no(i);
            if (candidate && candidate->key_frame && candidate->pts != AV_NOPTS_VALUE) {
                seek_index = candidate;
                break;
            }
        }

        if (av_seek_frame(m_fmtCtx.get(), video_stream_index, seek_index->pts, AVSEEK_FLAG_BACKWARD) < 0) {
            LOGD("seek failed: target frame=%d pts=%lld", target_frame_no, (long long)seek_index->pts);
            return false;
        }

        avcodec_flush_buffers(decCtx.get());
        if (audio_stream_index >= 0)
            avcodec_flush_buffers(audio_dec_ctx.get());
        if (audio_dev_id > 0)
            SDL_ClearQueuedAudio(audio_dev_id);
        m_audio_total_pushed_bytes = 0;
        m_audio_source_pts_us = AV_NOPTS_VALUE;
        m_audio_clock_ref_wall = 0;
        m_audio_clock_ref_pts = AV_NOPTS_VALUE;

        frame_cache_start = 0;
        frame_cache_count = 0;
        frame_cache_index = -1;

        md_packet seek_pkt;
        md_frame seek_frame;
        int fallback_frame_no = seek_index->frame_no - 1;

        while (av_read_frame(m_fmtCtx.get(), seek_pkt.get()) >= 0) {
            if (seek_pkt->stream_index != video_stream_index) {
                av_packet_unref(seek_pkt.get());
                continue;
            }

            if (avcodec_send_packet(decCtx.get(), seek_pkt.get()) < 0) {
                av_packet_unref(seek_pkt.get());
                continue;
            }
            av_packet_unref(seek_pkt.get());

            while (true) {
                int ret = avcodec_receive_frame(decCtx.get(), seek_frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    return false;

                bool frame_is_key = (seek_frame->flags & AV_FRAME_FLAG_KEY);
                const char *frame_type = picture_type_str(seek_frame->pict_type, frame_is_key);
                int64_t frame_pts = seek_frame->pts;
                int64_t frame_pts_us = frame_pts != AV_NOPTS_VALUE
                    ? av_rescale_q(frame_pts, m_fmtCtx->streams[video_stream_index]->time_base, (AVRational){1, 1000000})
                    : AV_NOPTS_VALUE;

                video_frame_index *known = find_index_by_pts(frame_pts);
                int frame_no = known ? known->frame_no : ++fallback_frame_no;
                if (!known)
                    record_frame_index(frame_no, frame_pts, frame_pts_us, -1, frame_type, frame_is_key);

                if (sws_ctx && scale_yuv_data[0])
                    sws_scale(sws_ctx, seek_frame->data, seek_frame->linesize, 0, vid_h, scale_yuv_data, scale_yuv_linesize);
                cache_current_frame(frame_pts_us, frame_no, frame_type, frame_is_key);

                if (frame_no >= target_frame_no) {
                    decoded_video_frame_count = frame_no;
                    if (frame_cache_index >= 0)
                        render_cached_frame(cache_ref(frame_cache_index), false);
                    return true;
                }
            }
        }

        return false;
    };

    auto show_previous_frame = [&]() {
        if (frame_cache_index > 0) {
            frame_cache_index--;
            render_cached_frame(cache_ref(frame_cache_index), false);
            return;
        }
        if (frame_cache_count <= 0)
            return;

        int target_frame_no = cache_ref(0).frame_no - 1;
        if (target_frame_no > 0)
            rebuild_cache_to_frame(target_frame_no);
    };

    auto show_next_frame = [&]() {
        if (frame_cache_index + 1 < frame_cache_count) {
            frame_cache_index++;
            render_cached_frame(cache_ref(frame_cache_index), false);
        } else {
            step_next_frame = true;
        }
    };

    while (!quit) {
        if (sdl_ok) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    quit = true;
                } else if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        quit = true;
                    } else if (event.key.keysym.sym == SDLK_SPACE) {
                        paused = !paused;
                        step_next_frame = false;
                        hold_prev_frame = false;
                        hold_next_frame = false;
                        if (audio_dev_id > 0)
                            SDL_PauseAudioDevice(audio_dev_id, paused ? 1 : 0);
                        if (!paused)
                            last_ticks = SDL_GetTicks();
                        if (paused && frame_cache_index >= 0)
                            render_cached_frame(cache_ref(frame_cache_index), false);
                    } else if (paused && event.key.keysym.sym == SDLK_LEFT) {
                        show_previous_frame();
                    } else if (paused && event.key.keysym.sym == SDLK_RIGHT) {
                        show_next_frame();
                    }
                } else if (paused && event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        hold_prev_frame = true;
                        hold_next_frame = false;
                        hold_next_tick = SDL_GetTicks() + 120;
                        show_previous_frame();
                    } else if (event.button.button == SDL_BUTTON_RIGHT) {
                        hold_next_frame = true;
                        hold_prev_frame = false;
                        hold_next_tick = SDL_GetTicks() + 120;
                        show_next_frame();
                    }
                } else if (event.type == SDL_MOUSEBUTTONUP) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        hold_prev_frame = false;
                    } else if (event.button.button == SDL_BUTTON_RIGHT) {
                        hold_next_frame = false;
                    }
                }
            }
        }
        if (quit) break;
        if (paused && !step_next_frame && (hold_prev_frame || hold_next_frame)) {
            uint32_t now = SDL_GetTicks();
            if (now >= hold_next_tick) {
                if (hold_prev_frame)
                    show_previous_frame();
                else if (hold_next_frame)
                    show_next_frame();
                hold_next_tick = SDL_GetTicks() + 120;
            }
        }
        if (paused && !step_next_frame) {
            SDL_Delay(10);
            continue;
        }

        int read_ret = av_read_frame(m_fmtCtx.get(), pkt.get());
        if (read_ret < 0) {
            if(loop)
            {
                LOGD("end of file, seek to beginning for loop play");
                if (video_stream_index >= 0) avcodec_flush_buffers(decCtx.get());
                if (audio_stream_index >= 0) avcodec_flush_buffers(audio_dec_ctx.get());
                av_seek_frame(m_fmtCtx.get(), -1, 0, AVSEEK_FLAG_BACKWARD);
                video_pkt_count = 0; audio_pkt_count = 0; keyframe_count = 0; non_keyframe_count = 0;
                decoded_video_frame_count = 0;
                frame_cache_start = 0;
                frame_cache_count = 0;
                frame_cache_index = -1;
                frame_index_list.clear();
                fps_frame_count = 0; fps_last_tick = SDL_GetTicks(); realtime_fps = 0.0;
                av_packet_unref(pkt.get());
                continue;
            }else{
                break;
            }

        }

        if (pkt->stream_index == video_stream_index) {
            int64_t video_pkt_pos = pkt->pos;
            video_pkt_count++;
            if (DEBUG_FRAME_LOGS)
                LOGD("vp_c=%d, kf?=%d", video_pkt_count, pkt->flags & AV_PKT_FLAG_KEY);
            if (pkt->flags & AV_PKT_FLAG_KEY) keyframe_count++;
            else non_keyframe_count++;

            if (avcodec_send_packet(decCtx.get(), pkt.get()) < 0) {
                av_packet_unref(pkt.get()); continue;
            }

            int ret = avcodec_receive_frame(decCtx.get(), frame.get());
            if (ret == 0) {
                decoded_video_frame_count++;
                if (m_width != frame->width || m_height != frame->height) {
                    m_width = frame->width; m_height = frame->height;
                }
                bool frame_is_key = (frame->flags & AV_FRAME_FLAG_KEY);
                const char *frame_type = picture_type_str(frame->pict_type, frame_is_key);
                
                if (DEBUG_FRAME_LOGS) {
                    switch (frame->pict_type) {
                    case AV_PICTURE_TYPE_I: 
                        LOGD("video_pkt_count %d I-frame", video_pkt_count);
                        break;
                    case AV_PICTURE_TYPE_P:
                        LOGD("video_pkt_count %d P-frame", video_pkt_count);
                        break;
                    case AV_PICTURE_TYPE_B:
                        LOGD("video_pkt_count %d B-frame", video_pkt_count);
                        break;
                    case AV_PICTURE_TYPE_S:
                        LOGD("video_pkt_count %d S-frame", video_pkt_count);
                        break;
                    
                    case AV_PICTURE_TYPE_SI:
                        LOGD("video_pkt_count %d SI-frame", video_pkt_count);
                        break;
                    case AV_PICTURE_TYPE_SP:
                        LOGD("video_pkt_count %d SP-frame", video_pkt_count);
                        break;
                    case AV_PICTURE_TYPE_BI:
                        LOGD("video_pkt_count %d BI-frame", video_pkt_count);
                        break;
                    default:
                        LOGD("video_pkt_count %d Unknown frame type", video_pkt_count);
                    }
                }

                if (sws_ctx && scale_yuv_data[0])
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, vid_h, scale_yuv_data, scale_yuv_linesize);

                int64_t frame_pts_us = frame->pts != AV_NOPTS_VALUE
                    ? av_rescale_q(frame->pts, m_fmtCtx->streams[video_stream_index]->time_base, (AVRational){1, 1000000})
                    : AV_NOPTS_VALUE;
                record_frame_index(decoded_video_frame_count, frame->pts, frame_pts_us,
                                   video_pkt_pos, frame_type, frame_is_key);
                cache_current_frame(frame_pts_us, decoded_video_frame_count, frame_type, frame_is_key);

                if (!snapshot_saved && sws_rgb_ctx && scale_rgb_data[0]) {
                    sws_scale(sws_rgb_ctx, frame->data, frame->linesize, 0, vid_h, scale_rgb_data, scale_rgb_linesize);
                    save_rgb_to_bmp("snap_000.bmp", scale_rgb_data[0], OUT_W, OUT_H);
                    snapshot_saved = true;
                }

                // 帧率统计
                fps_frame_count++;
                uint32_t now_tick = SDL_GetTicks();
                if (now_tick - fps_last_tick >= 1000) {
                    realtime_fps = fps_frame_count * 1000.0 / (now_tick - fps_last_tick);
                    fps_frame_count = 0; fps_last_tick = now_tick;
                }

                // 显示
                if (sdl_ok) {
                    if (frame_cache_index >= 0)
                        render_cached_frame(cache_ref(frame_cache_index), !paused);
                    if (!paused) {
                        uint32_t now = SDL_GetTicks();
                        int elapsed = now - last_ticks;
                        if (elapsed < frame_delay_ms) {
                            SDL_Delay(frame_delay_ms - elapsed);
                            now = SDL_GetTicks();
                        }
                        last_ticks = now;
                    }
                }
                if (paused && step_next_frame)
                    step_next_frame = false;
            }else{
                LOGD("avcodec_receive_frame video failed: %d", ret);
                LOGD("is keyframe? %d", pkt->flags & AV_PKT_FLAG_KEY);
            }

        } else if (pkt->stream_index == audio_stream_index) {
            if (paused) {
                av_packet_unref(pkt.get());
                continue;
            }
            audio_pkt_count++;
            if (avcodec_send_packet(audio_dec_ctx.get(), pkt.get()) < 0) { av_packet_unref(pkt.get()); continue; }

            int ret = avcodec_receive_frame(audio_dec_ctx.get(), audio_frame.get());
            if (ret == 0) {
                if (swr_ctx && audio_dev_id > 0) {
                    int dst_nb_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
                    if (dst_nb_samples > 0) {
                        uint8_t *dst_data[8] = {NULL};
                        int dst_linesize[8] = {0};
                        av_samples_alloc(dst_data, dst_linesize, audio_dst_channels, dst_nb_samples, audio_dst_fmt, 0);
                        if (dst_data[0]) {
                            const uint8_t *in_data[8];
                            for (int i = 0; i < 8; i++) in_data[i] = audio_frame->data[i];

                            int actual_samples = swr_convert(swr_ctx, (uint8_t**)dst_data, dst_nb_samples, in_data, audio_frame->nb_samples);
                            if (actual_samples > 0) {
                                int out_size = av_samples_get_buffer_size(dst_linesize, audio_dst_channels, actual_samples, audio_dst_fmt, 0);
                                if (out_size > 0) {
                                    int max_queued = audio_dst_sample_rate * audio_dst_channels * 2 / 2;
                                    while (SDL_GetQueuedAudioSize(audio_dev_id) > (Uint32)max_queued) SDL_Delay(5);

                                    SDL_QueueAudio(audio_dev_id, dst_data[0], out_size);

                                    // 更新音频追踪信息
                                    m_audio_total_pushed_bytes += out_size;
                                    int64_t pts_us = audio_frame->pts != AV_NOPTS_VALUE
                                        ? av_rescale_q(audio_frame->pts, m_fmtCtx->streams[audio_stream_index]->time_base, (AVRational){1, 1000000})
                                        : AV_NOPTS_VALUE;
                                    if (pts_us != AV_NOPTS_VALUE) {
                                        m_audio_source_pts_us = pts_us;
                                        // 首次推送音频帧时，建立墙钟 ↔ PTS 时钟基准
                                        if (m_audio_clock_ref_pts == AV_NOPTS_VALUE) {
                                            m_audio_clock_ref_pts = pts_us;
                                            m_audio_clock_ref_wall = av_gettime_relative();
                                            LOGD("[sync] audio clock ref: pts=%ld us  wall=%ld us",
                                                 (long)pts_us, (long)(m_audio_clock_ref_wall));
                                        }
                                    }
                                }
                            }
                            av_freep(&dst_data[0]);
                        }
                    }
                }
            }
        }

        av_packet_unref(pkt.get());
    }

    // ── 冲刷 + 等待音频 ──
    if (!quit && video_stream_index >= 0) {
        avcodec_send_packet(decCtx.get(), nullptr);
        while (avcodec_receive_frame(decCtx.get(), frame.get()) == 0) {}
    }
    if (!quit && audio_stream_index >= 0) {
        avcodec_send_packet(audio_dec_ctx.get(), nullptr);
        while (avcodec_receive_frame(audio_dec_ctx.get(), audio_frame.get()) == 0) {
            if (swr_ctx && audio_dev_id > 0) {
                int dst_nb_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
                if (dst_nb_samples > 0) {
                    uint8_t *dst_data = nullptr;
                    av_samples_alloc(&dst_data, NULL, audio_dst_channels, dst_nb_samples, audio_dst_fmt, 0);
                    if (dst_data) {
                        int actual_samples = swr_convert(swr_ctx, &dst_data, dst_nb_samples, (const uint8_t**)audio_frame->data, audio_frame->nb_samples);
                        if (actual_samples > 0) {
                            int out_size = av_samples_get_buffer_size(NULL, audio_dst_channels, actual_samples, audio_dst_fmt, 0);
                            if (out_size > 0) {
                                SDL_QueueAudio(audio_dev_id, dst_data, out_size);
                                m_audio_total_pushed_bytes += out_size;
                            }
                        }
                        av_freep(&dst_data);
                    }
                }
            }
        }
    }

    if (audio_dev_id > 0) {
        uint32_t wait_start = SDL_GetTicks();
        while (SDL_GetQueuedAudioSize(audio_dev_id) > 0) {
            SDL_Delay(10);
            if (SDL_GetTicks() - wait_start > 3000) break;
        }
    }

    // ── 清理 ──
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (sws_rgb_ctx) sws_freeContext(sws_rgb_ctx);
    if (scale_yuv_data[0]) av_freep(&scale_yuv_data[0]);
    if (scale_rgb_data[0]) av_freep(&scale_rgb_data[0]);
    if (audio_dev_id > 0) SDL_CloseAudioDevice(audio_dev_id);
    if (swr_ctx) swr_free(&swr_ctx);
    if (sdl_tex) SDL_DestroyTexture(sdl_tex);
    if (sdl_ren) SDL_DestroyRenderer(sdl_ren);
    if (sdl_win) SDL_DestroyWindow(sdl_win);
    if (sdl_font) TTF_CloseFont(sdl_font);
    if (sdl_ok) { TTF_Quit(); SDL_Quit(); }
    return 0;
}

bool md_file::end_of_file() { return false; }
