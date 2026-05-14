#include <iostream>

extern "C" {
    // 因为 FFmpeg 是用 C 语言编写的，所以在 C++ 中包含 FFmpeg 的头文件时，需要使用 extern "C" 来告诉编译器按照 C 的方式来链接这些函数。
#include "avformat.h"
}

#include <string>
#include "t_log.h"


int main(void)
{
    std::cout << "test ... ... " << std::endl;
    std::cout << "sunxilong: Hello, FFmpeg!" << std::endl;
    unsigned int version = avformat_version();
    std::cout << "FFmpeg avformat version: "
              << ((version >> 16) & 0xFF) << "."   // major
              << ((version >> 8) & 0xFF) << "."     // minor
              << (version & 0xFF) << std::endl;      // micro
    // 打开mp4 文件
    AVFormatContext* fmtCtx = nullptr;
    const char* filePath = "./res/test_zzz.mp4";

    // 1. 分配 AVFormatContext 内存
    fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        std::cerr << "内存分配失败" << std::endl;
        return -1;
    }

    // 2. 调用 avformat_open_input 打开 MP4 文件
    // 参数依次为：上下文指针、文件路径、输入格式（通常传NULL让FFmpeg自动探测）、附加选项
    if (avformat_open_input(&fmtCtx, filePath, nullptr, nullptr) != 0) {
        std::cerr << "无法打开文件: " << filePath << std::endl;
        return -1;
    }

    // 3. 获取流信息（读取音视频流的基本参数）
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        std::cerr << "无法获取流信息" << std::endl;
        avformat_close_input(&fmtCtx); // 记得关闭文件
        return -1;
    }

    std::cout << "成功打开 MP4 文件！" << std::endl;
    // 可以在这里打印文件时长、码率等信息
    std::cout << "时长: " << fmtCtx->duration / AV_TIME_BASE << " 秒" << std::endl;

    // 4. 使用完毕后，关闭文件并释放资源
    avformat_close_input(&fmtCtx);

    LOGD("test end");

    
    return 0;
}