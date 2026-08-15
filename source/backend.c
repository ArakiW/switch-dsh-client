#include <string.h>

#include "backend.h"

extern const backend_vtable_t backend_vtable_harness;
extern const backend_vtable_t backend_vtable_deepseek;

const backend_vtable_t *backend_resolve(const backend_config_t *cfg) {
    if (cfg && cfg->backend && strcmp(cfg->backend, "deepseek") == 0)
        return &backend_vtable_deepseek;
    return &backend_vtable_harness;
}
