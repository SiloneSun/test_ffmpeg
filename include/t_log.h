#ifndef _T_LOG_H_
#define _T_LOG_H_ 

// 帮我定义一个LOGD， 可以接受(fmt, ...)可扩展参数，然后将时间戳+fmt内容+后缀内容通过printf打印出来
#define LOGD(fmt, ...) printf("[%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#endif