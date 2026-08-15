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

/*
 * 原地剥离 Noto Sans CJK 字体无法渲染的字符(emoji、dingbat、
 * 杂项符号、箭头等),只保留 ASCII、拉丁-1、通用标点、CJK 汉字/
 * 假名/谚文/CJK 标点/全角形式。用于净化服务器标题与模型输出,
 * 避免出现"方框带叉"的缺字块。
 */
void utf8_sanitize(char *s);

#endif /* SWITCH_DSH_UTIL_H */
