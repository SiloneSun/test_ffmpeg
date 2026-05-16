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
    LOGD("file duration: %ds", fmtCtx->duration / AV_TIME_BASE);


    int video_stream_index = -1;
    int audio_stream_index = -1;
    int subtitle_stream_index = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) { 
        AVStream* stream = fmtCtx->streams[i];
        LOGD("stream index: %d, codec_type: %d, codec_id: %d", i, stream->codecpar->codec_type, stream->codecpar->codec_id);
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