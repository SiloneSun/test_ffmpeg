#include <iostream>

extern "C" {
    // 因为 FFmpeg 是用 C 语言编写的，所以在 C++ 中包含 FFmpeg 的头文件时，需要使用 extern "C" 来告诉编译器按照 C 的方式来链接这些函数。
#include "avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
}

#include <string>
#include "t_log.h"
#include <libavutil/log.h>

#include "json11.hpp"

// 将 AVRational 转为可读字符串
static std::string rational_str(AVRational r) {
    if (r.den == 0) return "0/0";
    return std::to_string(r.num) + "/" + std::to_string(r.den);
}

// 将 AVRational 转为浮点数
static double rational_to_double(AVRational r) {
    if (r.den == 0) return 0.0;
    return (double)r.num / (double)r.den;
}

// 将 int64_t 时间戳转为秒（小数）
static double ts_to_sec(int64_t ts, AVRational time_base) {
    if (ts == AV_NOPTS_VALUE) return -1.0;
    return (double)ts * (double)time_base.num / (double)time_base.den;
}

// 获取媒体类型字符串
static const char* media_type_str(enum AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_UNKNOWN:    return "unknown";
        case AVMEDIA_TYPE_VIDEO:      return "video";
        case AVMEDIA_TYPE_AUDIO:      return "audio";
        case AVMEDIA_TYPE_DATA:       return "data";
        case AVMEDIA_TYPE_SUBTITLE:   return "subtitle";
        case AVMEDIA_TYPE_ATTACHMENT: return "attachment";
        default:                      return "other";
    }
}

// 获取 field order 字符串
static const char* field_order_str(enum AVFieldOrder order) {
    switch (order) {
        case AV_FIELD_UNKNOWN:     return "unknown";
        case AV_FIELD_PROGRESSIVE: return "progressive";
        case AV_FIELD_TT:          return "top_field_first";
        case AV_FIELD_BB:          return "bottom_field_first";
        case AV_FIELD_TB:          return "top_bottom";
        case AV_FIELD_BT:          return "bottom_top";
        default:                   return "other";
    }
}

// 构建单个 AVStream 的 JSON 对象
static json11::Json build_stream_json(AVStream* stream, int stream_index) {
    AVCodecParameters* par = stream->codecpar;
    const char* codec_name = avcodec_get_name(par->codec_id);

    // ============ codecpar 层级 ============
    json11::Json::object codecpar_obj = {
        { "codec_type",      media_type_str(par->codec_type) },
        { "codec_id",        codec_name },
        { "codec_tag",       (int)par->codec_tag },
        { "extradata_size",  par->extradata_size },
        { "format",          par->format },
        { "bit_rate",        (double)par->bit_rate },
        { "bits_per_coded_sample", par->bits_per_coded_sample },
        { "bits_per_raw_sample",   par->bits_per_raw_sample },
        { "profile",         par->profile },
        { "level",           par->level },
    };

    // ---- 视频特有字段 ----
    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        codecpar_obj["width"]               = par->width;
        codecpar_obj["height"]              = par->height;
        codecpar_obj["sample_aspect_ratio"] = rational_str(par->sample_aspect_ratio);
        codecpar_obj["framerate"]           = rational_str(par->framerate);
        codecpar_obj["framerate_fps"]       = rational_to_double(par->framerate);
        codecpar_obj["field_order"]         = field_order_str(par->field_order);
        codecpar_obj["color_range"]         = par->color_range;
        codecpar_obj["color_primaries"]     = par->color_primaries;
        codecpar_obj["color_trc"]           = par->color_trc;
        codecpar_obj["color_space"]         = par->color_space;
        codecpar_obj["chroma_location"]     = par->chroma_location;
        codecpar_obj["video_delay"]         = par->video_delay;
    }

    // ---- 音频特有字段 ----
    if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        char ch_layout_buf[256] = {0};
        av_channel_layout_describe(&par->ch_layout, ch_layout_buf, sizeof(ch_layout_buf));
        codecpar_obj["ch_layout"]           = ch_layout_buf;
        codecpar_obj["nb_channels"]         = par->ch_layout.nb_channels;
        codecpar_obj["sample_rate"]         = par->sample_rate;
        codecpar_obj["block_align"]         = par->block_align;
        codecpar_obj["frame_size"]          = par->frame_size;
        codecpar_obj["initial_padding"]     = par->initial_padding;
        codecpar_obj["trailing_padding"]    = par->trailing_padding;
        codecpar_obj["seek_preroll"]        = par->seek_preroll;
    }

    // ============ AVStream 层级 ============
    json11::Json::object stream_obj = {
        { "index",           stream->index },
        { "id",              (int)stream->id },
        { "time_base",       rational_str(stream->time_base) },
        { "avg_frame_rate",  rational_str(stream->avg_frame_rate) },
        { "avg_frame_rate_fps", rational_to_double(stream->avg_frame_rate) },
        { "r_frame_rate",    rational_str(stream->r_frame_rate) },
        { "start_time_ts",   (double)stream->start_time },
        { "start_time_sec",  ts_to_sec(stream->start_time, stream->time_base) },
        { "duration_ts",     (double)stream->duration },
        { "duration_sec",    ts_to_sec(stream->duration, stream->time_base) },
        { "nb_frames",       (double)stream->nb_frames },
        { "disposition",     stream->disposition },
        { "sample_aspect_ratio", rational_str(stream->sample_aspect_ratio) },
        { "event_flags",     stream->event_flags },
        { "pts_wrap_bits",   stream->pts_wrap_bits },
        { "codecpar",        codecpar_obj },
    };

    return json11::Json(stream_obj);
}

static std::string pretty_print_json(const json11::Json& json, int indent = 0) {
    const std::string indent_str(4 * indent, ' ');

    switch (json.type()) {
        case json11::Json::NUL:
            return "null";
        case json11::Json::NUMBER:
            return json.dump();
        case json11::Json::BOOL:
            return json.bool_value() ? "true" : "false";
        case json11::Json::STRING:
            return json.dump();
        case json11::Json::ARRAY: {
            const auto& items = json.array_items();
            if (items.empty()) return "[]";
            std::string out = "[\n";
            for (size_t i = 0; i < items.size(); i++) {
                out += std::string(4 * (indent + 1), ' ') + pretty_print_json(items[i], indent + 1);
                if (i < items.size() - 1) out += ",";
                out += "\n";
            }
            out += indent_str + "]";
            return out;
        }
        case json11::Json::OBJECT: {
            const auto& obj = json.object_items();
            if (obj.empty()) return "{}";
            std::string out = "{\n";
            auto it = obj.begin();
            while (it != obj.end()) {
                out += std::string(4 * (indent + 1), ' ');
                out += "\"" + it->first + "\": " + pretty_print_json(it->second, indent + 1);
                if (++it != obj.end()) out += ",";
                out += "\n";
            }
            out += indent_str + "}";
            return out;
        }
    }
    return "";
}

static std::string build_streams_array(AVFormatContext* fmtCtx) {
    json11::Json::array video_streams;
    json11::Json::array audio_streams;
    json11::Json::array subtitle_streams;
    json11::Json::array data_streams;
    json11::Json::array attachment_streams;
    json11::Json::array unknown_streams;

    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        AVStream* stream = fmtCtx->streams[i];
        json11::Json stream_json = build_stream_json(stream, i);

        switch (stream->codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                video_streams.push_back(stream_json);
                break;
            case AVMEDIA_TYPE_AUDIO:
                audio_streams.push_back(stream_json);
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                subtitle_streams.push_back(stream_json);
                break;
            case AVMEDIA_TYPE_DATA:
                data_streams.push_back(stream_json);
                break;
            case AVMEDIA_TYPE_ATTACHMENT:
                attachment_streams.push_back(stream_json);
                break;
            default:
                unknown_streams.push_back(stream_json);
                break;
        }
    }

    json11::Json::object result_obj;

    if (!video_streams.empty())
        result_obj["video_streams"] = video_streams;
    if (!audio_streams.empty())
        result_obj["audio_streams"] = audio_streams;
    if (!subtitle_streams.empty())
        result_obj["subtitle_streams"] = subtitle_streams;
    if (!data_streams.empty())
        result_obj["data_streams"] = data_streams;
    if (!attachment_streams.empty())
        result_obj["attachment_streams"] = attachment_streams;
    if (!unknown_streams.empty())
        result_obj["unknown_streams"] = unknown_streams;

    return pretty_print_json(json11::Json(result_obj));
}


int main(int argc, char* argv[])
{
    unsigned int version = avformat_version();
    av_log_set_level(AV_LOG_ERROR);
    LOGD("FFmpeg avformat version: %d: %d.%d", (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF);
    // 打开mp4 文件
    AVFormatContext* fmtCtx = nullptr;
    const char* filePath = (argc > 1) ? argv[1] : "./res/test_zzz.mp4";
    LOGD("open file: %s", filePath);

    // 1. 分配 AVFormatContext 内存
    fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        LOGD("avformat_alloc_context failed");
        return -1;
    }

    // 2. 调用 avformat_open_input 打开 MP4 文件
    // 参数依次为：上下文指针、文件路径、输入格式（通常传NULL让FFmpeg自动探测）、附加选项
    if (avformat_open_input(&fmtCtx, filePath, nullptr, nullptr) != 0) {
        LOGD("open failed: %s", filePath);
        return -1;
    }

    // 3. 获取流信息（读取音视频流的基本参数）
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        LOGD("get stream info failed");
        avformat_close_input(&fmtCtx); // 记得关闭文件
        return -1;
    }
    LOGD("file path: %s open success", fmtCtx->url);
    // 可以在这里打印文件时长、码率等信息
    LOGD("file duration: %llds", (long long)(fmtCtx->duration / AV_TIME_BASE));

    // 打印每条流的 index 和类型
    int video_stream_index = -1;
    int audio_stream_index = -1;
    int subtitle_stream_index = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        AVStream* stream = fmtCtx->streams[i];
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

        // nb_frames
        LOGD("nb_frames: %d", stream->nb_frames);
        // AVCodecParameters
        AVCodecParameters* codecpar = stream->codecpar;
        // width height framerate
        LOGD("width: %d, height: %d, framerate: %d/%d", codecpar->width, codecpar->height,
            stream->avg_frame_rate.num, stream->avg_frame_rate.den);

        // stream->disposition;
        bool is_attached_pic = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        LOGD("disposition: %d, is_attached_pic=%d", stream->disposition, is_attached_pic);
        // stream->attached_pic;
        LOGD("attached_pic: %p, size=%d", stream->attached_pic.data, stream->attached_pic.size);
    }

    // 使用 json11 构建每个 stream 的 JSON 并打印
    std::string json_output = build_streams_array(fmtCtx);
    LOGD("========== streams json ==========");
    std::cout << json_output << std::endl;
    LOGD("========== end ==========");


    // ========== 初始化视频解码器 ==========
    AVCodecContext *decCtx = nullptr;
    AVFrame *frame = nullptr;
    if (video_stream_index >= 0) {
        // 根据 codec_id 查找解码器
        const AVCodec *decoder = avcodec_find_decoder(fmtCtx->streams[video_stream_index]->codecpar->codec_id);
        if (!decoder) {
            LOGD("avcodec_find_decoder failed");
            avformat_close_input(&fmtCtx);
            return -1;
        }
        LOGD("find decoder: %s", decoder->name);

        // 分配解码器上下文
        decCtx = avcodec_alloc_context3(decoder);
        if (!decCtx) {
            LOGD("avcodec_alloc_context3 failed");
            avformat_close_input(&fmtCtx);
            return -1;
        }

        // 将 codecpar 参数复制到解码器上下文
        if (avcodec_parameters_to_context(decCtx, fmtCtx->streams[video_stream_index]->codecpar) < 0) {
            LOGD("avcodec_parameters_to_context failed");
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            return -1;
        }

        // 打开解码器
        if (avcodec_open2(decCtx, decoder, nullptr) < 0) {
            LOGD("avcodec_open2 failed");
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            return -1;
        }

        // 分配 AVFrame 用于存储解码后的数据
        frame = av_frame_alloc();
        if (!frame) {
            LOGD("av_frame_alloc failed");
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            return -1;
        }
    }

    AVPacket *pkt = av_packet_alloc();
    int m_width = 0;
    int m_height = 0;
    int video_pkt_count = 0;
    int keyframe_count = 0;
    int non_keyframe_count = 0;
    while( av_read_frame(fmtCtx, pkt) >= 0){
        if(pkt->stream_index == video_stream_index)
        {
            video_pkt_count++;

            // 统计关键帧和非关键帧
            if (pkt->flags & AV_PKT_FLAG_KEY) {
                keyframe_count++;
            } else {
                non_keyframe_count++;
            }

            // 将 packet 发送给解码器
            if (avcodec_send_packet(decCtx, pkt) < 0) {
                LOGD("avcodec_send_packet failed");
                av_packet_unref(pkt);
                continue;
            }

            // 从解码器接收解码后的 frame
            int ret = avcodec_receive_frame(decCtx, frame);
            if (ret == 0) {
                // 打印宽高
                if ( m_width != frame->width || m_height != frame->height)
                {
                    LOGD("decoded frame: width=%d, height=%d", frame->width, frame->height);
                    m_width = frame->width;
                    m_height = frame->height;
                }
            } else if (ret == AVERROR(EAGAIN)) {
                // 需要更多数据才能解码出一帧，这是正常情况
                // LOGD("decode need more data");
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
                // LOGD("avcodec_receive_frame failed: %s", errbuf);
            }
        }else if (pkt->stream_index == audio_stream_index)
        {
            // audio packet
        }else if (pkt->stream_index == subtitle_stream_index)
        {
            // subtitle packet
        }else{
            // LOGD("other packet");
        }

        av_packet_unref(pkt); 
    }

    // 冲刷解码器：发送 NULL 让解码器输出缓冲的剩余帧
    if (decCtx) {
        avcodec_send_packet(decCtx, nullptr);
        while (avcodec_receive_frame(decCtx, frame) == 0) {
            LOGD("flush decoded frame: width=%d, height=%d", frame->width, frame->height);
        }
    }

    // 打印统计信息
    LOGD("========== video packet statistics ==========");
    LOGD("total video packets: %d", video_pkt_count);
    LOGD("  -> keyframe:     %d", keyframe_count);
    LOGD("  -> non-keyframe: %d", non_keyframe_count);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&decCtx);

    // 4. 使用完毕后，关闭文件并释放资源
    avformat_close_input(&fmtCtx);

    LOGD("test end");

    
    return 0;
}