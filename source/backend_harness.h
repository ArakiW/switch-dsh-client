#ifndef SWITCH_DSH_BACKEND_HARNESS_H
#define SWITCH_DSH_BACKEND_HARNESS_H

#include "backend.h"

/* Harness 后端专属:会话(工作区)浏览、切换与历史加载 */

typedef struct {
    char *session_id;
    char *title;        /* 会话标题;空白会话为 "空白会话" */
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

#endif /* SWITCH_DSH_BACKEND_HARNESS_H */
