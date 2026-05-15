#include <iostream>

extern "C" {
    // 因为 FFmpeg 是用 C 语言编写的，所以在 C++ 中包含 FFmpeg 的头文件时，需要使用 extern "C" 来告诉编译器按照 C 的方式来链接这些函数。
#include "avformat.h"
}

#include <string>
#include "t_log.h"


int main(void)
{
    unsigned int version = avformat_version();
    LOGD("FFmpeg avformat version: %d: %d.%d", (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF);
    // 打开mp4 文件
    AVFormatContext* fmtCtx = nullptr;
    const char* filePath = "./res/test_zzz.mp4";

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


    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) { 
        AVStream* stream = fmtCtx->streams[i];
        LOGD("stream index: %d, codec_type: %d, codec_id: %d", i, stream->codecpar->codec_type, stream->codecpar->codec_id);
        // nb_frames
        LOGD("nb_frames: %d", stream->nb_frames);
        // AVCodecParameters
        AVCodecParameters* codecpar = stream->codecpar;
        // width height framerate
        LOGD("width: %d, height: %d, framerate: %d/%d", codecpar->width, codecpar->height,
            stream->avg_frame_rate.num, stream->avg_frame_rate.den);
    }

    

    // 4. 使用完毕后，关闭文件并释放资源
    avformat_close_input(&fmtCtx);

    LOGD("test end");

    
    return 0;
}