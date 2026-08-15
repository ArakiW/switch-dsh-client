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
