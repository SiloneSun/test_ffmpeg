#ifndef __MD_FILE_H
#define __MD_FILE_H 

#include <memory>
#include <string>
#include <mutex>
#include <stdint.h>
#include "md_ffmpeg.h"

namespace sunxilong
{

    class md_file
    {
        public:
            static std::shared_ptr<md_file> get(std::string file_name);
            md_file(std::string file_name);
            ~md_file();
            std::int16_t init();
            std::int16_t deinit();
            std::int16_t open_file();
            std::int16_t close_file();
            std::int16_t play();
            bool end_of_file();
        protected:
        private:
            // ── 时间戳记录 ──
            void record_video_play_pts(int64_t pts_us, int64_t render_wall_us, uint32_t audio_dev_id);

            unsigned int m_ff_version;
            std::string m_file_name;
            md_format_ctx m_fmtCtx;

            // 统计信息
            int video_stream_index = -1;
            int audio_stream_index = -1;
            int subtitle_stream_index = -1;
            int video_pkt_count = 0;
            int audio_pkt_count = 0;
            int keyframe_count = 0;
            int non_keyframe_count = 0;

            // 音视频时间戳（微秒）
            std::mutex m_ts_mtx;
            int64_t m_last_video_pts_us = AV_NOPTS_VALUE;

            // 音频播放追踪
            int64_t m_audio_source_pts_us = AV_NOPTS_VALUE;  // 最新推送音频帧的 PTS
            uint64_t m_audio_total_pushed_bytes = 0;         // 推入 SDL 的总字节数
            int m_audio_sr = 0;                              // 音频采样率（record_timestamp 需要）
            int m_audio_channels = 0;                        // 音频声道数（record_timestamp 需要）

            // 音视频同步时钟基准（用于将 PTS 对齐到墙钟时间域）
            int64_t m_audio_clock_ref_wall = 0;              // 音频基准墙钟 (av_gettime_relative)
            int64_t m_audio_clock_ref_pts = AV_NOPTS_VALUE;  // 音频基准 PTS
    };
};

#endif