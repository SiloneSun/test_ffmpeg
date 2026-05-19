#ifndef __UTILS_H__
#define __UTILS_H__

extern "C" {
#include "avformat.h"
// #include "libavcodec/avcodec.h"
// #include "libavutil/frame.h"
}

#include "json11.hpp"
#include <string>


std::string rational_str(AVRational r);
double rational_to_double(AVRational r);
double ts_to_sec(int64_t ts, AVRational time_base);
const char* media_type_str(enum AVMediaType type);
const char* field_order_str(enum AVFieldOrder order);
json11::Json build_stream_json(AVStream* stream, int stream_index);
std::string pretty_print_json(const json11::Json& json, int indent = 0);
std::string build_streams_array(AVFormatContext* fmtCtx);


#endif