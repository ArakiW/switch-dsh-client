#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "backend_harness.h"

extern const backend_vtable_t backend_vtable_harness;
extern const backend_vtable_t backend_vtable_deepseek;

extern int deepseek_list_models(const backend_config_t *cfg,
                                model_option_t **out, size_t *out_n,
                                char *cur, size_t cursz,
                                char *cur_effort, size_t cur_effort_sz,
                                char *err, size_t errsz);
extern int deepseek_apply_model(backend_config_t *cfg, const char *model_id,
                                char *err, size_t errsz);

const backend_vtable_t *backend_resolve(const backend_config_t *cfg) {
    if (cfg && cfg->backend && strcmp(cfg->backend, "deepseek") == 0)
        return &backend_vtable_deepseek;
    return &backend_vtable_harness;
}

int backend_list_models(const backend_config_t *cfg,
                        model_option_t **out, size_t *out_n,
                        char *cur, size_t cursz,
                        char *cur_effort, size_t cur_effort_sz,
                        char *err, size_t errsz) {
    if (cur_effort && cur_effort_sz) cur_effort[0] = '\0';
    if (cfg && cfg->backend && strcmp(cfg->backend, "deepseek") == 0)
        return deepseek_list_models(cfg, out, out_n, cur, cursz,
                                    cur_effort, cur_effort_sz, err, errsz);
    return harness_list_models_ex(cfg, out, out_n, cur, cursz,
                                  cur_effort, cur_effort_sz, err, errsz);
}

void backend_models_free(model_option_t *list, size_t n) {
    harness_models_free(list, n);
}

int backend_apply_model(backend_config_t *cfg, const char *model_id,
                        char *err, size_t errsz) {
    if (cfg && cfg->backend && strcmp(cfg->backend, "deepseek") == 0)
        return deepseek_apply_model(cfg, model_id, err, errsz);

    /* harness:先查目录找到 model 对应的 provider,再 session.selectModel */
    model_option_t *list = NULL;
    size_t n = 0;
    char curm[128] = "";
    char e2[256] = {0};
    if (harness_list_models(cfg, &list, &n, curm, sizeof(curm), e2, sizeof(e2)) != 0) {
        if (err && errsz) snprintf(err, errsz, "%s", e2[0] ? e2 : "读取模型目录失败");
        return -1;
    }
    const char *provider = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].id, model_id) == 0) {
            provider = list[i].provider;
            break;
        }
    }
    int rc = harness_select_model(cfg, provider ? provider : "", model_id, err, errsz);
    harness_models_free(list, n);
    return rc;
}

int backend_apply_effort(backend_config_t *cfg, const char *effort,
                         char *err, size_t errsz) {
    if (cfg && cfg->backend && strcmp(cfg->backend, "deepseek") == 0) {
        /* deepseek:映射到 thinking 开关 */
        const char *t = (effort && strcmp(effort, "high") == 0) ? "enabled" : "disabled";
        free(cfg->deepseek_thinking);
        cfg->deepseek_thinking = strdup(t);
        return cfg->deepseek_thinking ? 0 : -1;
    }
    /* harness:当前模型 + reasoningEffort */
    model_option_t *list = NULL;
    size_t n = 0;
    char curm[128] = "";
    char e2[256] = {0};
    if (harness_list_models(cfg, &list, &n, curm, sizeof(curm), e2, sizeof(e2)) != 0) {
        if (err && errsz) snprintf(err, errsz, "%s", e2[0] ? e2 : "读取模型目录失败");
        return -1;
    }
    const char *provider = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].id, curm) == 0) {
            provider = list[i].provider;
            break;
        }
    }
    int rc = harness_select_model_ex(cfg, provider ? provider : "", curm,
                                     effort, err, errsz);
    harness_models_free(list, n);
    return rc;
}
