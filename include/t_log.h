#ifndef _T_LOG_H_
#define _T_LOG_H_ 

#define FILENAME (std::string(__FILE__).substr(std::string(__FILE__).find_last_of("/\\") + 1).c_str())
#define LOGD(fmt, ...) printf("[%s:%d]|%s|: " fmt "\n", FILENAME, __LINE__,__FUNCTION__, ##__VA_ARGS__)
#endif