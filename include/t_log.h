#ifndef _T_LOG_H_
#define _T_LOG_H_ 

#define LOGD(fmt, ...) printf("[%s:%d][sxl test] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#endif