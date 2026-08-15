#ifndef SWITCH_DSH_TEXTINPUT_H
#define SWITCH_DSH_TEXTINPUT_H

#include <stddef.h>

/*
 * 弹出系统软键盘让用户输入文本(阻塞,直到用户确认/取消)。
 * 系统键盘自带中文拼音输入;zh_only=1 时直接打开简体中文键盘
 * (SwkbdType_ZhHans),=0 时打开全语言键盘(用户可手动切换语言)。
 *
 * 返回:1=确认(文本写入 out,UTF-8,以 \0 结尾);
 *       0=取消或输入为空;
 *      -1=软键盘创建失败。
 */
int textinput_prompt(const char *header, const char *initial, int zh_only,
                     char *out, size_t outsz);

#endif /* SWITCH_DSH_TEXTINPUT_H */
