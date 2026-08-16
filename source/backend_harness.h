#ifndef SWITCH_DSH_BACKEND_HARNESS_H
#define SWITCH_DSH_BACKEND_HARNESS_H

#include "backend.h"

/* Harness 后端专属:会话(工作区)浏览、切换、历史与模型 */

typedef struct {
    char *session_id;
    char *title;        /* 会话标题;空白会话为 "空白会话" */
    char *cwd;          /* 会话所在目录(用于按工作区过滤) */
    int running;        /* 是否有回合在跑 */
    long long updated_at; /* epoch ms */
} harness_session_t;

/* 列出会话(按 updatedAt 新→旧)。成功 0;*out_list 由 harness_sessions_free 释放。 */
int harness_list_sessions(const backend_config_t *cfg,
                          harness_session_t **out_list, size_t *out_n,
                          char *err, size_t errsz);

void harness_sessions_free(harness_session_t *list, size_t n);

/* 切换会话:session_id 为 NULL/空串 = 新建会话。之后的 prompt 都发往该会话。 */
int harness_use_session(const backend_config_t *cfg, const char *session_id,
                        char *err, size_t errsz);

/* 拉当前会话的最近历史(只取 user/assistant 的 text 内容)。 */
int harness_fetch_history(const backend_config_t *cfg,
                          chat_message_t **out_msgs, size_t *out_n,
                          char *err, size_t errsz);

/* 翻页版:before_seq>0 时取 seq < before_seq 的更早消息;
 * first_seq 输出返回消息中最早的 seq(无消息时为 -1)。 */
int harness_fetch_history_ex(const backend_config_t *cfg, long long before_seq,
                             chat_message_t **out_msgs, size_t *out_n,
                             long long *first_seq, char *err, size_t errsz);

/* ---------- 会话操作 ---------- */

/* 取消当前会话正在运行的回合 */
int harness_cancel(const backend_config_t *cfg, char *err, size_t errsz);

int harness_rename_session(const backend_config_t *cfg, const char *session_id,
                           const char *title, char *err, size_t errsz);

/* 分叉会话,forked_id 输出子会话 id */
int harness_fork_session(const backend_config_t *cfg, const char *session_id,
                         char *forked_id, size_t forked_sz,
                         char *err, size_t errsz);

typedef struct {
    char *session_id;
    char *snippet;
} harness_search_hit_t;

int harness_search_sessions(const backend_config_t *cfg, const char *query,
                            harness_search_hit_t **out, size_t *out_n,
                            char *err, size_t errsz);

void harness_search_free(harness_search_hit_t *list, size_t n);

/* ---------- 工作区 ---------- */

typedef struct {
    char *workspace_id;
    char *path;
    char *title;
    size_t session_count;
} harness_workspace_t;

int harness_list_workspaces(const backend_config_t *cfg,
                            harness_workspace_t **out_list, size_t *out_n,
                            char *err, size_t errsz);

void harness_workspaces_free(harness_workspace_t *list, size_t n);

/* 采纳一个目录为新工作区(workspace.create) */
int harness_create_workspace(const backend_config_t *cfg, const char *path,
                             char *err, size_t errsz);

int harness_rename_workspace(const backend_config_t *cfg, const char *workspace_id,
                             const char *title, char *err, size_t errsz);

int harness_delete_workspace(const backend_config_t *cfg, const char *workspace_id,
                             char *err, size_t errsz);

/* 排序:把 workspace_id 移到 before_workspace_id 之前(NULL = 移到末尾) */
int harness_reorder_workspace(const backend_config_t *cfg, const char *workspace_id,
                              const char *before_workspace_id,
                              char *err, size_t errsz);

/* 在指定工作区新建会话并切换过去(workspace_id 为空 = 默认位置) */
int harness_new_session_in(const backend_config_t *cfg, const char *workspace_id,
                           char *err, size_t errsz);

/* ---------- 模型 ---------- */

/* 会话级模型目录 + 当前模型。成功 0;*out 由 harness_models_free 释放。 */
int harness_list_models(const backend_config_t *cfg,
                        model_option_t **out, size_t *out_n,
                        char *cur, size_t cursz, char *err, size_t errsz);

/* 同上,并输出当前推理强度(cur_effort,可空) */
int harness_list_models_ex(const backend_config_t *cfg,
                           model_option_t **out, size_t *out_n,
                           char *cur, size_t cursz,
                           char *cur_effort, size_t cur_effort_sz,
                           char *err, size_t errsz);

void harness_models_free(model_option_t *list, size_t n);

/* 当前活动会话 id(未创建时 NULL) */
const char *harness_current_session(void);

/* 会话级选模型(session.selectModel) */
int harness_select_model(const backend_config_t *cfg,
                         const char *provider, const char *model,
                         char *err, size_t errsz);

/* effort 可为 NULL(不变)或 "low"/"high" */
int harness_select_model_ex(const backend_config_t *cfg,
                            const char *provider, const char *model,
                            const char *effort, char *err, size_t errsz);

#endif /* SWITCH_DSH_BACKEND_HARNESS_H */
