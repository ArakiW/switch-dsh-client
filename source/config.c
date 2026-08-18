#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "cJSON.h"
#include "util.h"

#define CFG_DIR        "sdmc:/switch/switch-dsh-client"
#define CFG_PATH       CFG_DIR "/config.json"
#define CFG_PATH_ROMFS "romfs:/config.json"
#define KEY_FILE       CFG_DIR "/deepseek_api_key.txt"
#define CFG_MAX_BYTES  (1024 * 1024)

static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* 读整个文件(带 1MB 上限),返回 malloc 的以 \0 结尾的缓冲 */
static char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > CFG_MAX_BYTES) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_size) *out_size = rd;
    return buf;
}

static const char *json_str(const cJSON *root, const char *key) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(it)) return NULL;
    return it->valuestring;
}

/*
 * 读独立的 API Key 文本文件(便于在 PC 上直接编辑,免去在 Switch 上敲键盘)。
 * 内容取首尾空白后返回;文件不存在或为空时返回 NULL。
 */
static char *read_key_file(void) {
    size_t size = 0;
    char *raw = read_file(KEY_FILE, &size);
    if (!raw) return NULL;
    char *t = str_trim(raw);
    if (!t || !t[0]) {
        free(raw);
        return NULL;
    }
    return t;
}

void config_free(backend_config_t *cfg) {
    if (!cfg) return;
    free(cfg->backend);
    free(cfg->harness_base_url);
    free(cfg->deepseek_base_url);
    free(cfg->deepseek_api_key);
    free(cfg->model);
    free(cfg->deepseek_thinking);
    free(cfg->system_prompt);
    free(cfg->stt_url);
    memset(cfg, 0, sizeof(*cfg));
}

void config_apply_defaults(backend_config_t *cfg) {
    cfg->backend          = dupstr("harness");
    cfg->harness_base_url = dupstr("http://192.168.1.10:3080");
    cfg->deepseek_base_url = dupstr("https://api.deepseek.com");
    cfg->deepseek_api_key  = dupstr("");
    cfg->model             = dupstr("deepseek-v4-pro");
    cfg->deepseek_thinking = dupstr("disabled");
    cfg->system_prompt     = dupstr("");
    cfg->stt_url           = dupstr("");
    cfg->tts_rate          = 175;
    cfg->tts_volume        = 100;
    cfg->tts_pitch         = 50;
}

int config_load(backend_config_t *cfg) {
    config_free(cfg);
    config_apply_defaults(cfg);

    size_t size = 0;
    char *raw = read_file(CFG_PATH, &size);
    if (!raw) raw = read_file(CFG_PATH_ROMFS, &size);
    if (!raw) return 1; /* 无配置文件,用默认值 */

    cJSON *root = cJSON_ParseWithLength(raw, size);
    free(raw);
    if (!root) return 1; /* 解析失败,用默认值 */

    const char *s;

    s = json_str(root, "backend");
    if (s && (strcmp(s, "harness") == 0 || strcmp(s, "deepseek") == 0)) {
        free(cfg->backend);
        cfg->backend = dupstr(s);
    }
    if ((s = json_str(root, "harness_base_url")))  { free(cfg->harness_base_url);  cfg->harness_base_url  = dupstr(s); }
    if ((s = json_str(root, "deepseek_base_url"))) { free(cfg->deepseek_base_url); cfg->deepseek_base_url = dupstr(s); }
    if ((s = json_str(root, "deepseek_api_key")))  { free(cfg->deepseek_api_key);  cfg->deepseek_api_key  = dupstr(s); }
    if ((s = json_str(root, "model")))             { free(cfg->model);             cfg->model             = dupstr(s); }
    if ((s = json_str(root, "deepseek_thinking"))) { free(cfg->deepseek_thinking); cfg->deepseek_thinking = dupstr(s); }
    if ((s = json_str(root, "system_prompt")))     { free(cfg->system_prompt);     cfg->system_prompt     = dupstr(s); }
    if ((s = json_str(root, "stt_url")))           { free(cfg->stt_url);           cfg->stt_url           = dupstr(s); }
    {
        cJSON *num;
        num = cJSON_GetObjectItemCaseSensitive(root, "tts_rate");
        if (cJSON_IsNumber(num)) cfg->tts_rate = num->valueint;
        num = cJSON_GetObjectItemCaseSensitive(root, "tts_volume");
        if (cJSON_IsNumber(num)) cfg->tts_volume = num->valueint;
        num = cJSON_GetObjectItemCaseSensitive(root, "tts_pitch");
        if (cJSON_IsNumber(num)) cfg->tts_pitch = num->valueint;
    }

    cJSON_Delete(root);

    /* 独立的 key.txt 优先:存在且非空则覆盖 config.json 里的值 */
    char *keyfile = read_key_file();
    if (keyfile) {
        free(cfg->deepseek_api_key);
        cfg->deepseek_api_key = keyfile;
    }
    return 0;
}

int config_save(const backend_config_t *cfg) {
    mkdir(CFG_DIR, 0777); /* 目录已存在时会失败,忽略 */

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddStringToObject(root, "backend",          cfg->backend);
    cJSON_AddStringToObject(root, "harness_base_url", cfg->harness_base_url);
    cJSON_AddStringToObject(root, "deepseek_base_url", cfg->deepseek_base_url);
    cJSON_AddStringToObject(root, "deepseek_api_key", cfg->deepseek_api_key);
    cJSON_AddStringToObject(root, "model",            cfg->model);
    cJSON_AddStringToObject(root, "deepseek_thinking", cfg->deepseek_thinking);
    cJSON_AddStringToObject(root, "system_prompt",    cfg->system_prompt);
    cJSON_AddStringToObject(root, "stt_url",          cfg->stt_url);
    cJSON_AddNumberToObject(root, "tts_rate",          cfg->tts_rate);
    cJSON_AddNumberToObject(root, "tts_volume",        cfg->tts_volume);
    cJSON_AddNumberToObject(root, "tts_pitch",         cfg->tts_pitch);

    char *pretty = cJSON_Print(root);
    cJSON_Delete(root);
    if (!pretty) return -1;

    FILE *f = fopen(CFG_PATH, "wb");
    if (!f) { free(pretty); return -1; }
    fwrite(pretty, 1, strlen(pretty), f);
    fclose(f);
    free(pretty);

    /* 同步 API Key 到独立文本文件(设置里改完也能在 PC 上看到/再编辑) */
    FILE *kf = fopen(KEY_FILE, "wb");
    if (kf) {
        const char *key = cfg->deepseek_api_key ? cfg->deepseek_api_key : "";
        fwrite(key, 1, strlen(key), kf);
        fclose(kf);
    }
    return 0;
}

int config_reload_key(backend_config_t *cfg, char *msg, size_t msgsz) {
    char *keyfile = read_key_file();
    if (!keyfile) {
        if (msg && msgsz)
            snprintf(msg, msgsz,
                     "key.txt 不存在或为空\n(sdmc:/switch/switch-dsh-client/deepseek_api_key.txt)");
        return 0;
    }
    free(cfg->deepseek_api_key);
    cfg->deepseek_api_key = keyfile;
    if (msg && msgsz) snprintf(msg, msgsz, "已加载");
    return 1;
}

void config_check(const backend_config_t *cfg, char *err, size_t errsz) {
    if (errsz == 0) return;
    err[0] = '\0';

    if (!cfg->harness_base_url || !cfg->harness_base_url[0]) {
        snprintf(err, errsz, "harness_base_url 为空");
    } else if (strncmp(cfg->harness_base_url, "http://", 7) != 0 &&
               strncmp(cfg->harness_base_url, "https://", 8) != 0) {
        snprintf(err, errsz, "harness_base_url 需要 http:// 或 https:// 前缀");
    } else if (strcmp(cfg->backend, "deepseek") == 0 &&
               (!cfg->deepseek_api_key || !cfg->deepseek_api_key[0])) {
        snprintf(err, errsz, "DeepSeek 模式需要填写 API Key");
    } else if (strcmp(cfg->backend, "deepseek") == 0 &&
               (!cfg->deepseek_base_url ||
                strncmp(cfg->deepseek_base_url, "https://", 8) != 0)) {
        snprintf(err, errsz, "deepseek_base_url 需要 https:// 前缀");
    }
}
