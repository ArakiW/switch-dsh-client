/*
 * 后端①:局域网 DeepSeek Harness(经 dsh-bridge)。
 * 协议见 DSH_API_SPEC.md:
 *   - RPC:POST /api/<method>,信封 {"type":"client-request","rpcId","method","payload"}
 *   - 下行:bridge 的 SSE 端点 /api/events.sse?sessionId=x,每行是 server-request 帧
 *   - 正文:session/event -> assistant/chunk -> text-delta
 *   - 结束:assistant/message 或 turn/end;错误:stream/error
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SWITCH__
#include <switch.h>
#else
#include "switch_compat.h" /* host 测试兼容 */
#endif

#include "backend.h"
#include "net.h"
#include "cJSON.h"

#ifdef __SWITCH__
#define SESSION_FILE "sdmc:/switch/switch-dsh-client/session_id.txt"
#endif

static char *g_base = NULL;    /* 去尾斜杠后的 harness_base_url */
static char *g_session = NULL; /* 会话 id(session-<uuid>) */

/* ---------- 会话持久化(仅 Switch;host 测试每次新建会话) ---------- */

#ifdef __SWITCH__
static void save_session(const char *id) {
    FILE *f = fopen(SESSION_FILE, "wb");
    if (f) {
        fwrite(id, 1, strlen(id), f);
        fclose(f);
    }
}

static char *load_session(void) {
    FILE *f = fopen(SESSION_FILE, "rb");
    if (!f) return NULL;
    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n < 8) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}
#else
static void save_session(const char *id) { (void)id; }
static char *load_session(void) { return NULL; }
#endif

/* ---------- 工具 ---------- */

static void make_uuid(char *out) {
    u8 b[16];
    randomGet(b, sizeof(b));
    b[6] = (u8)((b[6] & 0x0F) | 0x40);
    b[8] = (u8)((b[8] & 0x3F) | 0x80);
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/*
 * 发一个 RPC。payload 所有权被接管。成功时 *value_out 为 result.value 的副本
 * (调用方 cJSON_Delete),失败时 err 有可展示信息。
 */
static int rpc(const char *base, const char *method, cJSON *payload,
               cJSON **value_out, char *err, size_t errsz) {
    char rpcid[37];
    make_uuid(rpcid);

    cJSON *env = cJSON_CreateObject();
    if (!env) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        cJSON_Delete(payload);
        return -1;
    }
    cJSON_AddStringToObject(env, "type", "client-request");
    cJSON_AddStringToObject(env, "rpcId", rpcid);
    cJSON_AddStringToObject(env, "method", method);
    cJSON_AddItemToObject(env, "payload", payload);

    char *body = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (!body) {
        if (err && errsz) snprintf(err, errsz, "JSON 序列化失败");
        return -1;
    }

    char url[640];
    snprintf(url, sizeof(url), "%s/api/%s", base, method);

    net_buffer_t resp = {0};
    long code = 0;
    int ok = net_post_json(url, body, &resp, &code, err, errsz);
    free(body);
    if (ok != 0) {
        net_buffer_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.data);
    net_buffer_free(&resp);
    if (!root) {
        if (err && errsz) snprintf(err, errsz, "响应不是合法 JSON");
        return -1;
    }

    int ret = -1;
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *rok = res ? cJSON_GetObjectItemCaseSensitive(res, "ok") : NULL;
    if (cJSON_IsTrue(rok)) {
        const cJSON *val = cJSON_GetObjectItemCaseSensitive(res, "value");
        if (value_out) *value_out = cJSON_Duplicate(val ? val : cJSON_CreateObject(), 1);
        ret = 0;
    } else {
        const cJSON *e = res ? cJSON_GetObjectItemCaseSensitive(res, "error") : NULL;
        const cJSON *msg = e ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;
        if (err && errsz) {
            snprintf(err, errsz, "Harness: %s",
                     cJSON_IsString(msg) ? msg->valuestring
                     : (code == 502 ? "桥接不可达(dsh-bridge 未运行?)" : "未知错误"));
        }
    }
    cJSON_Delete(root);
    return ret;
}

/* ---------- 会话保障 ---------- */

static int ensure_session(const backend_config_t *cfg, char *err, size_t errsz) {
    char *base = strdup(cfg->harness_base_url ? cfg->harness_base_url : "");
    if (!base) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') base[--len] = '\0';

    if (g_base && strcmp(g_base, base) == 0 && g_session) {
        free(base);
        return 0;
    }

    free(g_base);
    free(g_session);
    g_base = base;
    g_session = load_session();
    if (g_session) return 0;

    cJSON *payload = cJSON_CreateObject();
    cJSON *val = NULL;
    if (rpc(g_base, "session.create", payload, &val, err, errsz) != 0) return -1;

    const cJSON *sid = val ? cJSON_GetObjectItemCaseSensitive(val, "sessionId") : NULL;
    if (cJSON_IsString(sid) && sid->valuestring[0]) {
        g_session = strdup(sid->valuestring);
        save_session(g_session);
    } else if (err && errsz) {
        snprintf(err, errsz, "session.create 未返回 sessionId");
    }
    cJSON_Delete(val);
    return g_session ? 0 : -1;
}

/* ---------- SSE 帧解析 ---------- */

typedef struct {
    backend_chunk_cb on_chunk;
    void *ud;
    int finished;
    char err[256];
} hctx_t;

static int on_sse_line(const char *line, void *ud) {
    hctx_t *c = (hctx_t *)ud;
    cJSON *frame = cJSON_Parse(line);
    if (!frame) return 0;

    int stop = 0;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(frame, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "server-request") == 0) {
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(frame, "payload");
        const cJSON *ptype = payload ? cJSON_GetObjectItemCaseSensitive(payload, "type") : NULL;
        if (cJSON_IsString(ptype)) {
            if (strcmp(ptype->valuestring, "session/event") == 0) {
                const cJSON *event = cJSON_GetObjectItemCaseSensitive(payload, "event");
                const cJSON *etype = event ? cJSON_GetObjectItemCaseSensitive(event, "type") : NULL;
                const cJSON *data = event ? cJSON_GetObjectItemCaseSensitive(event, "data") : NULL;
                if (cJSON_IsString(etype) && data) {
                    if (strcmp(etype->valuestring, "assistant/chunk") == 0) {
                        const cJSON *chunk = cJSON_GetObjectItemCaseSensitive(data, "chunk");
                        const cJSON *ctype = chunk ? cJSON_GetObjectItemCaseSensitive(chunk, "type") : NULL;
                        if (cJSON_IsString(ctype) && strcmp(ctype->valuestring, "text-delta") == 0) {
                            const cJSON *text = cJSON_GetObjectItemCaseSensitive(chunk, "text");
                            if (cJSON_IsString(text) && text->valuestring[0]) {
                                if (c->on_chunk) c->on_chunk(text->valuestring, c->ud);
                            }
                        }
                        /* reasoning-delta / tool-call-delta / usage:一期忽略 */
                    } else if (strcmp(etype->valuestring, "assistant/message") == 0 ||
                               strcmp(etype->valuestring, "turn/end") == 0) {
                        c->finished = 1;
                        stop = 1;
                    }
                }
            } else if (strcmp(ptype->valuestring, "stream/error") == 0) {
                const cJSON *e = cJSON_GetObjectItemCaseSensitive(payload, "error");
                if (cJSON_IsString(e)) snprintf(c->err, sizeof(c->err), "%s", e->valuestring);
                else snprintf(c->err, sizeof(c->err), "事件流错误");
                c->finished = 1;
                stop = 1;
            }
        }
    }
    cJSON_Delete(frame);
    return stop;
}

/* ---------- chat 入口 ---------- */

static int harness_chat(const backend_config_t *cfg,
                        const chat_message_t *history, size_t n_history,
                        backend_chunk_cb on_chunk, backend_done_cb on_done,
                        void *ud) {
    (void)n_history;
    char err[256] = {0};

    /* 服务端自维护历史,这里只取最后一条用户消息 */
    const char *user_text = NULL;
    for (size_t i = n_history; i-- > 0;) {
        if (history[i].role == ROLE_USER && history[i].content && history[i].content[0]) {
            user_text = history[i].content;
            break;
        }
    }
    if (!user_text) {
        if (on_done) on_done(0, "没有待发送的用户消息", ud);
        return 0;
    }

    if (ensure_session(cfg, err, sizeof(err)) != 0) {
        if (on_done) on_done(0, err, ud);
        return 0;
    }

    /* session.prompt */
    cJSON *content = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", "text");
    cJSON_AddStringToObject(part, "text", user_text);
    cJSON_AddItemToArray(content, part);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sessionId", g_session);
    cJSON_AddStringToObject(payload, "mode", "queue");
    cJSON_AddItemToObject(payload, "content", content);

    cJSON *val = NULL;
    if (rpc(g_base, "session.prompt", payload, &val, err, sizeof(err)) != 0) {
        if (on_done) on_done(0, err, ud);
        return 0;
    }
    cJSON_Delete(val);

    /* 下行 SSE(bridge 已按 sessionId 过滤) */
    char url[640];
    snprintf(url, sizeof(url), "%s/api/events.sse?sessionId=%s", g_base, g_session);

    hctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.on_chunk = on_chunk;
    ctx.ud = ud;

    int rc = net_sse(url, "GET", NULL, NULL, 0, on_sse_line, &ctx, err, sizeof(err));

    if (on_done) {
        if (rc != 0) on_done(0, err[0] ? err : "事件流中断", ud);
        else if (ctx.err[0]) on_done(0, ctx.err, ud);
        else on_done(1, NULL, ud);
    }
    return 0;
}

const backend_vtable_t backend_vtable_harness = {
    .name = "Harness",
    .chat = harness_chat,
};
