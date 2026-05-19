#ifndef __MD_FFMPEG_H__
#define __MD_FFMPEG_H__

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/frame.h"
}

namespace sunxilong {

// 空对象标记：用于构造 md_obj 时不分配内存，初始化为 nullptr
struct empty_obj_t {};

// ============================================================
// 1. 分配/释放特征模板 ff_traits<T>
//    每种类型特化 alloc() 和 free()
// ============================================================
template<typename T>
struct ff_traits;

// --- AVPacket ---
template<>
struct ff_traits<AVPacket> {
    static AVPacket* alloc() { return av_packet_alloc(); }
    static void free(AVPacket** p) { av_packet_free(p); }
};

// --- AVFrame ---
template<>
struct ff_traits<AVFrame> {
    static AVFrame* alloc() { return av_frame_alloc(); }
    static void free(AVFrame** p) { av_frame_free(p); }
};

// --- AVCodecContext ---
template<>
struct ff_traits<AVCodecContext> {
    static AVCodecContext* alloc() { return avcodec_alloc_context3(nullptr); }
    static void free(AVCodecContext** p) { avcodec_free_context(p); }
};

// --- AVFormatContext ---
template<>
struct ff_traits<AVFormatContext> {
    static AVFormatContext* alloc() { return avformat_alloc_context(); }
    static void free(AVFormatContext** p) { avformat_close_input(p); }
};

// ============================================================
// 2. 通用 RAII 包装器 md_obj<T>
// ============================================================
template<typename T>
class md_obj {
public:
    using traits = ff_traits<T>;

    // 默认构造：自动分配
    md_obj() : m_ptr(traits::alloc()) {}

    // 空构造：不分配，初始化为 nullptr
    md_obj(empty_obj_t) : m_ptr(nullptr) {}

    // 析构：自动释放
    ~md_obj() { reset(); }

    // --- 禁止拷贝 ---
    md_obj(const md_obj&) = delete;
    md_obj& operator=(const md_obj&) = delete;

    // --- 允许移动 ---
    md_obj(md_obj&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }
    md_obj& operator=(md_obj&& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    // --- 获取原始指针 ---
    T* get() { return m_ptr; }
    const T* get() const { return m_ptr; }

    // --- 指针语义 ---
    T* operator->() { return m_ptr; }
    const T* operator->() const { return m_ptr; }
    T& operator*() { return *m_ptr; }
    const T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    // --- 替换为新的对象，或置空 ---
    void reset(T* p = nullptr) {
        if (m_ptr) {
            traits::free(&m_ptr);
        }
        m_ptr = p;
    }

    // --- 用于 FFmpeg 需要 T** 参数的场合 ---
    T** ptr() { return &m_ptr; }

private:
    T* m_ptr = nullptr;
};

// ============================================================
// 3. 便捷类型别名
// ============================================================
using md_packet     = md_obj<AVPacket>;
using md_frame      = md_obj<AVFrame>;
using md_codec_ctx  = md_obj<AVCodecContext>;
using md_format_ctx = md_obj<AVFormatContext>;

}  // namespace sunxilong

#endif  // __MD_FFMPEG_H__