#ifndef SWITCH_DSH_NET_H
#define SWITCH_DSH_NET_H

#include <stddef.h>

/* 全量响应体缓冲 */
typedef struct {
    char *data;
    size_t len;
} net_buffer_t;

void net_buffer_free(net_buffer_t *b);

/*
 * POST JSON 并收集响应体。返回 0 传输成功(HTTP 状态码经 http_code 返回,
 * 调用方自行判断),-1 传输失败(err 有可展示信息)。out 由调用方释放。
 */
int net_post_json(const char *url, const char *body, net_buffer_t *out,
                  long *http_code, char *err, size_t errsz);

/*
 * POST 二进制音频(WAV,Content-Type: audio/wav)并收集响应体。
 * data/len 为原始字节(可含 \0)。其余语义同 net_post_json。
 */
int net_post_audio(const char *url, const void *data, size_t len,
                   net_buffer_t *out, long *http_code, char *err, size_t errsz);

/*
 * GET 请求并收集响应体(headers 为附加头数组,n_headers 条,可为空)。
 * 返回 0 传输成功,-1 失败。out 由调用方释放。
 */
int net_get_json(const char *url, const char *const *headers, size_t n_headers,
                 net_buffer_t *out, long *http_code, char *err, size_t errsz);

/*
 * SSE 流式请求。method 为 "GET" 或 "POST";body 非 NULL 时作为 JSON 请求体发送。
 * headers 为附加头数组(如 "Authorization: Bearer xxx"),n_headers 条。
 * 每收到一条以 "data:" 开头的行,回调一次(参数为去掉前缀与空白后的内容,
 * 不含换行;": " 开头的注释行与 event: 行会被忽略)。
 * 回调返回非 0 视为正常中止。返回 0 成功(含回调中止),-1 失败。
 */
typedef int (*net_sse_line_cb)(const char *line, void *userdata);
int net_sse(const char *url, const char *method, const char *body,
            const char *const *headers, size_t n_headers,
            net_sse_line_cb on_line, void *userdata, char *err, size_t errsz);

/*
 * 请求中止:从任意线程调用,使正在进行的 net_sse 尽快返回(返回 0,
 * 视为"已中止")。下次 net_sse 开始时自动复位。
 */
void net_sse_cancel(void);
int net_sse_aborted(void);

#endif /* SWITCH_DSH_NET_H */
