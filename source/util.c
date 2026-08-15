#include <ctype.h>
#include <string.h>

#include "util.h"

size_t utf8_strlen(const char *s) {
    size_t n = 0;
    while (s && *s) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) != 0x80) n++; /* 不数续字节 */
        s++;
    }
    return n;
}

size_t utf8_next_len(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t utf8_fit_bytes(const char *s, size_t max_bytes) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n] && n < max_bytes) {
        size_t step = 1;
        unsigned char c = (unsigned char)s[n];
        if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (n + step > max_bytes) break; /* 切开会把码点切断 */
        n += step;
    }
    return n;
}

char *str_trim(char *s) {
    if (!s) return s;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

static int cp_allowed(unsigned int cp) {
    if (cp >= 0x20 && cp <= 0x7E) return 1;          /* ASCII 可打印 */
    if (cp >= 0xA0 && cp <= 0xFF) return 1;          /* 拉丁-1 补充(含 ·) */
    if (cp >= 0x1100 && cp <= 0x9FFF) return 1;      /* 谚文/假名/CJK 汉字与标点 */
    if (cp >= 0x2000 && cp <= 0x206F) return 1;      /* 通用标点(… “ ” — 等) */
    if (cp >= 0xFE30 && cp <= 0xFE4F) return 1;      /* CJK 兼容形式 */
    if (cp >= 0xFF00 && cp <= 0xFFEF) return 1;      /* 全角形式(＋ 等) */
    return 0; /* emoji/dingbat/杂项符号/箭头等:丢弃 */
}

void utf8_sanitize(char *s) {
    if (!s) return;
    unsigned char *p = (unsigned char *)s;
    char *w = s;
    while (*p) {
        size_t len = utf8_next_len((const char *)p);
        unsigned int cp = 0;
        if (len == 1) cp = p[0];
        else if (len == 2) cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        else if (len == 3) cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        else if (len == 4) cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
                                ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        if (cp_allowed(cp)) {
            if (w != (char *)p) memmove(w, p, len);
            w += len;
        }
        p += len;
    }
    *w = '\0';
}
