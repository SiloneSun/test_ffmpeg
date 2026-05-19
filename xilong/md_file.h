#ifndef __MD_FILE_H
#define __MD_FILE_H 

#include <memory>
#include <string>
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
    };
};

#endif