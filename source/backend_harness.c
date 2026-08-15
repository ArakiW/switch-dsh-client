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
#include "backend_harness.h"
#include "net.h"
#include "cJSON.h"
#include "util.h"

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

static char *normalize_base(const backend_config_t *cfg) {
    char *base = strdup(cfg->harness_base_url ? cfg->harness_base_url : "");
    if (!base) return NULL;
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') base[--len] = '\0';
    return base;
}

/* force_new=1 时忽略已保存的会话,直接新建 */
static int ensure_session(const backend_config_t *cfg, char *err, size_t errsz,
                          int force_new) {
    char *base = normalize_base(cfg);
    if (!base) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }

    if (g_base && strcmp(g_base, base) == 0 && g_session) {
        free(base);
        return 0;
    }

    free(g_base);
    free(g_session);
    g_base = base;
    if (!force_new) {
        g_session = load_session();
        if (g_session) return 0;
    }

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
                        if (cJSON_IsString(ctype)) {
                            if (strcmp(ctype->valuestring, "text-delta") == 0) {
                                const cJSON *text = cJSON_GetObjectItemCaseSensitive(chunk, "text");
                                if (cJSON_IsString(text) && text->valuestring[0]) {
                                    if (c->on_chunk) c->on_chunk(text->valuestring, 0, c->ud);
                                }
                            } else if (strcmp(ctype->valuestring, "reasoning-delta") == 0) {
                                /* 思考过程:单独通道,UI 以暗色显示 */
                                const cJSON *text = cJSON_GetObjectItemCaseSensitive(chunk, "text");
                                if (cJSON_IsString(text) && text->valuestring[0]) {
                                    if (c->on_chunk) c->on_chunk(text->valuestring, 1, c->ud);
                                }
                            }
                        }
                        /* tool-call-delta / usage:忽略 */
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

    if (ensure_session(cfg, err, sizeof(err), 0) != 0) {
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

/* ---------- 会话浏览 / 切换 / 历史 ---------- */

int harness_list_sessions(const backend_config_t *cfg,
                          harness_session_t **out_list, size_t *out_n,
                          char *err, size_t errsz) {
    *out_list = NULL;
    *out_n = 0;

    char *base = normalize_base(cfg);
    if (!base) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }

    cJSON *val = NULL;
    int rc = rpc(base, "session.list", cJSON_CreateObject(), &val, err, errsz);
    free(base);
    if (rc != 0) return -1;

    const cJSON *items = val ? cJSON_GetObjectItemCaseSensitive(val, "items") : NULL;
    size_t n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    harness_session_t *list = (harness_session_t *)calloc(n ? n : 1, sizeof(*list));
    if (!list) {
        cJSON_Delete(val);
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }

    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(items, i);
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(it, "sessionId");
        if (!cJSON_IsString(sid) || !sid->valuestring[0]) continue;
        const cJSON *proj = cJSON_GetObjectItemCaseSensitive(it, "projections");
        const cJSON *vals = proj ? cJSON_GetObjectItemCaseSensitive(proj, "values") : NULL;
        const cJSON *title = vals ? cJSON_GetObjectItemCaseSensitive(vals, "title") : NULL;
        const cJSON *blank = cJSON_GetObjectItemCaseSensitive(it, "blank");
        const cJSON *running = cJSON_GetObjectItemCaseSensitive(it, "running");
        const cJSON *upd = cJSON_GetObjectItemCaseSensitive(it, "updatedAt");
        const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(it, "cwd");

        list[k].session_id = strdup(sid->valuestring);
        if (cJSON_IsString(title) && title->valuestring[0])
            list[k].title = strdup(title->valuestring);
        else
            list[k].title = strdup(cJSON_IsTrue(blank) ? "空白会话" : "未命名会话");
        utf8_sanitize(list[k].title); /* 剥 emoji 等缺字字符 */
        list[k].cwd = cJSON_IsString(cwd) ? strdup(cwd->valuestring) : NULL;
        list[k].running = cJSON_IsTrue(running);
        list[k].updated_at = cJSON_IsNumber(upd) ? (long long)upd->valuedouble : 0;
        k++;
    }
    cJSON_Delete(val);
    *out_list = list;
    *out_n = k;
    return 0;
}

void harness_sessions_free(harness_session_t *list, size_t n) {
    if (!list) return;
    for (size_t i = 0; i < n; i++) {
        free(list[i].session_id);
        free(list[i].title);
        free(list[i].cwd);
    }
    free(list);
}

int harness_use_session(const backend_config_t *cfg, const char *session_id,
                        char *err, size_t errsz) {
    char *base = normalize_base(cfg);
    if (!base) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    if (!g_base || strcmp(g_base, base) != 0) {
        free(g_base);
        g_base = base;
        free(g_session);
        g_session = NULL;
    } else {
        free(base);
    }

    if (session_id && session_id[0]) {
        free(g_session);
        g_session = strdup(session_id);
        save_session(g_session);
        return 0;
    }
    free(g_session);
    g_session = NULL;
    return ensure_session(cfg, err, errsz, 1); /* 强制新建 */
}

/* 把 content 数组里的 text 块拼进 *buf */
static void append_text_parts(const cJSON *content, char **buf, size_t *len) {
    if (!cJSON_IsArray(content)) return;
    size_t n = cJSON_GetArraySize(content);
    for (size_t i = 0; i < n; i++) {
        const cJSON *part = cJSON_GetArrayItem(content, i);
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(part, "type");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "text") != 0 ||
            !cJSON_IsString(text) || !text->valuestring[0])
            continue;
        size_t add = strlen(text->valuestring);
        if (*len == 0) {
            *buf = strdup(text->valuestring);
            if (*buf) *len = add;
        } else {
            char *nb = (char *)realloc(*buf, *len + add + 1);
            if (nb) {
                memcpy(nb + *len, text->valuestring, add + 1);
                *buf = nb;
                *len += add;
            }
        }
    }
}

int harness_fetch_history(const backend_config_t *cfg,
                          chat_message_t **out_msgs, size_t *out_n,
                          char *err, size_t errsz) {
    *out_msgs = NULL;
    *out_n = 0;

    if (!g_session) {
        if (ensure_session(cfg, err, errsz, 0) != 0) return -1;
    }

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sessionId", g_session);
    cJSON_AddNumberToObject(payload, "maxMessages", 50);
    cJSON *val = NULL;
    if (rpc(g_base, "session.history", payload, &val, err, errsz) != 0) return -1;

    const cJSON *events = val ? cJSON_GetObjectItemCaseSensitive(val, "events") : NULL;
    size_t n = cJSON_IsArray(events) ? cJSON_GetArraySize(events) : 0;

    /* 扫描全部事件,用环形缓冲保留最后 64 条 user/assistant 消息 */
    chat_message_t *msgs = (chat_message_t *)calloc(64, sizeof(*msgs));
    if (!msgs) {
        cJSON_Delete(val);
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    size_t m_total = 0;
    for (size_t i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(events, i);
        const cJSON *ev = item ? cJSON_GetObjectItemCaseSensitive(item, "event") : NULL;
        const cJSON *type = ev ? cJSON_GetObjectItemCaseSensitive(ev, "type") : NULL;
        const cJSON *data = ev ? cJSON_GetObjectItemCaseSensitive(ev, "data") : NULL;
        if (!cJSON_IsString(type) || !data) continue;

        int role = -1;
        const cJSON *content = NULL;
        if (strcmp(type->valuestring, "user/message") == 0) {
            role = ROLE_USER;
            content = cJSON_GetObjectItemCaseSensitive(data, "content");
        } else if (strcmp(type->valuestring, "assistant/message") == 0) {
            role = ROLE_ASSISTANT;
            const cJSON *message = cJSON_GetObjectItemCaseSensitive(data, "message");
            content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
        }
        if (role < 0) continue;

        char *text = NULL;
        size_t tlen = 0;
        append_text_parts(content, &text, &tlen);
        if (text && text[0]) {
            size_t slot = m_total % 64;
            free(msgs[slot].content);
            msgs[slot].role = (chat_role_t)role;
            msgs[slot].content = text;
            m_total++;
        } else {
            free(text);
        }
    }
    cJSON_Delete(val);

    size_t m = m_total < 64 ? m_total : 64;
    if (m_total > 64) {
        /* 旋转:最旧消息挪到下标 0 */
        size_t rot = m_total % 64;
        chat_message_t tmp[64];
        memcpy(tmp, msgs, sizeof(tmp));
        for (size_t i = 0; i < 64; i++) msgs[i] = tmp[(i + rot) % 64];
    }
    *out_msgs = msgs;
    *out_n = m;
    return 0;
}

/* ---------- 工作区 ---------- */

int harness_list_workspaces(const backend_config_t *cfg,
                            harness_workspace_t **out_list, size_t *out_n,
                            char *err, size_t errsz) {
    *out_list = NULL;
    *out_n = 0;

    char *base = normalize_base(cfg);
    if (!base) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    cJSON *val = NULL;
    int rc = rpc(base, "workspace.list", cJSON_CreateObject(), &val, err, errsz);
    free(base);
    if (rc != 0) return -1;

    const cJSON *items = val ? cJSON_GetObjectItemCaseSensitive(val, "items") : NULL;
    size_t n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    harness_workspace_t *list =
        (harness_workspace_t *)calloc(n ? n : 1, sizeof(*list));
    if (!list) {
        cJSON_Delete(val);
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(items, i);
        const cJSON *wid = cJSON_GetObjectItemCaseSensitive(it, "workspaceId");
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(it, "path");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(it, "title");
        const cJSON *sids = cJSON_GetObjectItemCaseSensitive(it, "sessionIds");
        if (!cJSON_IsString(wid)) continue;
        list[k].workspace_id = strdup(wid->valuestring);
        list[k].path = cJSON_IsString(path) ? strdup(path->valuestring) : strdup("");
        list[k].title = cJSON_IsString(title) && title->valuestring[0]
                            ? strdup(title->valuestring)
                            : strdup("未命名工作区");
        utf8_sanitize(list[k].title);
        list[k].session_count = cJSON_IsArray(sids) ? cJSON_GetArraySize(sids) : 0;
        k++;
    }
    cJSON_Delete(val);
    *out_list = list;
    *out_n = k;
    return 0;
}

void harness_workspaces_free(harness_workspace_t *list, size_t n) {
    if (!list) return;
    for (size_t i = 0; i < n; i++) {
        free(list[i].workspace_id);
        free(list[i].path);
        free(list[i].title);
    }
    free(list);
}

int harness_create_workspace(const backend_config_t *cfg, const char *path,
                             char *err, size_t errsz) {
    char *base = normalize_base(cfg);
    if (!base) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "path", path ? path : "");
    cJSON *val = NULL;
    int rc = rpc(base, "workspace.create", payload, &val, err, errsz);
    free(base);
    cJSON_Delete(val);
    return rc;
}

/* 用给定 payload 建会话并切换过去 */
static int create_session_with(const backend_config_t *cfg, cJSON *payload,
                               char *err, size_t errsz) {
    if (!g_base) {
        char *base = normalize_base(cfg);
        if (!base) {
            cJSON_Delete(payload);
            if (err && errsz) snprintf(err, errsz, "内存不足");
            return -1;
        }
        g_base = base;
    }
    free(g_session);
    g_session = NULL;

    cJSON *val = NULL;
    int rc = rpc(g_base, "session.create", payload, &val, err, errsz);
    if (rc != 0) return -1;

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

int harness_new_session_in(const backend_config_t *cfg, const char *workspace_id,
                           char *err, size_t errsz) {
    cJSON *payload = cJSON_CreateObject();
    if (workspace_id && workspace_id[0])
        cJSON_AddStringToObject(payload, "workspaceId", workspace_id);
    return create_session_with(cfg, payload, err, errsz);
}

/* ---------- 模型 ---------- */

int harness_list_models(const backend_config_t *cfg,
                        model_option_t **out, size_t *out_n,
                        char *cur, size_t cursz, char *err, size_t errsz) {
    *out = NULL;
    *out_n = 0;
    if (cur && cursz) cur[0] = '\0';

    if (ensure_session(cfg, err, errsz, 0) != 0) return -1;

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sessionId", g_session);
    cJSON *val = NULL;
    if (rpc(g_base, "session.models", payload, &val, err, errsz) != 0) return -1;

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(val, "current");
    if (current) {
        const cJSON *cm = cJSON_GetObjectItemCaseSensitive(current, "model");
        if (cur && cursz && cJSON_IsString(cm))
            snprintf(cur, cursz, "%s", cm->valuestring);
    }

    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(val, "groups");
    size_t total = 0;
    if (cJSON_IsArray(groups)) {
        size_t ng = cJSON_GetArraySize(groups);
        for (size_t g = 0; g < ng; g++) {
            const cJSON *grp = cJSON_GetArrayItem(groups, g);
            const cJSON *models = cJSON_GetObjectItemCaseSensitive(grp, "models");
            if (cJSON_IsArray(models)) total += cJSON_GetArraySize(models);
        }
    }

    model_option_t *list = (model_option_t *)calloc(total ? total : 1, sizeof(*list));
    if (!list) {
        cJSON_Delete(val);
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    size_t k = 0;
    if (cJSON_IsArray(groups)) {
        size_t ng = cJSON_GetArraySize(groups);
        for (size_t g = 0; g < ng; g++) {
            const cJSON *grp = cJSON_GetArrayItem(groups, g);
            const cJSON *gid = cJSON_GetObjectItemCaseSensitive(grp, "id");
            const cJSON *models = cJSON_GetObjectItemCaseSensitive(grp, "models");
            if (!cJSON_IsArray(models)) continue;
            size_t nm = cJSON_GetArraySize(models);
            for (size_t i = 0; i < nm; i++) {
                const cJSON *m = cJSON_GetArrayItem(models, i);
                const cJSON *mid = cJSON_GetObjectItemCaseSensitive(m, "id");
                const cJSON *mname = cJSON_GetObjectItemCaseSensitive(m, "name");
                if (!cJSON_IsString(mid)) continue;
                list[k].id = strdup(mid->valuestring);
                list[k].name = cJSON_IsString(mname) && mname->valuestring[0]
                                   ? strdup(mname->valuestring)
                                   : strdup(mid->valuestring);
                list[k].provider = cJSON_IsString(gid) ? strdup(gid->valuestring)
                                                       : strdup("");
                k++;
            }
        }
    }
    cJSON_Delete(val);
    *out = list;
    *out_n = k;
    return 0;
}

void harness_models_free(model_option_t *list, size_t n) {
    if (!list) return;
    for (size_t i = 0; i < n; i++) {
        free(list[i].id);
        free(list[i].name);
        free(list[i].provider);
    }
    free(list);
}

int harness_select_model(const backend_config_t *cfg,
                         const char *provider, const char *model,
                         char *err, size_t errsz) {
    if (ensure_session(cfg, err, errsz, 0) != 0) return -1;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sessionId", g_session);
    cJSON_AddStringToObject(payload, "provider", provider ? provider : "");
    cJSON_AddStringToObject(payload, "model", model ? model : "");
    cJSON *val = NULL;
    int rc = rpc(g_base, "session.selectModel", payload, &val, err, errsz);
    cJSON_Delete(val);
    return rc;
}

const backend_vtable_t backend_vtable_harness = {
    .name = "Harness",
    .chat = harness_chat,
};
