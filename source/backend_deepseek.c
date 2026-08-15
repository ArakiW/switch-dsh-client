/*
 * 后端②:DeepSeek 官方 API(直连,HTTPS + SSE)。
 * 要点(RESEARCH.md):
 *   - POST {base}/chat/completions,Authorization: Bearer <key>
 *   - 模型 deepseek-v4-flash / deepseek-v4-pro;thinking.type enabled|disabled
 *   - 流式每帧 "data: <json>",正文 choices[0].delta.content,
 *     结束 data: [DONE](最后一帧带 finish_reason/usage)
 *   - HTTPS 走 libnx 系统 SSL(switch-curl 构建即如此),无需自定义 CA
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "net.h"
#include "cJSON.h"

typedef struct {
    backend_chunk_cb on_chunk;
    void *ud;
    int done;
    char err[256];
} dsctx_t;

static int ds_on_line(const char *line, void *ud) {
    dsctx_t *c = (dsctx_t *)ud;

    if (strcmp(line, "[DONE]") == 0) {
        c->done = 1;
        return 1;
    }

    cJSON *f = cJSON_Parse(line);
    if (!f) return 0;

    int stop = 0;
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(f, "error");
    if (err) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
        if (cJSON_IsString(m)) snprintf(c->err, sizeof(c->err), "%s", m->valuestring);
        else snprintf(c->err, sizeof(c->err), "DeepSeek 返回错误");
        c->done = 1;
        stop = 1;
    } else {
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(f, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            const cJSON *ch = cJSON_GetArrayItem(choices, 0);
            const cJSON *delta = ch ? cJSON_GetObjectItemCaseSensitive(ch, "delta") : NULL;
            if (delta) {
                /* 思考过程(thinking 开启时) */
                const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
                if (cJSON_IsString(reasoning) && reasoning->valuestring[0]) {
                    if (c->on_chunk) c->on_chunk(reasoning->valuestring, 1, c->ud);
                }
                /* 正文 */
                const cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
                if (cJSON_IsString(content) && content->valuestring[0]) {
                    if (c->on_chunk) c->on_chunk(content->valuestring, 0, c->ud);
                }
            }
            const cJSON *fr = ch ? cJSON_GetObjectItemCaseSensitive(ch, "finish_reason") : NULL;
            if (cJSON_IsString(fr) && fr->valuestring[0]) {
                c->done = 1;
                stop = 1;
            }
        }
    }
    cJSON_Delete(f);
    return stop;
}

static int ds_chat(const backend_config_t *cfg,
                   const chat_message_t *history, size_t n_history,
                   backend_chunk_cb on_chunk, backend_done_cb on_done,
                   void *ud) {
    char err[256] = {0};

    /* messages:system + 历史 */
    cJSON *msgs = cJSON_CreateArray();
    if (cfg->system_prompt && cfg->system_prompt[0]) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "system");
        cJSON_AddStringToObject(m, "content", cfg->system_prompt);
        cJSON_AddItemToArray(msgs, m);
    }
    for (size_t i = 0; i < n_history; i++) {
        if (!history[i].content || !history[i].content[0]) continue;
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role",
                                history[i].role == ROLE_USER ? "user" : "assistant");
        cJSON_AddStringToObject(m, "content", history[i].content);
        cJSON_AddItemToArray(msgs, m);
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model",
                            cfg->model && cfg->model[0] ? cfg->model : "deepseek-v4-pro");
    cJSON_AddItemToObject(body, "messages", msgs);
    cJSON_AddBoolToObject(body, "stream", 1);
    cJSON *thinking = cJSON_CreateObject();
    cJSON_AddStringToObject(thinking, "type",
                            (cfg->deepseek_thinking &&
                             strcmp(cfg->deepseek_thinking, "enabled") == 0)
                                ? "enabled" : "disabled");
    cJSON_AddItemToObject(body, "thinking", thinking);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        if (on_done) on_done(0, "JSON 序列化失败", ud);
        return 0;
    }

    /* URL:base 或 base + /chat/completions */
    const char *base = cfg->deepseek_base_url ? cfg->deepseek_base_url : "https://api.deepseek.com";
    char url[640];
    if (strstr(base, "/chat/completions") ==
        base + strlen(base) - strlen("/chat/completions")) {
        snprintf(url, sizeof(url), "%s", base);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", base);
    }

    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s",
             cfg->deepseek_api_key ? cfg->deepseek_api_key : "");
    const char *hdrs[] = { auth };

    dsctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.on_chunk = on_chunk;
    ctx.ud = ud;

    int rc = net_sse(url, "POST", json, hdrs, 1, ds_on_line, &ctx, err, sizeof(err));
    free(json);

    if (on_done) {
        if (rc != 0) on_done(0, err[0] ? err : "连接中断", ud);
        else if (net_sse_aborted()) on_done(1, NULL, ud); /* 用户主动停止 */
        else if (ctx.err[0]) on_done(0, ctx.err, ud);
        else if (!ctx.done) on_done(0, "响应意外结束", ud);
        else on_done(1, NULL, ud);
    }
    return 0;
}

/* ---------- 模型 ---------- */

/*
 * 动态拉取模型列表(GET /models);失败或为空时回退内置列表。
 * cur 直接取 cfg->model。
 */
int deepseek_list_models(const backend_config_t *cfg,
                         model_option_t **out, size_t *out_n,
                         char *cur, size_t cursz,
                         char *cur_effort, size_t cur_effort_sz,
                         char *err, size_t errsz) {
    *out = NULL;
    *out_n = 0;
    if (cur && cursz)
        snprintf(cur, cursz, "%s", cfg->model ? cfg->model : "");
    if (cur_effort && cur_effort_sz) {
        snprintf(cur_effort, cur_effort_sz, "%s",
                 (cfg->deepseek_thinking &&
                  strcmp(cfg->deepseek_thinking, "enabled") == 0)
                     ? "high" : "low");
    }

    const char *base = cfg->deepseek_base_url ? cfg->deepseek_base_url
                                              : "https://api.deepseek.com";
    char url[640];
    snprintf(url, sizeof(url), "%s/models", base);
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s",
             cfg->deepseek_api_key ? cfg->deepseek_api_key : "");
    const char *hdrs[] = { auth };

    net_buffer_t resp = {0};
    long code = 0;
    char e2[256] = {0};
    if (net_get_json(url, hdrs, 1, &resp, &code, e2, sizeof(e2)) == 0 && code == 200) {
        cJSON *root = cJSON_Parse(resp.data);
        const cJSON *data = root ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
        if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
            size_t n = cJSON_GetArraySize(data);
            model_option_t *list = (model_option_t *)calloc(n, sizeof(*list));
            if (list) {
                size_t k = 0;
                for (size_t i = 0; i < n; i++) {
                    const cJSON *m = cJSON_GetArrayItem(data, i);
                    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(m, "id");
                    if (!cJSON_IsString(mid) || !mid->valuestring[0]) continue;
                    list[k].id = strdup(mid->valuestring);
                    list[k].name = strdup(mid->valuestring);
                    list[k].provider = NULL;
                    k++;
                }
                if (root) cJSON_Delete(root);
                net_buffer_free(&resp);
                if (k > 0) {
                    *out = list;
                    *out_n = k;
                    return 0;
                }
                for (size_t i = 0; i < n; i++) {
                    free(list[i].id);
                    free(list[i].name);
                }
                free(list);
            }
        }
        if (root) cJSON_Delete(root);
    }
    net_buffer_free(&resp);

    /* 回退:内置列表 */
    static const char *builtin[] = { "deepseek-v4-pro", "deepseek-v4-flash" };
    model_option_t *list =
        (model_option_t *)calloc(2, sizeof(*list));
    if (!list) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        list[i].id = strdup(builtin[i]);
        list[i].name = strdup(builtin[i]);
        list[i].provider = NULL;
    }
    *out = list;
    *out_n = 2;
    return 0;
}

int deepseek_apply_model(backend_config_t *cfg, const char *model_id,
                         char *err, size_t errsz) {
    if (!model_id || !model_id[0]) {
        if (err && errsz) snprintf(err, errsz, "模型 id 为空");
        return -1;
    }
    free(cfg->model);
    cfg->model = strdup(model_id);
    return cfg->model ? 0 : -1;
}

const backend_vtable_t backend_vtable_deepseek = {
    .name = "DeepSeek",
    .chat = ds_chat,
};
