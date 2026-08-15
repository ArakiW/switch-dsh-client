#ifndef SWITCH_DSH_CONFIG_H
#define SWITCH_DSH_CONFIG_H

#include "backend.h"

/* 配置读写。优先 sdmc:/switch/switch-dsh-client/config.json,
 * 其次 romfs:/config.json,都没有则用内置默认值。 */

void config_apply_defaults(backend_config_t *cfg); /* 填默认值(覆盖) */
int  config_load(backend_config_t *cfg);           /* 0=读到了配置文件,1=用了默认值 */
int  config_save(const backend_config_t *cfg);     /* 写回 SD 卡,0 成功 */
void config_free(backend_config_t *cfg);

/* 校验配置,错误描述写入 err(errsz 建议 >= 160);无错误时 err 为空串 */
void config_check(const backend_config_t *cfg, char *err, size_t errsz);

#endif /* SWITCH_DSH_CONFIG_H */
