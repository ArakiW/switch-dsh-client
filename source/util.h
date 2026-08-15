#ifndef SWITCH_DSH_UTIL_H
#define SWITCH_DSH_UTIL_H

#include <stddef.h>

/* UTF-8 码点数(字符数) */
size_t utf8_strlen(const char *s);

/* 当前码点的字节长度(1-4) */
size_t utf8_next_len(const char *s);

/* 返回最长不超过 max_bytes 且不切断码点的前缀字节数 */
size_t utf8_fit_bytes(const char *s, size_t max_bytes);

/* 去掉首尾空白,原地修改,返回 s */
char *str_trim(char *s);

#endif /* SWITCH_DSH_UTIL_H */
