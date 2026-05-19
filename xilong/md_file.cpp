#include "md_file.h"
#include "t_log.h"
#include "json11.hpp"
#include <iostream>
#include <string>
#include <ostream>
#include <libavutil/log.h>
#include "utils.h"

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

std::int16_t md_file::play()
{
    // ========== 使用 RAII 类型别名 ==========
    // md_codec_ctx 自动 avcodec_alloc_context3(nullptr)
    // md_frame      自动 av_frame_alloc()
    // md_packet     自动 av_packet_alloc()
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

        // decCtx 已在构造时分配，只需复制参数并打开解码器
        if (avcodec_parameters_to_context(decCtx.get(), m_fmtCtx->streams[video_stream_index]->codecpar) < 0) {
            LOGD("avcodec_parameters_to_context failed");
            return -1;
        }

        if (avcodec_open2(decCtx.get(), decoder, nullptr) < 0) {
            LOGD("avcodec_open2 failed");
            return -1;
        }
    }

    if(audio_stream_index >= 0)
    {
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

    md_packet pkt;
    int m_width = 0;
    int m_height = 0;

    while(av_read_frame(m_fmtCtx.get(), pkt.get()) >= 0){
        if(pkt->stream_index == video_stream_index)
        {
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
            if (ret == 0)
            {
                if ( m_width != frame->width || m_height != frame->height)
                {
                    LOGD("decoded frame: width=%d, height=%d, format=%d", frame->width, frame->height, frame->format);
                    m_width = frame->width;
                    m_height = frame->height;
                }
                LOGD("test decoded frame: width=%d, height=%d, format=%d", frame->width, frame->height, frame->format);
            } else if (ret == AVERROR(EAGAIN)) {
                // 需要更多数据才能解码出一帧，这是正常情况
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
            }
        }else if (pkt->stream_index == audio_stream_index)
        {
            audio_pkt_count++;
            if (avcodec_send_packet(audio_dec_ctx.get(), pkt.get()) < 0) {
                LOGD("avcodec_send_packet failed");
                av_packet_unref(pkt.get());
                continue;
            }
            int ret = avcodec_receive_frame(audio_dec_ctx.get(), audio_frame.get());
            if (ret == 0)
            {
                LOGD("decode audio frame: format=%d, sample_rate=%d, nb_samples=%d, channels=%d",
                     audio_frame->format, audio_frame->sample_rate,
                     audio_frame->nb_samples, audio_frame->ch_layout.nb_channels);
            } else if (ret == AVERROR(EAGAIN)) {
                // 需要更多数据才能解码出一帧，这是正常情况
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
            }
        }else if (pkt->stream_index == subtitle_stream_index)
        {
            // subtitle packet
        }else{
            LOGD("other packet");
        }

        av_packet_unref(pkt.get());
    }

    // 冲刷视频解码器：发送 NULL 让解码器输出缓冲的剩余帧
    if (video_stream_index >= 0) {
        avcodec_send_packet(decCtx.get(), nullptr);
        while (avcodec_receive_frame(decCtx.get(), frame.get()) == 0) {
            LOGD("flush decoded frame: width=%d, height=%d", frame->width, frame->height);
        }
    }

    // 冲刷音频解码器
    if (audio_stream_index >= 0) {
        avcodec_send_packet(audio_dec_ctx.get(), nullptr);
        while (avcodec_receive_frame(audio_dec_ctx.get(), audio_frame.get()) == 0) {
            LOGD("flush audio frame: format=%d, nb_samples=%d",
                 audio_frame->format, audio_frame->nb_samples);
        }
    }

    // decCtx, audio_dec_ctx, frame, audio_frame, pkt 离开作用域时自动释放
    return 0;
}

bool md_file::end_of_file()
{
    return false;
}