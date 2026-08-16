#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "net.h"

static int g_curl_ready = 0;

static void ensure_curl(void) {
    if (!g_curl_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_curl_ready = 1;
    }
}

/* ---------- 通用增长缓冲 ---------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} growbuf_t;

static size_t buf_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    growbuf_t *b = (growbuf_t *)ud;
    size_t n = size * nmemb;
    if (n == 0) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 8192;
        while (nc < b->len + n + 1) nc *= 2;
        char *nd = (char *)realloc(b->data, nc);
        if (!nd) return 0; /* 中止 */
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

void net_buffer_free(net_buffer_t *b) {
    if (!b) return;
    free(b->data);
    memset(b, 0, sizeof(*b));
}

/* ---------- SSE ---------- */

static volatile int g_sse_cancel = 0;

void net_sse_cancel(void) { g_sse_cancel = 1; }
int net_sse_aborted(void) { return g_sse_cancel != 0; }

typedef struct {
    net_sse_line_cb on_line;
    void *ud;
    growbuf_t line;
    int aborted;
} sse_ctx_t;

static size_t sse_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    sse_ctx_t *c = (sse_ctx_t *)ud;
    size_t n = size * nmemb;
    if (n == 0) return 0;
    if (g_sse_cancel) { /* 外部请求中止 */
        c->aborted = 1;
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        char ch = ptr[i];
        if (ch == '\n') {
            char *p = c->line.data ? c->line.data : (char *)"";
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "data:", 5) == 0) {
                p += 5;
                if (*p == ' ') p++;
                if (c->on_line(p, c->ud)) c->aborted = 1;
            }
            c->line.len = 0;
            if (c->line.data) c->line.data[0] = '\0';
            if (c->aborted) return 0; /* 上层按正常中止处理 */
        } else if (ch != '\r') {
            growbuf_t *b = &c->line;
            if (b->len + 2 > b->cap) {
                size_t nc = b->cap ? b->cap * 2 : 4096;
                char *nd = (char *)realloc(b->data, nc);
                if (!nd) return 0;
                b->data = nd;
                b->cap = nc;
            }
            b->data[b->len++] = ch;
            b->data[b->len] = '\0';
        }
    }
    return n;
}

/* ---------- 请求实现 ---------- */

static CURL *new_easy(const char *url, char *err, size_t errsz) {
    ensure_curl();
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err && errsz) snprintf(err, errsz, "curl 初始化失败");
        return NULL;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "switch-dsh-client/0.1");
    return curl;
}

int net_post_json(const char *url, const char *body, net_buffer_t *out,
                  long *http_code, char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    CURL *curl = new_easy(url, err, errsz);
    if (!curl) return -1;

    growbuf_t buf = {0};
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L); /* LAN 操作 15s 上限,避免冻屏 */

    CURLcode rc = curl_easy_perform(curl);
    if (http_code) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    out->data = buf.data ? buf.data : strdup("");
    out->len = buf.len;
    if (rc != CURLE_OK) {
        if (err && errsz) snprintf(err, errsz, "网络错误: %s", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

int net_get_json(const char *url, const char *const *headers, size_t n_headers,
                 net_buffer_t *out, long *http_code, char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    CURL *curl = new_easy(url, err, errsz);
    if (!curl) return -1;

    growbuf_t buf = {0};
    struct curl_slist *hdrs = NULL;
    for (size_t i = 0; i < n_headers; i++) hdrs = curl_slist_append(hdrs, headers[i]);

    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);

    CURLcode rc = curl_easy_perform(curl);
    if (http_code) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    out->data = buf.data ? buf.data : strdup("");
    out->len = buf.len;
    if (rc != CURLE_OK) {
        if (err && errsz) snprintf(err, errsz, "网络错误: %s", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

int net_sse(const char *url, const char *method, const char *body,
            const char *const *headers, size_t n_headers,
            net_sse_line_cb on_line, void *userdata, char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    g_sse_cancel = 0; /* 复位中止标志 */

    CURL *curl = new_easy(url, err, errsz);
    if (!curl) return -1;

    sse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.on_line = on_line;
    ctx.ud = userdata;

    struct curl_slist *hdrs = NULL;
    if (body) hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    for (size_t i = 0; i < n_headers; i++) hdrs = curl_slist_append(hdrs, headers[i]);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L); /* SSE:无总超时 */
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(ctx.line.data);

    if (rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && ctx.aborted)) return 0;
    if (err && errsz) snprintf(err, errsz, "网络错误: %s", curl_easy_strerror(rc));
    return -1;
}
