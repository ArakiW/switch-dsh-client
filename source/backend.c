#include <stdio.h>
#include <string.h>

#include "backend.h"
#include "backend_harness.h"

extern const backend_vtable_t backend_vtable_harness;
extern const backend_vtable_t backend_vtable_deepseek;

extern int deepseek_list_models(const backend_config_t *cfg,
                                model_option_t **out, size_t *out_n,
                                char *cur, size_t cursz, char *err, size_t errsz);
extern int deepseek_apply_model(backend_config_t *cfg, const char *model_id,
                                char *err, size_t errsz);

const backend_vtable_t *backend_resolve(const backend_config_t *cfg) {
    if (cfg && cfg->backend && strcmp(cfg->backend, "deepseek") == 0)
        return &backend_vtable_deepseek;
    return &backend_vtable_harness;
}

int backend_list_models(const backend_config_t *cfg,
                        model_option_t **out, size_t *out_n,
                        char *cur, size_t cursz, char *err, size_t errsz) {
    if (cfg && cfg->backend && strcmp(cfg->backend, "deepseek") == 0)
        return deepseek_list_models(cfg, out, out_n, cur, cursz, err, errsz);
    return harness_list_models(cfg, out, out_n, cur, cursz, err, errsz);
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
