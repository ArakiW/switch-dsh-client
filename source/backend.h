#ifndef SWITCH_DSH_BACKEND_H
#define SWITCH_DSH_BACKEND_H

#include <stddef.h>

/*
 * 后端抽象接口。
 *
 * 两个实现:
 *   - backend_harness.c  局域网 DeepSeek Harness(HTTP RPC + SSE)
 *   - backend_deepseek.c DeepSeek 官方 API(HTTPS + SSE)
 *
 * 语音转写为独立能力(见 stt.h),不走本接口。
 */

typedef enum {
    ROLE_USER = 0,
    ROLE_ASSISTANT = 1,
} chat_role_t;

typedef struct {
    chat_role_t role;
    char *content; /* UTF-8,以 \0 结尾 */
} chat_message_t;

/* 后端连接配置(与 config.json 对应,见 config.h 的加载函数) */
typedef struct {
    char *backend;            /* "harness" | "deepseek" */
    char *harness_base_url;   /* 如 http://192.168.1.10:3080 */
    char *deepseek_base_url;  /* 如 https://api.deepseek.com */
    char *deepseek_api_key;
    char *model;              /* deepseek-v4-flash / deepseek-v4-pro 等 */
    char *deepseek_thinking;  /* "enabled" | "disabled"(默认) */
    char *system_prompt;      /* 可为 NULL 或空串 */
    char *stt_url;            /* STT 转写服务地址(空串=禁用语音输入) */
    int   tts_rate;           /* TTS 语速,words-per-minute,默认 175 */
    int   tts_volume;         /* TTS 音量,0-200,默认 100 */
    int   tts_pitch;          /* TTS 音调,0-100,默认 50 */
} backend_config_t;

/*
 * 流式回调。kind:
 *   0 = 正文增量
 *   1 = 思考过程增量(暗色显示)
 *   2 = 工具活动(如 "read"),以活动行显示
 *   3 = 需要用户处理的提示(审批/提问),以醒目色显示
 *   4 = 任务清单(todo/write),整串替换显示
 * 注意:实现可在后台线程中调用这两个回调,调用方(UI)必须自行
 * 把数据投递到主线程(互斥队列),不要在回调里直接碰 SDL 纹理。
 */
typedef void (*backend_chunk_cb)(const char *delta, int kind, void *userdata);
typedef void (*backend_done_cb)(int ok, const char *error, void *userdata);

/*
 * 发送一轮对话。history 为完整消息历史(不含本轮的新助手回复),
 * n_history 为其长度。实现负责:
 *   - 组装请求(含 system_prompt、model 等)
 *   - 流式接收时逐个 delta 调 on_chunk
 *   - 结果一律经 on_done 恰好一次汇报(ok=1 成功;ok=0 且 error 为
 *     可展示的 UTF-8 文本);同步失败也走 on_done(0, err)
 * 返回值:0 正常;-1 保留(实现同步失败时二选一:返回 -1 或回调 on_done,
 * 但不要两者都做)。on_chunk/on_done 可能在后台线程触发,UI 必须自行
 * 投递到主线程。
 */
typedef int (*backend_chat_fn)(const backend_config_t *cfg,
                               const chat_message_t *history, size_t n_history,
                               backend_chunk_cb on_chunk,
                               backend_done_cb on_done, void *userdata);

typedef struct {
    const char *name;     /* 展示名,如 "Harness" / "DeepSeek" */
    backend_chat_fn chat;
} backend_vtable_t;

/* 按 cfg->backend 选择实现,未知值回退到 harness */
const backend_vtable_t *backend_resolve(const backend_config_t *cfg);

/* ---------- 模型选择(对话页直接切换版本) ---------- */

typedef struct {
    char *id;       /* 模型 id,如 deepseek-v4-pro */
    char *name;     /* 展示名 */
    char *provider; /* harness 的 provider(deepseek 后端为 NULL) */
} model_option_t;

/*
 * 取可选模型列表并给当前模型(cur)与当前推理强度(cur_effort,可空)。
 * harness:session.models;deepseek:GET /models(失败回退内置列表)。
 * 成功 0;*out 由 backend_models_free 释放。
 */
int backend_list_models(const backend_config_t *cfg,
                        model_option_t **out, size_t *out_n,
                        char *cur, size_t cursz,
                        char *cur_effort, size_t cur_effort_sz,
                        char *err, size_t errsz);

void backend_models_free(model_option_t *list, size_t n);

/*
 * 应用所选模型:harness -> session.selectModel(会话级);
 * deepseek -> 更新 cfg->model(调用方负责 config_save 持久化)。
 * 成功 0,失败 -1(err 有信息)。
 */
int backend_apply_model(backend_config_t *cfg, const char *model_id,
                        char *err, size_t errsz);

/*
 * 设置推理强度("low"/"high"):
 * harness -> 当前模型 + session.selectModel(reasoningEffort);
 * deepseek -> 映射为 thinking:low=disabled,high=enabled。
 */
int backend_apply_effort(backend_config_t *cfg, const char *effort,
                         char *err, size_t errsz);

#endif /* SWITCH_DSH_BACKEND_H */
