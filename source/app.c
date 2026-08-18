#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL_image.h>
#include <sys/stat.h>
#include <switch.h>

#include "app.h"
#include "backend.h"
#include "backend_harness.h"
#include "cJSON.h"
#include "config.h"
#include "net.h"
#include "stt.h"
#include "textinput.h"
#include "tts.h"
#include "util.h"

#define WIN_W        1280
#define WIN_H        720
#define HEADER_H     64
#define FOOTER_H     44
#define MAX_MSGS     256
#define MAX_LINE_LEN 4096
#define MAX_LINES    128
#define MAX_QUEUE    512
#define SB_W         360  /* 侧栏宽 */
#define SB_ROWS_MAX  512
#define HIST_HARNESS  "sdmc:/switch/switch-dsh-client/history_harness.json"
#define HIST_DEEPSEEK "sdmc:/switch/switch-dsh-client/history_deepseek.json"

typedef struct {
    char *text;   /* 正文, malloc, UTF-8 */
    char *think;  /* 思考过程(可 NULL) */
    char *tools;  /* 工具活动行(可 NULL) */
    char *notice; /* 处理提示(审批/提问,可 NULL) */
    int role;     /* ROLE_USER / ROLE_ASSISTANT */
    int done;     /* 助手消息是否已结束(流式标记) */
    SDL_Texture *tex; /* 预渲染纹理缓存 */
    int tex_w;
    int tex_h;
    int dirty;        /* 内容变化,需重渲染纹理 */
} msg_t;

/* 工作线程 -> 主线程 事件队列 */
typedef struct {
    int kind;   /* 1=正文增量 2=完成 3=失败 */
    char *text; /* kind==1 增量文本;kind==3 错误文本;kind==2 NULL */
} app_event_t;

typedef enum {
    SCREEN_CHOICE, SCREEN_CHAT, SCREEN_SETTINGS,
    SCREEN_SESSIONS, SCREEN_WORKSPACES, SCREEN_WS_SESSIONS, SCREEN_MODELS,
    SCREEN_SEARCH,
} screen_t;

static SDL_Window   *g_win   = NULL;
static SDL_Renderer *g_ren   = NULL;
static TTF_Font *g_font      = NULL; /* 正文 */
static TTF_Font *g_font_title = NULL;
static TTF_Font *g_font_hint = NULL;

static backend_config_t g_cfg;
static msg_t g_msgs[MAX_MSGS];
static int g_nmsgs = 0;
static PadState g_pad;
static char g_input[4096];
static int g_want_exit = 0;
static int g_dirty = 1;
static int g_voice_enabled = 1; /* 语音朗读开关(仅 Harness 后端生效) */
static screen_t g_screen = SCREEN_CHOICE;
static int g_set_idx = 0;
static int g_choice_idx = 0;

/* 会话列表 */
static harness_session_t *g_sessions = NULL;
static size_t g_sessions_n = 0;
static int g_sess_idx = 0;
static int g_sess_loaded = 0;
static char g_sess_err[256];
static int g_sess_from_startup = 0;
static screen_t g_sess_back = SCREEN_CHAT;

/* 工作区 */
static harness_workspace_t *g_wss = NULL;
static size_t g_wss_n = 0;
static int g_ws_loaded = 0;
static int g_ws_idx = 0;
static char g_ws_err[256];
static int g_ws_confirm = -1; /* 删除确认:-1 = 无,>=0 = 该行待确认 */

/* 工作区内会话 */
static harness_session_t *g_wss_sessions = NULL;
static size_t g_wss_sessions_n = 0;
static int g_wss_loaded = 0;
static int g_wss_idx = 0;
static char g_wss_err[256];

/* 模型选择 */
static model_option_t *g_models = NULL;
static size_t g_models_n = 0;
static int g_models_loaded = 0;
static int g_model_idx = 0;
static char g_models_err[256];
static char g_cur_model[128] = "";

/* 后台请求 */
static SDL_mutex *g_q_mtx = NULL;
static app_event_t g_queue[MAX_QUEUE];
static int g_q_head = 0, g_q_tail = 0, g_q_count = 0;
static volatile int g_worker_busy = 0;
static int g_stream_think = 0; /* 当前流式处于思考阶段 */

/* 任务清单 / 滚动 / 历史翻页 / 推理强度 */
static char g_todos_str[512];
static int g_scroll_offset = 0; /* 相对底部的上滚像素(0=贴底) */
static long long g_first_seq = -1;
static char g_cur_effort[16] = "";

/* 双后端独立会话缓冲(切换/重启不丢) */
static msg_t g_buf_h[MAX_MSGS];
static msg_t g_buf_d[MAX_MSGS];
static int g_bufn_h = 0, g_bufn_d = 0;
static int g_bufscroll_h = 0, g_bufscroll_d = 0;
static long long g_buffirst_h = -1, g_buffirst_d = -1;

/* 侧栏(默认显示,对齐桌面版) */
typedef struct {
    int kind; /* 0 新建 1 会话 4 分组头 2 工作区管理 3 设置 */
    int idx;  /* kind==1 时是 g_sessions 下标 */
    int fy;
    int fh;
} sb_row_t;
static sb_row_t g_sb_rows[SB_ROWS_MAX];
static int g_sb_rows_n = 0;
static int g_sb_focus_of[SB_ROWS_MAX]; /* 行 -> 焦点序号(-1 不可聚焦) */
static int g_sb_row_of[SB_ROWS_MAX];   /* 焦点序号 -> 行 */
static int g_sb_focus_n = 0;
static int g_sb_loaded = 0;
static int g_focus = 0; /* 0=对话 1=侧栏 */
static int g_sb_idx = 0;
static char g_sess_title[128] = "新会话";
static screen_t g_prev_screen = SCREEN_CHOICE;

/* 侧栏后台加载(不阻塞主线程) */
static volatile int g_sb_loading = 0;
static volatile int g_sb_ready = 0;
static harness_workspace_t *g_sb_tmp_wss = NULL;
static size_t g_sb_tmp_wss_n = 0;
static harness_session_t *g_sb_tmp_sess = NULL;
static size_t g_sb_tmp_sess_n = 0;
static char g_sb_err[256];

/* 配色对齐 DeepSeek Harness Web 暗色主题(design-platform.css) */
static const SDL_Color COL_BG    = {  21,  21,  23, 255 }; /* bg-base (bluish-950) */
static const SDL_Color COL_SURF  = {  27,  27,  28, 255 }; /* sidebar/layer-1 (bluish-900) */
static const SDL_Color COL_SURF2 = {  53,  54,  56, 255 }; /* layer-2/hover (bluish-800) */
static const SDL_Color COL_BRAND = {  86, 134, 254, 255 }; /* DeepSeek 蓝 (deepseek-450) */
static const SDL_Color COL_TEXT  = { 235, 238, 242, 255 }; /* label-primary (bluish-100) */
static const SDL_Color COL_TEXT2 = { 151, 157, 166, 255 }; /* label-secondary (bluish-500) */
static const SDL_Color COL_TEXT3 = { 129, 133, 140, 255 }; /* label-tertiary (bluish-600) */
static const SDL_Color COL_GREEN = {  34, 197,  94, 255 }; /* success (green-500) */
static const SDL_Color COL_RED   = { 239,  68,  68, 255 }; /* error (red-500) */
static const SDL_Color COL_WHITE = { 255, 255, 255, 255 };

/* 兼容旧命名 */
#define COL_HEADER COL_SURF
#define COL_USER   COL_BRAND
#define COL_ASST   COL_SURF
#define COL_HINT   COL_TEXT3
#define COL_ACCENT COL_BRAND

/* 资源加载:本机 Windows 构建经 objcopy 把字体嵌入 rodata(weak 符号);
 * 标准 devkitPro(CI)构建走 romfs。符号未嵌入时为 NULL。 */
__attribute__((weak)) extern const unsigned char _binary_NotoSansCJKsc_Regular_otf_start[];
__attribute__((weak)) extern const unsigned char _binary_NotoSansCJKsc_Regular_otf_end[];
__attribute__((weak)) extern const unsigned char _binary_logo_png_start[];
__attribute__((weak)) extern const unsigned char _binary_logo_png_end[];

static SDL_Texture *g_logo_tex = NULL;

static TTF_Font *open_font(int ptsize) {
    TTF_Font *f = TTF_OpenFont("romfs:/fonts/NotoSansCJKsc-Regular.otf", ptsize);
    if (f) return f;
    if (_binary_NotoSansCJKsc_Regular_otf_start != NULL) {
        size_t size = (size_t)(_binary_NotoSansCJKsc_Regular_otf_end -
                               _binary_NotoSansCJKsc_Regular_otf_start);
        if (size > 0 && size < 0x7FFFFFFF) {
            SDL_RWops *rw = SDL_RWFromConstMem(_binary_NotoSansCJKsc_Regular_otf_start,
                                               (int)size);
            if (rw) return TTF_OpenFontRW(rw, 1, ptsize);
        }
    }
    return NULL;
}

/* ---------- 消息列表 ---------- */

#define MAX_TEX_CACHE 32
static int g_tex_count = 0;

static void msg_tex_destroy(msg_t *m) {
    if (m->tex) {
        SDL_DestroyTexture(m->tex);
        m->tex = NULL;
        g_tex_count--;
    }
}

static void add_msg(int role, const char *text, int done) {
    if (!text) text = "";
    if (g_nmsgs >= MAX_MSGS) {
        msg_tex_destroy(&g_msgs[0]);
        free(g_msgs[0].text);
        free(g_msgs[0].think);
        free(g_msgs[0].tools);
        free(g_msgs[0].notice);
        memmove(&g_msgs[0], &g_msgs[1], sizeof(msg_t) * (MAX_MSGS - 1));
        g_nmsgs = MAX_MSGS - 1;
    }
    msg_t *m = &g_msgs[g_nmsgs++];
    m->text = malloc(strlen(text) + 1);
    if (m->text) {
        strcpy(m->text, text);
        utf8_sanitize(m->text); /* 剥离字体无法渲染的字符 */
    }
    m->think = NULL;
    m->tools = NULL;
    m->notice = NULL;
    m->role = role;
    m->done = done;
    m->tex = NULL;
    m->tex_w = 0;
    m->tex_h = 0;
    m->dirty = 1;
    g_dirty = 1;
}

/* ---------- 双后端会话缓冲与本地持久化 ---------- */

static void buf_clear(msg_t *dst, int *dn) {
    for (int i = 0; i < *dn; i++) {
        msg_tex_destroy(&dst[i]);
        free(dst[i].text);
        free(dst[i].think);
        free(dst[i].tools);
        free(dst[i].notice);
    }
    *dn = 0;
}

static void store_active(msg_t *dst, int *dn, int *ds, long long *df) {
    buf_clear(dst, dn);
    *dn = g_nmsgs;
    for (int i = 0; i < g_nmsgs; i++) dst[i] = g_msgs[i]; /* 所有权转移 */
    *ds = g_scroll_offset;
    *df = g_first_seq;
    g_nmsgs = 0;
    memset(g_msgs, 0, sizeof(g_msgs));
    g_dirty = 1;
}

static void restore_active(msg_t *src, int *sn, int ss, long long sf) {
    for (int i = 0; i < g_nmsgs; i++) {
        msg_tex_destroy(&g_msgs[i]);
        free(g_msgs[i].text);
        free(g_msgs[i].think);
        free(g_msgs[i].tools);
        free(g_msgs[i].notice);
    }
    g_nmsgs = *sn;
    for (int i = 0; i < *sn; i++) g_msgs[i] = src[i];
    *sn = 0;
    memset(src, 0, sizeof(g_msgs));
    g_scroll_offset = ss;
    g_first_seq = sf;
    g_dirty = 1;
}

static void history_save(const char *path, const msg_t *msgs, int n) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;
    for (int i = 0; i < n; i++) {
        if (!msgs[i].text || !msgs[i].text[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "role", msgs[i].role);
        cJSON_AddStringToObject(o, "text", msgs[i].text);
        cJSON_AddItemToArray(arr, o);
    }
    char *j = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!j) return;
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(j, 1, strlen(j), f);
        fclose(f);
    }
    free(j);
}

/* 导出当前对话到 SD 卡 */
static char g_export_msg[256];

static void export_conversation(void) {
    mkdir("sdmc:/switch/switch-dsh-client", 0777);
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char path[256];
    snprintf(path, sizeof(path),
             "sdmc:/switch/switch-dsh-client/export_%04d%02d%02d_%02d%02d%02d.json",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    history_save(path, g_msgs, g_nmsgs);
    snprintf(g_export_msg, sizeof(g_export_msg), "已导出: %s", path);
}

static void history_load(const char *path, msg_t *dst, int *dn) {
    buf_clear(dst, dn);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return;
    }
    fseek(f, 0, SEEK_SET);
    char *raw = (char *)malloc((size_t)sz + 1);
    if (!raw) {
        fclose(f);
        return;
    }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = '\0';
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return;
    if (cJSON_IsArray(root)) {
        size_t n = cJSON_GetArraySize(root);
        for (size_t i = 0; i < n && *dn < MAX_MSGS; i++) {
            const cJSON *o = cJSON_GetArrayItem(root, i);
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(o, "role");
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "text");
            if (cJSON_IsString(t) && t->valuestring[0]) {
                msg_t *m = &dst[(*dn)++];
                memset(m, 0, sizeof(*m));
                m->text = strdup(t->valuestring);
                if (m->text) utf8_sanitize(m->text);
                m->role = cJSON_IsNumber(r) && r->valueint == 1 ? ROLE_ASSISTANT : ROLE_USER;
                m->done = 1;
                m->dirty = 1;
            }
        }
    }
    cJSON_Delete(root);
}

static void save_current_history(void) {
    const char *p = strcmp(g_cfg.backend, "deepseek") == 0 ? HIST_DEEPSEEK : HIST_HARNESS;
    history_save(p, g_msgs, g_nmsgs);
}

/* ---------- 事件队列 ---------- */

static void push_event(int kind, const char *text) {
    SDL_LockMutex(g_q_mtx);
    if (g_q_count >= MAX_QUEUE) {
        SDL_UnlockMutex(g_q_mtx);
        return; /* 丢弃(理论上到不了) */
    }
    app_event_t *e = &g_queue[g_q_tail];
    e->kind = kind;
    e->text = text ? strdup(text) : NULL;
    g_q_tail = (g_q_tail + 1) % MAX_QUEUE;
    g_q_count++;
    SDL_UnlockMutex(g_q_mtx);
}

static int pop_event(app_event_t *out) {
    SDL_LockMutex(g_q_mtx);
    if (g_q_count == 0) {
        SDL_UnlockMutex(g_q_mtx);
        return 0;
    }
    *out = g_queue[g_q_head];
    g_q_head = (g_q_head + 1) % MAX_QUEUE;
    g_q_count--;
    SDL_UnlockMutex(g_q_mtx);
    return 1;
}

/* ---------- 后台线程 ---------- */

static void backend_chunk(const char *delta, int kind, void *ud) {
    (void)ud;
    /* kind: 0 正文 1 思考 2 工具 3 提示 4 任务 */
    static const int map[] = { 1, 4, 5, 6, 7 };
    if (kind >= 0 && kind <= 4) push_event(map[kind], delta);
}

static void backend_done(int ok, const char *error, void *ud) {
    (void)ud;
    if (ok) push_event(2, NULL);
    else push_event(3, error ? error : "未知错误");
}

typedef struct {
    backend_config_t cfg;
    chat_message_t *history;
    size_t n_history;
} worker_arg_t;

static void cfg_clone(backend_config_t *dst, const backend_config_t *src) {
    memset(dst, 0, sizeof(*dst));
    dst->backend          = strdup(src->backend ? src->backend : "");
    dst->harness_base_url = strdup(src->harness_base_url ? src->harness_base_url : "");
    dst->deepseek_base_url = strdup(src->deepseek_base_url ? src->deepseek_base_url : "");
    dst->deepseek_api_key  = strdup(src->deepseek_api_key ? src->deepseek_api_key : "");
    dst->model             = strdup(src->model ? src->model : "");
    dst->deepseek_thinking = strdup(src->deepseek_thinking ? src->deepseek_thinking : "");
    dst->system_prompt     = strdup(src->system_prompt ? src->system_prompt : "");
    dst->stt_url           = strdup(src->stt_url ? src->stt_url : "");
    dst->tts_rate          = src->tts_rate;
    dst->tts_volume        = src->tts_volume;
    dst->tts_pitch         = src->tts_pitch;
}

static int worker_fn(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    const backend_vtable_t *be = backend_resolve(&wa->cfg);
    printf("worker: backend=%s\n", be->name);
    be->chat(&wa->cfg, wa->history, wa->n_history, backend_chunk, backend_done, NULL);

    for (size_t i = 0; i < wa->n_history; i++) free(wa->history[i].content);
    free(wa->history);
    config_free(&wa->cfg);
    free(wa);
    g_worker_busy = 0;
    return 0;
}

static void start_worker(void) {
    worker_arg_t *wa = (worker_arg_t *)calloc(1, sizeof(*wa));
    if (!wa) return;
    cfg_clone(&wa->cfg, &g_cfg);

    /* 拷贝历史(不含末尾的空助手占位) */
    size_t n = (size_t)g_nmsgs;
    if (n > 0 && g_msgs[n - 1].role == ROLE_ASSISTANT && !g_msgs[n - 1].done) n--;
    wa->history = (chat_message_t *)calloc(n ? n : 1, sizeof(chat_message_t));
    if (!wa->history) {
        config_free(&wa->cfg);
        free(wa);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        wa->history[i].role = (chat_role_t)g_msgs[i].role;
        wa->history[i].content = strdup(g_msgs[i].text ? g_msgs[i].text : "");
    }
    wa->n_history = n;

    g_worker_busy = 1;
    SDL_Thread *th = SDL_CreateThread(worker_fn, "backend", wa);
    if (th) SDL_DetachThread(th);
    else {
        for (size_t i = 0; i < n; i++) free(wa->history[i].content);
        free(wa->history);
        config_free(&wa->cfg);
        free(wa);
        g_worker_busy = 0;
    }
}

/* ---- STT(语音输入)后台转写 ---- */
static volatile int g_stt_busy = 0;

static int stt_thread_fn(void *arg) {
    char *url = (char *)arg;
    char text[4096];
    char err[256];
    if (stt_transcribe(url, text, sizeof(text), err, sizeof(err)) == 0)
        push_event(8, text);
    else
        push_event(9, err[0] ? err : "转写失败");
    free(url);
    g_stt_busy = 0;
    return 0;
}

static void stt_transcribe_async(void) {
    if (g_stt_busy) return;
    if (!g_cfg.stt_url || !g_cfg.stt_url[0]) {
        add_msg(ROLE_ASSISTANT, "未配置 STT 地址(设置 → STT 服务地址)", 1);
        return;
    }
    g_stt_busy = 1;
    char *url = strdup(g_cfg.stt_url);
    if (!url) { g_stt_busy = 0; return; }
    SDL_Thread *th = SDL_CreateThread(stt_thread_fn, "stt", url);
    if (th) SDL_DetachThread(th);
    else { free(url); g_stt_busy = 0; }
}

static void drain_queue(void) {
    app_event_t ev;
    while (pop_event(&ev)) {
        if (ev.kind == 1) {
            if (g_nmsgs > 0) {
                msg_t *m = &g_msgs[g_nmsgs - 1];
                if (m->role == ROLE_ASSISTANT && !m->done) {
                    size_t old = strlen(m->text);
                    size_t add = strlen(ev.text ? ev.text : "");
                    char *nt = (char *)realloc(m->text, old + add + 1);
                    if (nt) {
                        memcpy(nt + old, ev.text ? ev.text : "", add + 1);
                        m->text = nt;
                    }
                    m->dirty = 1;
                }
            }
            g_stream_think = 0;
            g_dirty = 1;
        } else if (ev.kind == 4) {
            /* 思考增量 */
            if (g_nmsgs > 0) {
                msg_t *m = &g_msgs[g_nmsgs - 1];
                if (m->role == ROLE_ASSISTANT && !m->done) {
                    size_t old = m->think ? strlen(m->think) : 0;
                    size_t add = strlen(ev.text ? ev.text : "");
                    char *nt = (char *)realloc(m->think, old + add + 1);
                    if (nt) {
                        if (!m->think) nt[0] = '\0';
                        memcpy(nt + old, ev.text ? ev.text : "", add + 1);
                        m->think = nt;
                    }
                    m->dirty = 1;
                }
            }
            g_stream_think = 1;
            g_dirty = 1;
        } else if (ev.kind == 5 || ev.kind == 6) {
            /* 工具活动(5)/处理提示(6) */
            if (g_nmsgs > 0) {
                msg_t *m = &g_msgs[g_nmsgs - 1];
                if (m->role == ROLE_ASSISTANT && !m->done) {
                    char **field = (ev.kind == 5) ? &m->tools : &m->notice;
                    char buf[640];
                    if (ev.kind == 5)
                        snprintf(buf, sizeof(buf), "· 工具:%s", ev.text ? ev.text : "");
                    else
                        snprintf(buf, sizeof(buf), "%s", ev.text ? ev.text : "");
                    size_t old = *field ? strlen(*field) : 0;
                    size_t add = strlen(buf);
                    char *nt = (char *)realloc(*field, old + add + 2);
                    if (nt) {
                        char *dst = nt + old;
                        if (old > 0) *dst++ = '\n';
                        memcpy(dst, buf, add + 1);
                        *field = nt;
                    }
                    m->dirty = 1;
                }
            }
            g_dirty = 1;
        } else if (ev.kind == 7) {
            snprintf(g_todos_str, sizeof(g_todos_str), "%s", ev.text ? ev.text : "");
            g_dirty = 1;
        } else if (ev.kind == 2) {
            if (g_nmsgs > 0) {
                g_msgs[g_nmsgs - 1].done = 1;
                if (g_msgs[g_nmsgs - 1].text[0] == '\0')
                    strcpy(g_msgs[g_nmsgs - 1].text, "(无回复内容)");
                g_msgs[g_nmsgs - 1].dirty = 1;
            }
            g_stream_think = 0;
            g_dirty = 1;
            save_current_history();
            /* 助手回复完成:语音开启 + TTS 可用时自动朗读(本地合成,不限后端) */
            if (g_voice_enabled && tts_available() && g_nmsgs > 0 &&
                g_msgs[g_nmsgs - 1].text[0] != '\0') {
                tts_speak(g_msgs[g_nmsgs - 1].text);
            }
        } else if (ev.kind == 3) {
            if (g_nmsgs > 0) {
                msg_t *m = &g_msgs[g_nmsgs - 1];
                size_t old = strlen(m->text);
                char buf[640];
                snprintf(buf, sizeof(buf), "%s[错误] %s",
                         old == 0 ? "" : "\n", ev.text ? ev.text : "");
                size_t add = strlen(buf);
                char *nt = (char *)realloc(m->text, old + add + 1);
                if (nt) {
                    memcpy(nt + old, buf, add + 1);
                    m->text = nt;
                }
                m->done = 1;
                m->dirty = 1;
            }
            g_dirty = 1;
            save_current_history();
        } else if (ev.kind == 8) {
            /* STT 转写成功:作为用户消息发送 */
            if (!g_worker_busy && ev.text && ev.text[0]) {
                add_msg(ROLE_USER, ev.text, 1);
                add_msg(ROLE_ASSISTANT, "", 0);
                start_worker();
            }
            g_dirty = 1;
        } else if (ev.kind == 9) {
            add_msg(ROLE_ASSISTANT, ev.text ? ev.text : "语音输入失败", 1);
            g_dirty = 1;
        }
        free(ev.text);
    }
}

/* ---------- 文本换行 ---------- */

typedef struct {
    size_t off; /* 原串字节偏移 */
    size_t len; /* 本行字节数 */
} wrap_line_t;

/* 贪心换行:返回行数;lines 为 NULL 时只测量 */
static int wrap_text(TTF_Font *font, int maxw, const char *text,
                     wrap_line_t *lines, int max_lines) {
    if (!text || !*text) return 0;
    const char *p = text;
    int n = 0;
    char buf[MAX_LINE_LEN];
    while (*p && n < max_lines) {
        size_t acc = 0;
        while (p[acc]) {
            size_t clen = utf8_next_len(p + acc);
            if (acc + clen + 1 >= sizeof(buf)) break;
            memcpy(buf + acc, p + acc, clen);
            buf[acc + clen] = '\0';
            int w = 0, h = 0;
            TTF_SizeUTF8(font, buf, &w, &h);
            if (w > maxw && acc > 0) break;
            acc += clen;
            if (w > maxw) break; /* 单字符超宽也接受 */
        }
        if (acc == 0) break;
        if (lines) {
            lines[n].off = (size_t)(p - text);
            lines[n].len = acc;
        }
        n++;
        p += acc;
    }
    return n;
}

static void draw_line(TTF_Font *font, SDL_Color col, int x, int y,
                      const char *text, size_t off, size_t len) {
    char buf[MAX_LINE_LEN];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text + off, len);
    buf[len] = '\0';
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, buf, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    if (tex) {
        SDL_Rect dst = { x, y, surf->w, surf->h };
        SDL_RenderCopy(g_ren, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void clamp_scroll(int max_off);

/* ---------- Markdown-lite 渲染 ---------- */

static void strip_md_markers(const char *src, char *dst, size_t dstsz) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < dstsz; p++) {
        if (p[0] == '*' && p[1] == '*') {
            p++;
            continue;
        }
        dst[o++] = *p;
    }
    dst[o] = '\0';
}

/* 画一行,支持 ** 粗体分段;返回推进的像素 */
static int draw_md_line(TTF_Font *font, SDL_Color col, SDL_Color boldc,
                        int force_bold, int x, int y,
                        const char *text, size_t off, size_t len) {
    char buf[MAX_LINE_LEN];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text + off, len);
    buf[len] = '\0';
    int cx = x;
    if (force_bold) {
        draw_line(font, boldc, cx, y, buf, 0, strlen(buf));
        int w = 0, h = 0;
        TTF_SizeUTF8(font, buf, &w, &h);
        return w;
    }
    char *seg = buf;
    int bold = 0;
    while (*seg) {
        char *mark = strstr(seg, "**");
        if (mark) *mark = '\0';
        if (*seg) {
            draw_line(font, bold ? boldc : col, cx, y, seg, 0, strlen(seg));
            int w = 0, h = 0;
            TTF_SizeUTF8(font, seg, &w, &h);
            cx += w;
        }
        if (!mark) break;
        seg = mark + 2;
        bold = !bold;
    }
    return cx - x;
}

/*
 * 渲染/测量一段文本(markdown-lite):
 *   - "#" 开头的行整行加粗
 *   - ``` 围住的代码块:小号字体、次级色、缩进 + 背景条
 *   - ** 之间的内容加粗显示
 * measure_only=1 时只计算不绘制。返回总高度;*max_w_out 为最宽行宽。
 */
static int render_md_text(int maxw, int lineh, int x, int y, const char *text,
                          SDL_Color base_col, SDL_Color bold_col,
                          int measure_only, int *max_w_out) {
    int cy = y;
    int in_code = 0;
    const char *p = text;
    char line[8192];
    *max_w_out = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len == 0) {
            cy += lineh / 2;
            p += 1;
            continue;
        }
        size_t cl = len;
        if (cl >= sizeof(line)) cl = sizeof(line) - 1;
        memcpy(line, p, cl);
        line[cl] = '\0';
        p += (nl ? len + 1 : len);

        if (strncmp(line, "```", 3) == 0) {
            in_code = !in_code;
            continue;
        }

        TTF_Font *lf = g_font;
        int lh = lineh;
        int indent = 0;
        SDL_Color col = base_col;
        SDL_Color boldc = bold_col;
        int force_bold = 0;

        if (in_code) {
            lf = g_font_hint;
            lh = TTF_FontHeight(lf) + 4;
            indent = 10;
            col = COL_TEXT2;
            boldc = COL_TEXT2;
        } else if (line[0] == '#') {
            force_bold = 1;
        }

        char *t = line;
        if (line[0] == '#') {
            while (*t == '#') t++;
            while (*t == ' ' || *t == '\t') t++;
        }

        wrap_line_t wl[MAX_LINES];
        int nwl = wrap_text(lf, maxw - indent, t, wl, MAX_LINES);
        for (int j = 0; j < nwl; j++) {
            char buf[MAX_LINE_LEN];
            size_t bl = wl[j].len;
            if (bl >= sizeof(buf)) bl = sizeof(buf) - 1;
            memcpy(buf, t + wl[j].off, bl);
            buf[bl] = '\0';
            char mb[MAX_LINE_LEN];
            strip_md_markers(buf, mb, sizeof(mb));
            int w = 0, h = 0;
            TTF_SizeUTF8(lf, mb, &w, &h);
            if (w > *max_w_out) *max_w_out = w;
            if (!measure_only) {
                if (in_code) {
                    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
                    SDL_Rect bg = { x + indent - 6, cy, w + 12, lh };
                    SDL_RenderFillRect(g_ren, &bg);
                }
                draw_md_line(lf, col, boldc, force_bold,
                             x + indent, cy, t, wl[j].off, wl[j].len);
            }
            cy += lh;
        }
    }
    return cy - y;
}

/*
 * 单条消息按"桌面版对话流"排版(无气泡):
 *   - 用户:"你" 标签(品牌蓝)+ 正文
 *   - 助手:思考(暗色 + 左侧竖线缩进)→ 工具行(次级色)→ 处理提示(红)→ 正文
 * measure_only=1 时只测量。返回高度;*maxw_out 为最宽行。
 */
static int render_msg_flow(int x0, int maxw, int lineh, int y, msg_t *m,
                           int measure_only, int *maxw_out) {
    int cy = y;
    *maxw_out = 0;
    int w2 = 0;

    if (m->role == ROLE_USER) {
        if (!measure_only) {
            SDL_Surface *s = TTF_RenderUTF8_Blended(g_font_hint, "你", COL_BRAND);
            if (s) {
                SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
                SDL_Rect d = { x0, cy, s->w, s->h };
                SDL_RenderCopy(g_ren, t, NULL, &d);
                SDL_DestroyTexture(t);
                SDL_FreeSurface(s);
            }
        }
        cy += TTF_FontHeight(g_font_hint) + 6;
        if (m->text && m->text[0]) {
            int h2 = render_md_text(maxw, lineh, x0, cy, m->text,
                                    COL_TEXT, COL_WHITE, measure_only, &w2);
            if (w2 > *maxw_out) *maxw_out = w2;
            cy += h2;
        }
        return cy - y;
    }

    /* 助手:思考(缩进 + 竖线) */
    if (m->think && m->think[0]) {
        int h2 = render_md_text(maxw - 12, lineh, x0 + 12, cy, m->think,
                                COL_TEXT3, COL_TEXT3, measure_only, &w2);
        if (!measure_only) {
            SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
            SDL_Rect bar = { x0, cy, 2, h2 };
            SDL_RenderFillRect(g_ren, &bar);
        }
        if (w2 > *maxw_out) *maxw_out = w2;
        cy += h2 + 8;
    }
    /* 工具活动行 */
    if (m->tools && m->tools[0]) {
        int h2 = render_md_text(maxw, lineh, x0, cy, m->tools,
                                COL_TEXT2, COL_TEXT2, measure_only, &w2);
        if (w2 > *maxw_out) *maxw_out = w2;
        cy += h2 + 8;
    }
    /* 处理提示(红) */
    if (m->notice && m->notice[0]) {
        int h2 = render_md_text(maxw, lineh, x0, cy, m->notice,
                                COL_RED, COL_RED, measure_only, &w2);
        if (w2 > *maxw_out) *maxw_out = w2;
        cy += h2 + 8;
    }
    /* 正文 */
    if (m->text && m->text[0]) {
        int h2 = render_md_text(maxw, lineh, x0, cy, m->text,
                                COL_TEXT, COL_WHITE, measure_only, &w2);
        if (w2 > *maxw_out) *maxw_out = w2;
        cy += h2;
    }
    return cy - y;
}

/* ---------- 侧栏(默认显示,对齐桌面版) ---------- */

/* 本节用到的后置函数声明 */
static void clear_msgs(void);
static void enter_workspaces(int from_startup);
static void draw_line(TTF_Font *font, SDL_Color col, int x, int y,
                      const char *text, size_t off, size_t len);
static void chat_resume_last(void);

static SDL_Texture *g_sb_tex = NULL;
static int g_sb_rev = -1;
static int g_drag_in_sb = 0;
static int g_chat_resumed = 0;

static void draw_trunc(TTF_Font *font, const char *text, SDL_Color col,
                       int x, int y, int maxw) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", text);
    for (;;) {
        int w = 0, h = 0;
        TTF_SizeUTF8(font, buf, &w, &h);
        if (w <= maxw) break;
        size_t len = strlen(buf);
        if (len <= 1) break;
        size_t cut = utf8_fit_bytes(buf, len - 2);
        if (cut == 0 || cut >= len) break;
        buf[cut] = '\0';
        strcat(buf, "…");
    }
    draw_line(font, col, x, y, buf, 0, strlen(buf));
}

/* 右对齐像素级截断:x_right 为文本右边缘 */
static void draw_trunc_right(TTF_Font *font, const char *text, SDL_Color col,
                             int x_right, int y, int maxw) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", text);
    for (;;) {
        int w = 0, h = 0;
        TTF_SizeUTF8(font, buf, &w, &h);
        if (w <= maxw) break;
        size_t len = strlen(buf);
        if (len <= 1) break;
        size_t cut = utf8_fit_bytes(buf, len - 2);
        if (cut == 0 || cut >= len) break;
        buf[cut] = '\0';
        strcat(buf, "…");
    }
    int w = 0, h = 0;
    TTF_SizeUTF8(font, buf, &w, &h);
    draw_line(font, col, x_right - w, y, buf, 0, strlen(buf));
}

static void sb_build_rows(void) {
    g_sb_rows_n = 0;
    g_sb_rows[g_sb_rows_n].kind = 0;
    g_sb_rows[g_sb_rows_n].idx = 0;
    g_sb_rows_n++;

    if (strcmp(g_cfg.backend, "harness") == 0 && g_sb_rows_n < SB_ROWS_MAX) {
        char *matched = (char *)calloc(g_sessions_n ? g_sessions_n : 1, 1);
        for (size_t w = 0; w < g_wss_n && g_sb_rows_n < SB_ROWS_MAX - 4; w++) {
            g_sb_rows[g_sb_rows_n].kind = 4;
            g_sb_rows[g_sb_rows_n].idx = (int)w;
            g_sb_rows_n++;
            for (size_t i = 0; i < g_sessions_n && g_sb_rows_n < SB_ROWS_MAX - 4; i++) {
                if (g_sessions[i].cwd && g_wss[w].path &&
                    strcmp(g_sessions[i].cwd, g_wss[w].path) == 0) {
                    g_sb_rows[g_sb_rows_n].kind = 1;
                    g_sb_rows[g_sb_rows_n].idx = (int)i;
                    g_sb_rows_n++;
                    matched[i] = 1;
                }
            }
        }
        /* 其他(未归入工作区的会话) */
        int has_other = 0;
        for (size_t i = 0; i < g_sessions_n; i++) if (!matched[i]) { has_other = 1; break; }
        if (has_other && g_sb_rows_n < SB_ROWS_MAX - 4) {
            g_sb_rows[g_sb_rows_n].kind = 4;
            g_sb_rows[g_sb_rows_n].idx = -1;
            g_sb_rows_n++;
            for (size_t i = 0; i < g_sessions_n && g_sb_rows_n < SB_ROWS_MAX - 4; i++) {
                if (!matched[i]) {
                    g_sb_rows[g_sb_rows_n].kind = 1;
                    g_sb_rows[g_sb_rows_n].idx = (int)i;
                    g_sb_rows_n++;
                }
            }
        }
        free(matched);
    }
    /* 工作区管理仅在 Harness 后端时显示(DeepSeek 直连无此功能) */
    if (strcmp(g_cfg.backend, "harness") == 0) {
        g_sb_rows[g_sb_rows_n].kind = 2;
        g_sb_rows[g_sb_rows_n].idx = 0;
        g_sb_rows_n++;
    }
    g_sb_rows[g_sb_rows_n].kind = 3;
    g_sb_rows[g_sb_rows_n].idx = 0;
    g_sb_rows_n++;
}

static int sb_thread_fn(void *arg) {
    (void)arg;
    char err[256] = {0};
    harness_workspace_t *w = NULL;
    size_t wn = 0;
    harness_session_t *s = NULL;
    size_t sn = 0;
    harness_list_workspaces(&g_cfg, &w, &wn, err, sizeof(err));
    harness_list_sessions(&g_cfg, &s, &sn, err, sizeof(err));
    g_sb_tmp_wss = w;
    g_sb_tmp_wss_n = wn;
    g_sb_tmp_sess = s;
    g_sb_tmp_sess_n = sn;
    g_sb_ready = 1;
    return 0;
}

static void sb_load(void) {
    if (g_sb_loading) return;
    g_sb_err[0] = '\0';
    if (strcmp(g_cfg.backend, "deepseek") == 0) {
        /* 无会话列表,即时完成 */
        sb_build_rows();
        g_sb_loaded = 1;
        g_dirty = 1;
        return;
    }
    g_sb_loading = 1;
    g_sb_ready = 0;
    SDL_Thread *th = SDL_CreateThread(sb_thread_fn, "sb", NULL);
    if (th) {
        SDL_DetachThread(th);
    } else {
        g_sb_loading = 0;
        g_sb_loaded = 1; /* 线程失败也放行,避免卡死 */
        g_dirty = 1;
    }
}

/* 主线程收口:后台列表就绪后原子替换 */
static void sb_apply_ready(void) {
    if (!g_sb_loading || !g_sb_ready) return;
    if (strcmp(g_cfg.backend, "harness") != 0) {
        /* 加载期间切换了后端:丢弃过期结果 */
        harness_workspaces_free(g_sb_tmp_wss, g_sb_tmp_wss_n);
        harness_sessions_free(g_sb_tmp_sess, g_sb_tmp_sess_n);
    } else {
        harness_workspaces_free(g_wss, g_wss_n);
        g_wss = g_sb_tmp_wss;
        g_wss_n = g_sb_tmp_wss_n;
        harness_sessions_free(g_sessions, g_sessions_n);
        g_sessions = g_sb_tmp_sess;
        g_sessions_n = g_sb_tmp_sess_n;
        sb_build_rows();
        g_sb_idx = 0;
        g_sb_loaded = 1;
    }
    g_sb_tmp_wss = NULL;
    g_sb_tmp_wss_n = 0;
    g_sb_tmp_sess = NULL;
    g_sb_tmp_sess_n = 0;
    g_sb_loading = 0;
    g_sb_ready = 0;
    g_dirty = 1;
}

static void ensure_sidebar_tex(void) {
    const char *cur = harness_current_session();
    if (!cur) cur = "";
    int rev = (strcmp(g_cfg.backend, "deepseek") == 0 ? 1 : 0) + g_focus * 2 +
              g_sb_idx * 8 + (g_sb_loaded ? 16 : 0) + (g_sb_loading ? 32 : 0) +
              (g_drag_in_sb ? 64 : 0);
    for (const char *p = g_sb_err; *p; p++) rev = rev * 31 + (unsigned char)*p;
    for (const char *p = cur; *p; p++) rev = rev * 31 + (unsigned char)*p;
    for (int i = 0; i < g_sb_rows_n; i++) {
        if (g_sb_rows[i].kind == 1 && (size_t)g_sb_rows[i].idx < g_sessions_n)
            for (const char *p = g_sessions[g_sb_rows[i].idx].title; *p; p++)
                rev = rev * 31 + (unsigned char)*p;
        else if (g_sb_rows[i].kind == 4 && g_sb_rows[i].idx >= 0 &&
                 (size_t)g_sb_rows[i].idx < g_wss_n)
            for (const char *p = g_wss[g_sb_rows[i].idx].title; *p; p++)
                rev = rev * 31 + (unsigned char)*p;
    }
    if (rev == g_sb_rev && g_sb_tex) return;
    if (g_sb_tex) SDL_DestroyTexture(g_sb_tex);
    g_sb_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA8888,
                                 SDL_TEXTUREACCESS_TARGET, SB_W, WIN_H);
    if (!g_sb_tex) return;
    SDL_SetRenderTarget(g_ren, g_sb_tex);
    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { SB_W - 1, 0, 1, WIN_H };
    SDL_RenderFillRect(g_ren, &hair);

    /* 侧栏焦点:品牌蓝描边,状态一目了然 */
    if (g_focus) {
        roundedRectangleRGBA(g_ren, 2, 2, SB_W - 2, WIN_H - 2, 10,
                             COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);
        roundedRectangleRGBA(g_ren, 3, 3, SB_W - 3, WIN_H - 3, 9,
                             COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);
    }

    /* LOGO + 名称 */
    if (g_logo_tex) {
        SDL_Rect ld = { 10, 8, 36, 36 };
        SDL_RenderCopy(g_ren, g_logo_tex, NULL, &ld);
    }
    draw_line(g_font_hint, COL_TEXT, 56, 18, "DSH Switch", 0, 10);

    /* 后端标签 */
    {
        char tag[64];
        snprintf(tag, sizeof(tag), "%s",
                 strcmp(g_cfg.backend, "deepseek") == 0 ? "DeepSeek(官方 API)"
                                                        : "Harness(局域网)");
        draw_line(g_font_hint, COL_ACCENT, 16, 58, tag, 0, strlen(tag));
    }

    /* 行 */
    int y = 92;
    int fi = 0;
    if (g_sb_loading || !g_sb_loaded) {
        draw_line(g_font_hint, COL_TEXT2, 16, y, "正在加载会话…",
                  0, strlen("正在加载会话…"));
        y += 26;
    }
    for (int i = 0; i < g_sb_rows_n; i++) {
        sb_row_t *r = &g_sb_rows[i];
        if (r->kind == 4) {
            if (y + 30 > WIN_H - 100) break;
            r->fy = y;
            r->fh = 30;
            g_sb_focus_of[i] = -1;
            const char *t = "其他";
            if (r->idx >= 0 && (size_t)r->idx < g_wss_n) t = g_wss[r->idx].title;
            draw_trunc(g_font_hint, t, COL_TEXT3, 20, y + 4, SB_W - 40);
            y += 30;
            continue;
        }
        if (y + 48 > WIN_H - 100) break;
        r->fy = y;
        r->fh = 48;
        g_sb_focus_of[i] = fi;
        g_sb_row_of[fi] = i;
        fi++;

        int sel = (g_focus && g_sb_idx == fi - 1);
        if (sel) {
            roundedBoxRGBA(g_ren, 8, (Sint16)y, SB_W - 12, (Sint16)(y + 44), 8,
                           COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
            SDL_SetRenderDrawColor(g_ren, COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);
            SDL_Rect bar = { 8, y + 8, 3, 28 };
            SDL_RenderFillRect(g_ren, &bar);
        }

        if (r->kind == 0) {
            const char *lab = strcmp(g_cfg.backend, "deepseek") == 0
                                  ? "＋ 新对话" : "＋ 新建会话";
            draw_line(g_font_hint, COL_BRAND, 20, y + 12, lab, 0, strlen(lab));
        } else if (r->kind == 1) {
            harness_session_t *s = &g_sessions[r->idx];
            const char *curs = harness_current_session();
            SDL_Color tc = (curs && strcmp(curs, s->session_id) == 0) ? COL_BRAND : COL_TEXT;
            draw_trunc(g_font_hint, s->title, tc, 20, y + 6, SB_W - 60);
            if (s->running) draw_line(g_font_hint, COL_GREEN, 20, y + 28, "运行中", 0, 9);
        } else if (r->kind == 2) {
            draw_line(g_font_hint, COL_TEXT, 20, y + 12, "工作区管理", 0, strlen("工作区管理"));
        } else if (r->kind == 3) {
            draw_line(g_font_hint, COL_TEXT, 20, y + 12, "设置", 0, 6);
        }
        y += 48;
    }
    g_sb_focus_n = fi;

    if (g_sb_err[0])
        draw_trunc(g_font_hint, g_sb_err, COL_RED, 16, WIN_H - 52, SB_W - 32);

    if (strcmp(g_cfg.backend, "deepseek") == 0)
        draw_line(g_font_hint, COL_TEXT3, 16, WIN_H - FOOTER_H - 24,
                  "会话/工作区管理需 Harness 桥接",
                  0, strlen("会话/工作区管理需 Harness 桥接"));

    draw_line(g_font_hint, COL_TEXT3, 16, WIN_H - FOOTER_H - 4, "X 焦点切换  Y 设置  R 切后端",
              0, strlen("X 焦点切换  Y 设置  R 切后端"));
    SDL_SetRenderTarget(g_ren, NULL);
    g_sb_rev = rev;
}

static void sb_nav(int dir) {
    if (g_sb_focus_n == 0) return;
    g_sb_idx += dir;
    if (g_sb_idx < 0) g_sb_idx = 0;
    if (g_sb_idx >= g_sb_focus_n) g_sb_idx = g_sb_focus_n - 1;
    g_dirty = 1;
}

static void sb_activate(int fi) {
    if (fi < 0 || fi >= g_sb_focus_n) return;
    int row = g_sb_row_of[fi];
    sb_row_t *r = &g_sb_rows[row];
    if (r->kind == 0) {
        if (strcmp(g_cfg.backend, "harness") == 0) {
            char err[256] = {0};
            if (harness_new_session_in(&g_cfg, NULL, err, sizeof(err)) == 0) {
                clear_msgs();
                add_msg(ROLE_ASSISTANT, "已新建会话。\nA 输入消息开始。", 1);
                snprintf(g_sess_title, sizeof(g_sess_title), "新会话");
                save_current_history();
            } else {
                snprintf(g_sb_err, sizeof(g_sb_err), "%s",
                         err[0] ? err : "新建会话失败(桥接未运行?)");
            }
        } else {
            clear_msgs();
            add_msg(ROLE_ASSISTANT, "新对话已开始。", 1);
            snprintf(g_sess_title, sizeof(g_sess_title), "DeepSeek 对话");
            save_current_history();
        }
        g_focus = 0;
        g_dirty = 1;
        return;
    }
    if (r->kind == 1) {
        if ((size_t)r->idx >= g_sessions_n) return;
        harness_session_t *s = &g_sessions[r->idx];
        char err[256] = {0};
        if (harness_use_session(&g_cfg, s->session_id, err, sizeof(err)) != 0) {
            snprintf(g_sb_err, sizeof(g_sb_err), "%s",
                     err[0] ? err : "切换失败(桥接未运行?)");
            g_dirty = 1;
            return;
        }
        clear_msgs();
        chat_message_t *msgs = NULL;
        size_t n = 0;
        long long first = -1;
        if (harness_fetch_history_ex(&g_cfg, 0, &msgs, &n, &first,
                                     err, sizeof(err)) == 0) {
            for (size_t i = 0; i < n; i++)
                add_msg(msgs[i].role, msgs[i].content ? msgs[i].content : "", 1);
            for (size_t i = 0; i < n; i++) free(msgs[i].content);
            free(msgs);
            g_first_seq = first;
        }
        if (g_nmsgs == 0) add_msg(ROLE_ASSISTANT, "(该会话暂无聊天记录)", 1);
        snprintf(g_sess_title, sizeof(g_sess_title), "%s", s->title);
        g_focus = 0;
        g_dirty = 1;
        return;
    }
    if (r->kind == 2) {
        enter_workspaces(0);
        return;
    }
    if (r->kind == 3) {
        g_set_idx = 0;
        g_screen = SCREEN_SETTINGS;
        g_dirty = 1;
    }
}

static void sb_tap(int ty) {
    for (int i = 0; i < g_sb_rows_n; i++) {
        sb_row_t *r = &g_sb_rows[i];
        if (ty >= r->fy && ty < r->fy + r->fh && g_sb_focus_of[i] >= 0) {
            g_sb_idx = g_sb_focus_of[i];
            sb_activate(g_sb_idx);
            return;
        }
    }
}

static void chat_resume_last(void) {
    if (strcmp(g_cfg.backend, "harness") != 0) return;
    const char *sid = harness_current_session();
    if (!sid || !sid[0]) return;
    char err[256] = {0};
    chat_message_t *msgs = NULL;
    size_t n = 0;
    long long first = -1;
    if (harness_fetch_history_ex(&g_cfg, 0, &msgs, &n, &first,
                                 err, sizeof(err)) == 0 && n > 0) {
        clear_msgs();
        for (size_t i = 0; i < n; i++)
            add_msg(msgs[i].role, msgs[i].content ? msgs[i].content : "", 1);
        for (size_t i = 0; i < n; i++) free(msgs[i].content);
        free(msgs);
        g_first_seq = first;
        g_dirty = 1;
    }
}

/* ---------- 渲染缓存(60fps:消息预渲染成纹理,帧内只贴图) ---------- */

static SDL_Texture *g_hdr_tex = NULL;
static SDL_Texture *g_ftr_tex = NULL;
static int g_hdr_rev = -1;
static int g_ftr_rev = -1;

static void evict_textures(void) {
    while (g_tex_count > MAX_TEX_CACHE) {
        int freed = 0;
        for (int i = 0; i < g_nmsgs; i++) {
            msg_t *m = &g_msgs[i];
            if (m->tex && i != g_nmsgs - 1) {
                msg_tex_destroy(m);
                m->dirty = 1; /* 滚回来时重新渲染 */
                freed = 1;
                break;
            }
        }
        if (!freed) break;
    }
}

/* 把一条消息渲染进纹理(内容变化才调用);超长消息不缓存走直绘 */
static void render_msg_texture(msg_t *m, int x0, int maxw, int lineh) {
    msg_tex_destroy(m);
    int mw = 0;
    int h = render_msg_flow(x0, maxw, lineh, 0, m, 1, &mw);
    if (h <= 0) h = 12;
    m->tex_h = h;
    m->tex_w = maxw;
    m->dirty = 0;
    if (h > 2800) return; /* 超长消息直接绘制 */

    SDL_Texture *tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET, maxw, h);
    if (!tex) return;
    g_tex_count++;
    SDL_SetRenderTarget(g_ren, tex);
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);
    int wout = 0;
    render_msg_flow(0, maxw, lineh, 0, m, 0, &wout);
    SDL_SetRenderTarget(g_ren, NULL);
    m->tex = tex;
}

static void ensure_header_tex(void) {
    int rev = strcmp(g_cfg.backend, "deepseek") == 0 ? 1 : 0;
    const char *mp = g_cur_model[0] ? g_cur_model : (g_cfg.model ? g_cfg.model : "");
    for (const char *p = mp; *p; p++) rev = rev * 31 + (unsigned char)*p;
    for (const char *p = g_sess_title; *p; p++) rev = rev * 31 + (unsigned char)*p;
    if (rev == g_hdr_rev && g_hdr_tex) return;
    if (g_hdr_tex) SDL_DestroyTexture(g_hdr_tex);
    g_hdr_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA8888,
                                  SDL_TEXTUREACCESS_TARGET, WIN_W - SB_W, HEADER_H);
    if (!g_hdr_tex) return;
    SDL_SetRenderTarget(g_ren, g_hdr_tex);
    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W - SB_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    char title[160];
    snprintf(title, sizeof(title), "%s", g_sess_title);
    draw_trunc(g_font_title, title, COL_TEXT, 20,
               (HEADER_H - TTF_FontHeight(g_font_title)) / 2,
               (WIN_W - SB_W) - 20 - 340); /* 右侧预留后端/模型标签 */
    snprintf(title, sizeof(title), "%s · %s",
             strcmp(g_cfg.backend, "deepseek") == 0 ? "DeepSeek" : "Harness", mp);
    draw_trunc_right(g_font_hint, title, COL_ACCENT,
                     WIN_W - SB_W - 24, (HEADER_H - TTF_FontHeight(g_font_hint)) / 2,
                     360);
    SDL_SetRenderTarget(g_ren, NULL);
    g_hdr_rev = rev;
}

static void ensure_footer_tex(void) {
    int busy = g_worker_busy ? 1 : 0;
    int th = g_stream_think ? 1 : 0;
    int be = strcmp(g_cfg.backend, "harness") == 0 ? 1 : 0;
    int streaming = (g_nmsgs > 0 && g_msgs[g_nmsgs - 1].role == ROLE_ASSISTANT &&
                     !g_msgs[g_nmsgs - 1].done) ? 1 : 0;
    int tts_on = tts_playing() ? 1 : 0;
    int stt_on = stt_recording() ? 1 : 0;
    int rev = busy * 10000 + th * 1000 + be * 100 + streaming * 10 + tts_on * 3 + stt_on + (g_focus ? 1000000 : 0);
    if (rev == g_ftr_rev && g_ftr_tex) return;
    if (g_ftr_tex) SDL_DestroyTexture(g_ftr_tex);
    g_ftr_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA8888,
                                  SDL_TEXTUREACCESS_TARGET, WIN_W - SB_W, FOOTER_H);
    if (!g_ftr_tex) return;
    SDL_SetRenderTarget(g_ren, g_ftr_tex);
    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect fhair = { 0, 0, WIN_W - SB_W, 1 };
    SDL_RenderFillRect(g_ren, &fhair);

    const char *hint;
    if (g_focus)
        hint = "侧栏模式:方向键选择  A 打开  B 返回对话  R 切后端  Y 设置";
    else if (stt_on)
        hint = "语音录制中…松开 ZR 发送    + 退出";
    else if (tts_on)
        hint = "朗读中…(- 停止)    + 退出";
    else if (busy)
        hint = th ? "思考中…(B 停止)    + 退出" : "回复中…(B 停止)    + 退出";
    else
        hint = be ? "A输入 B停止 X侧栏 Y设置 L模型 R后端 -朗读 ZR录音 ZL更早 +退出"
                  : "A输入 B停止 X侧栏 Y设置 L模型 R后端 +退出";
    int hint_max = WIN_W - SB_W - 48 - (streaming ? 170 : 24);
    draw_trunc(g_font_hint, hint, COL_HINT, 24,
               (FOOTER_H - TTF_FontHeight(g_font_hint)) / 2, hint_max);
    /* 右侧提示 */
    if (streaming) {
        SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_hint, th ? "思考中…" : "回复中…", COL_ACCENT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { WIN_W - SB_W - ts->w - 24, (FOOTER_H - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
    }
    SDL_SetRenderTarget(g_ren, NULL);
    g_ftr_rev = rev;
}

/* ---------- 渲染 ---------- */

static void render_chat(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    /* 左侧栏(默认显示,桌面版布局) */
    ensure_sidebar_tex();
    if (g_sb_tex) {
        SDL_Rect sbd = { 0, 0, SB_W, WIN_H };
        SDL_RenderCopy(g_ren, g_sb_tex, NULL, &sbd);
    }

    /* 顶栏(缓存纹理,会话标题) */
    ensure_header_tex();
    if (g_hdr_tex) {
        SDL_Rect hd = { SB_W, 0, WIN_W - SB_W, HEADER_H };
        SDL_RenderCopy(g_ren, g_hdr_tex, NULL, &hd);
    }

    /* 消息区(对话面板) */
    const int area_y0 = HEADER_H + 10;
    const int area_y1 = WIN_H - FOOTER_H - 8;
    const int lineh = TTF_FontHeight(g_font) + 6;
    const int content_x = SB_W + 90;
    const int cmaxw = WIN_W - content_x - 90;

    /* 任务清单条(顶部浮层,短文本直接画) */
    if (g_todos_str[0]) {
        int wout = 0;
        int th2 = render_md_text(cmaxw, lineh, content_x, HEADER_H + 8,
                                 g_todos_str, COL_TEXT3, COL_TEXT3, 1, &wout);
        SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
        SDL_Rect tbg = { content_x - 12, HEADER_H + 4, WIN_W - (content_x - 12) * 2, th2 + 8 };
        SDL_RenderFillRect(g_ren, &tbg);
        render_md_text(cmaxw, lineh, content_x, HEADER_H + 8,
                       g_todos_str, COL_TEXT3, COL_TEXT3, 0, &wout);
    }

    /* 有内容变化的消息重渲染纹理 + 汇总总高 */
    int total_h = 0;
    for (int i = 0; i < g_nmsgs; i++) {
        msg_t *m = &g_msgs[i];
        if (m->dirty) render_msg_texture(m, content_x, cmaxw, lineh);
        total_h += m->tex_h + 18;
    }
    evict_textures();

    int view_h = area_y1 - area_y0;
    int max_off = total_h > view_h ? total_h - view_h : 0;
    clamp_scroll(max_off);
    int y = area_y0 + g_scroll_offset - (total_h > view_h ? total_h - view_h : 0);

    /* 可见区贴图 */
    for (int i = 0; i < g_nmsgs; i++) {
        msg_t *m = &g_msgs[i];
        int mh = m->tex_h;
        if (y + mh < area_y0 || y > area_y1) {
            y += mh + 18;
            continue;
        }
        if (m->tex) {
            SDL_Rect dst = { content_x, y, m->tex_w, mh };
            SDL_RenderCopy(g_ren, m->tex, NULL, &dst);
        } else {
            /* 未缓存的超长消息:直接画 */
            int mw = 0;
            render_msg_flow(content_x, cmaxw, lineh, y, m, 0, &mw);
        }
        y += mh + 18;
    }

    /* 滚动条 */
    if (max_off > 0) {
        int track_h = view_h - 8;
        int thumb_h = track_h * view_h / total_h;
        if (thumb_h < 24) thumb_h = 24;
        int thumb_y = area_y0 + 4 + (track_h - thumb_h) * g_scroll_offset / max_off;
        roundedBoxRGBA(g_ren, WIN_W - 12, (Sint16)thumb_y,
                       WIN_W - 6, (Sint16)(thumb_y + thumb_h), 3,
                       COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 200);
    }

    /* 底栏(缓存纹理) */
    ensure_footer_tex();
    if (g_ftr_tex) {
        SDL_Rect fd = { SB_W, WIN_H - FOOTER_H, WIN_W - SB_W, FOOTER_H };
        SDL_RenderCopy(g_ren, g_ftr_tex, NULL, &fd);
    }
}

/* ---------- 输入:手柄 + 触摸屏(含拖动滚动) ---------- */

static int g_tap_x = -1;
static int g_tap_y = -1;
static int g_drag = 0;
static int g_drag_moved = 0;
static int g_drag_start_y = 0;
static int g_drag_start_x = 0;
static int g_drag_last_y = 0;
static int g_drag_last_x = 0;
static int g_drag_start_offset = 0;
static u32 g_touch_prev = 0;

static void clamp_scroll(int max_off) {
    if (g_scroll_offset < 0) g_scroll_offset = 0;
    if (g_scroll_offset > max_off) g_scroll_offset = max_off;
}

static void poll_events(void) {
    g_tap_x = -1;
    g_tap_y = -1;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_MOUSEBUTTONDOWN &&
            ev.button.button == SDL_BUTTON_LEFT) {
            g_drag = 1;
            g_drag_moved = 0;
            g_drag_in_sb = (ev.button.x < SB_W);
            g_drag_start_y = ev.button.y;
            g_drag_last_y = ev.button.y;
            g_drag_start_offset = g_scroll_offset;
        } else if (ev.type == SDL_MOUSEMOTION && g_drag) {
            int dy = g_drag_last_y - ev.motion.y; /* 上滑 = 看更早 */
            if (dy > 8 || dy < -8) g_drag_moved = 1;
            if (g_drag_moved && !g_drag_in_sb && g_screen == SCREEN_CHAT) {
                g_scroll_offset = g_drag_start_offset + (g_drag_start_y - ev.motion.y);
                g_dirty = 1;
            }
            g_drag_last_y = ev.motion.y;
        } else if (ev.type == SDL_MOUSEBUTTONUP &&
                   ev.button.button == SDL_BUTTON_LEFT) {
            if (!g_drag_moved) {
                g_tap_x = ev.button.x;
                g_tap_y = ev.button.y;
            }
            g_drag = 0;
        }
#ifdef SDL_FINGERDOWN
        else if (ev.type == SDL_FINGERDOWN) {
            g_drag = 1;
            g_drag_moved = 0;
            g_drag_start_y = (int)(ev.tfinger.y * (float)WIN_H);
            g_drag_last_y = g_drag_start_y;
            g_drag_start_offset = g_scroll_offset;
        } else if (ev.type == SDL_FINGERMOTION && g_drag) {
            int cy = (int)(ev.tfinger.y * (float)WIN_H);
            int dy = g_drag_last_y - cy;
            if (dy > 8 || dy < -8) g_drag_moved = 1;
            if (g_drag_moved && g_screen == SCREEN_CHAT) {
                g_scroll_offset = g_drag_start_offset + (g_drag_start_y - cy);
                g_dirty = 1;
            }
            g_drag_last_y = cy;
        } else if (ev.type == SDL_FINGERUP) {
            if (!g_drag_moved) {
                g_tap_x = (int)(ev.tfinger.x * (float)WIN_W);
                g_tap_y = (int)(ev.tfinger.y * (float)WIN_H);
            }
            g_drag = 0;
        }
#endif
    }

    /* 原始 hid 触摸(真机上最可靠,绕过 SDL 触摸映射) */
    {
        HidTouchScreenState ts;
        memset(&ts, 0, sizeof(ts));
        hidGetTouchScreenStates(&ts, 1);
        u32 cnt = ts.count;
        if (cnt > 0 && g_touch_prev == 0) {
            g_drag = 1;
            g_drag_moved = 0;
            g_drag_in_sb = ((int)ts.touches[0].x < SB_W);
            g_drag_start_y = (int)ts.touches[0].y;
            g_drag_start_x = (int)ts.touches[0].x;
            g_drag_last_y = g_drag_start_y;
            g_drag_last_x = g_drag_start_x;
            g_drag_start_offset = g_scroll_offset;
        } else if (cnt > 0 && g_drag) {
            int cy2 = (int)ts.touches[0].y;
            int dy = g_drag_last_y - cy2;
            if (dy > 8 || dy < -8) g_drag_moved = 1;
            if (g_drag_moved && !g_drag_in_sb && g_screen == SCREEN_CHAT) {
                g_scroll_offset = g_drag_start_offset + (g_drag_start_y - cy2);
                g_dirty = 1;
            }
            g_drag_last_y = cy2;
            g_drag_last_x = (int)ts.touches[0].x;
        } else if (cnt == 0 && g_touch_prev > 0 && g_drag) {
            if (!g_drag_moved) {
                g_tap_x = g_drag_last_x;
                g_tap_y = g_drag_last_y;
            }
            g_drag = 0;
        }
        g_touch_prev = cnt;
    }
}

/* 通用列表点击:计算行号并触发 A */
static int tap_row_index(int y0, int row_h, int gap) {
    if (g_tap_x < 0) return -1;
    int idx = (g_tap_y - y0) / (row_h + gap);
    g_tap_x = -1;
    g_tap_y = -1;
    if (idx < 0) return -1;
    return idx;
}

/* API Key 子菜单 */
static int g_key_menu_open = 0;
static int g_key_menu_idx = 0;
static char g_key_menu_msg[256];

/* 设置界面 ---------- */
static int g_set_scroll = 0; /* 设置页滚动偏移 */
#define SET_COUNT 13

static const char *set_label(int i) {
    switch (i) {
        case 0: return "后端";
        case 1: return "Harness 地址";
        case 2: return "DeepSeek 地址";
        case 3: return "API Key(key.txt 优先)";
        case 4: return "模型";
        case 5: return "思考模式";
        case 6: return "系统提示词";
        case 7: return "语音朗读";
        case 8: return "STT 服务地址";
        case 9: return "语音语速";
        case 10: return "语音音量";
        case 11: return "语音音调";
        case 12: return "导出对话到 SD 卡";
        default: return "";
    }
}

static void set_value(int i, char *out, size_t outsz) {
    switch (i) {
        case 0:
            snprintf(out, outsz, "%s",
                     strcmp(g_cfg.backend, "deepseek") == 0 ? "DeepSeek" : "Harness");
            break;
        case 1: snprintf(out, outsz, "%s", g_cfg.harness_base_url); break;
        case 2: snprintf(out, outsz, "%s", g_cfg.deepseek_base_url); break;
        case 3:
            snprintf(out, outsz, "%s",
                     (g_cfg.deepseek_api_key && g_cfg.deepseek_api_key[0]) ? "(已设置)" : "(未设置)");
            break;
        case 4: snprintf(out, outsz, "%s", g_cfg.model); break;
        case 5:
            snprintf(out, outsz, "%s",
                     (g_cfg.deepseek_thinking &&
                      strcmp(g_cfg.deepseek_thinking, "enabled") == 0) ? "enabled" : "disabled");
            break;
        case 6:
            snprintf(out, outsz, "%s",
                     (g_cfg.system_prompt && g_cfg.system_prompt[0]) ? g_cfg.system_prompt : "(空)");
            break;
        case 7:
            snprintf(out, outsz, "%s", g_voice_enabled ? "开" : "关");
            break;
        case 8:
            snprintf(out, outsz, "%s",
                     (g_cfg.stt_url && g_cfg.stt_url[0]) ? g_cfg.stt_url : "(空)");
            break;
        case 9:  snprintf(out, outsz, "%d", g_cfg.tts_rate);   break;
        case 10: snprintf(out, outsz, "%d", g_cfg.tts_volume); break;
        case 11: snprintf(out, outsz, "%d", g_cfg.tts_pitch);  break;
        case 12: snprintf(out, outsz, "%s", "(按 A 导出当前对话)"); break;
        default: out[0] = '\0';
    }
}

static void set_edit_field(char **field, const char *label, int zh, int password) {
    char buf[4096];
    if (textinput_prompt(label, *field ? *field : "", zh, password, buf, sizeof(buf)) == 1) {
        free(*field);
        *field = strdup(buf);
    }
}

/* ---------- API Key 子菜单 ---------- */

#define KEY_MENU_ROWS 3

static void key_menu_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    if (kDown & HidNpadButton_B) {
        g_key_menu_open = 0;
        g_dirty = 1;
        return;
    }
    if (kDown & HidNpadButton_Down) {
        if (g_key_menu_idx < KEY_MENU_ROWS - 1) g_key_menu_idx++;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        if (g_key_menu_idx > 0) g_key_menu_idx--;
        g_dirty = 1;
    }
    int tidx = tap_row_index(HEADER_H + 150, 64, 8);
    if (tidx >= 0 && tidx < KEY_MENU_ROWS) {
        g_key_menu_idx = tidx;
        kDown |= HidNpadButton_A;
    }
    if (!(kDown & HidNpadButton_A)) return;

    switch (g_key_menu_idx) {
        case 0: {
            char msg[256];
            if (config_reload_key(&g_cfg, msg, sizeof(msg)) == 1)
                snprintf(g_key_menu_msg, sizeof(g_key_menu_msg), "已从 key.txt 加载 Key");
            else
                snprintf(g_key_menu_msg, sizeof(g_key_menu_msg), "%s", msg);
            break;
        }
        case 1:
            set_edit_field(&g_cfg.deepseek_api_key, "API Key(手动输入,密码式)", 0, 1);
            snprintf(g_key_menu_msg, sizeof(g_key_menu_msg),
                     "已更新(离开设置时同步写回 key.txt)");
            break;
        case 2:
            free(g_cfg.deepseek_api_key);
            g_cfg.deepseek_api_key = strdup("");
            snprintf(g_key_menu_msg, sizeof(g_key_menu_msg), "已清空 Key");
            break;
    }
    g_dirty = 1;
}

static void render_key_menu(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, "API Key 设置", COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    char status[160];
    snprintf(status, sizeof(status), "当前:%s",
             (g_cfg.deepseek_api_key && g_cfg.deepseek_api_key[0]) ? "已设置" : "未设置");
    ts = TTF_RenderUTF8_Blended(g_font, status,
                                (g_cfg.deepseek_api_key && g_cfg.deepseek_api_key[0])
                                    ? COL_GREEN : COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 60, HEADER_H + 24, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    const char *rows[KEY_MENU_ROWS] = {
        "从 key.txt 读取(推荐,免手输)",
        "手动输入(软键盘,密码式)",
        "清空 Key",
    };
    int y = HEADER_H + 150;
    for (int i = 0; i < KEY_MENU_ROWS; i++) {
        SDL_Rect row = { 60, y, WIN_W - 120, 64 };
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       i == g_key_menu_idx ? COL_SURF2.r : COL_SURF.r,
                       i == g_key_menu_idx ? COL_SURF2.g : COL_SURF.g,
                       i == g_key_menu_idx ? COL_SURF2.b : COL_SURF.b, 255);
        ts = TTF_RenderUTF8_Blended(g_font, rows[i], i == 0 ? COL_BRAND : COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + (row.h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += 72;
    }

    if (g_key_menu_msg[0]) {
        draw_trunc(g_font_hint, g_key_menu_msg, COL_TEXT2, 60, y + 8, WIN_W - 140);
    }

    const char *hint = "方向键/点击 选择    A 执行    B 返回设置    + 退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, WIN_H - 48, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
}

static void settings_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    if (g_key_menu_open) {
        key_menu_input(kDown);
        return;
    }
    /* 触摸:点行 = 选中并触发 */
    if (g_tap_x >= 0) {
        int y0 = HEADER_H + 20;
        int idx = (g_tap_y - y0) / 76;
        if (g_tap_y >= y0 && idx >= 0 && idx < SET_COUNT) {
            g_set_idx = idx;
            kDown |= HidNpadButton_A;
        }
        g_tap_x = -1;
        g_tap_y = -1;
    }
    if (kDown & HidNpadButton_B) {
        config_save(&g_cfg);
        printf("settings: saved, backend=%s\n", g_cfg.backend);
        g_screen = SCREEN_CHAT;
        g_dirty = 1;
        return;
    }
    if (kDown & HidNpadButton_Down) {
        g_set_idx = (g_set_idx + 1) % SET_COUNT;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        g_set_idx = (g_set_idx + SET_COUNT - 1) % SET_COUNT;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_A) {
        switch (g_set_idx) {
            case 0:
                free(g_cfg.backend);
                g_cfg.backend = strdup(strcmp(g_cfg.backend, "deepseek") == 0
                                           ? "harness" : "deepseek");
                break;
            case 1: set_edit_field(&g_cfg.harness_base_url, "Harness 地址(指向 dsh-bridge)", 0, 0); break;
            case 2: set_edit_field(&g_cfg.deepseek_base_url, "DeepSeek 地址", 0, 0); break;
            case 3:
                g_key_menu_open = 1;
                g_key_menu_idx = 0;
                g_key_menu_msg[0] = '\0';
                break;
            case 4: set_edit_field(&g_cfg.model, "模型(如 deepseek-v4-pro)", 0, 0); break;
            case 5:
                free(g_cfg.deepseek_thinking);
                g_cfg.deepseek_thinking = strdup(strcmp(g_cfg.deepseek_thinking, "enabled") == 0
                                                     ? "disabled" : "enabled");
                break;
            case 6: set_edit_field(&g_cfg.system_prompt, "系统提示词", 1, 0); break;
            case 7:
                g_voice_enabled = !g_voice_enabled;
                if (!g_voice_enabled) tts_stop();
                break;
            case 8:
                set_edit_field(&g_cfg.stt_url,
                               "STT 服务地址(如 http://192.168.1.10:9000)", 0, 0);
                break;
            case 9: { /* 语速: 循环选择预设值 */
                static const int rates[] = {120, 150, 175, 200, 250, 300};
                int n = (int)(sizeof(rates)/sizeof(rates[0]));
                for (int r = 0; r < n; r++)
                    if (rates[r] >= g_cfg.tts_rate) { g_cfg.tts_rate = rates[(r+1)%n]; break; }
                tts_set_params(g_cfg.tts_rate, g_cfg.tts_volume, g_cfg.tts_pitch);
                break;
            }
            case 10: { /* 音量 */
                static const int vols[] = {30, 60, 100, 150, 200};
                int n = (int)(sizeof(vols)/sizeof(vols[0]));
                for (int r = 0; r < n; r++)
                    if (vols[r] >= g_cfg.tts_volume) { g_cfg.tts_volume = vols[(r+1)%n]; break; }
                tts_set_params(g_cfg.tts_rate, g_cfg.tts_volume, g_cfg.tts_pitch);
                break;
            }
            case 11: { /* 音调 */
                static const int pitches[] = {25, 40, 50, 65, 80};
                int n = (int)(sizeof(pitches)/sizeof(pitches[0]));
                for (int r = 0; r < n; r++)
                    if (pitches[r] >= g_cfg.tts_pitch) { g_cfg.tts_pitch = pitches[(r+1)%n]; break; }
                tts_set_params(g_cfg.tts_rate, g_cfg.tts_volume, g_cfg.tts_pitch);
                break;
            }
            case 12: /* 导出对话到 SD */
                export_conversation();
                break;
        }
        g_dirty = 1;
    }
}

static void render_settings(void) {
    if (g_key_menu_open) {
        render_key_menu();
        return;
    }
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    SDL_SetRenderDrawColor(g_ren, COL_HEADER.r, COL_HEADER.g, COL_HEADER.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);

    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, "设置", COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    const int row_h = 76;
    /* 自动滚动:确保选中项始终可见 */
    {
        int vis_h = WIN_H - HEADER_H - 80;
        int top_y = g_set_idx * row_h + HEADER_H + 20;
        if (top_y < g_set_scroll + 10) g_set_scroll = top_y - 10;
        int bot_y = (g_set_idx + 1) * row_h + HEADER_H + 20;
        if (bot_y > g_set_scroll + vis_h) g_set_scroll = bot_y - vis_h;
        if (g_set_scroll < 0) g_set_scroll = 0;
    }
    for (int i = 0; i < SET_COUNT; i++) {
        int y = HEADER_H + 20 + i * row_h - g_set_scroll;
        if (y + row_h < HEADER_H || y > WIN_H) continue; /* 跳出屏幕的行 */

        /* 选中高亮(layer-2 圆角) */
        if (i == g_set_idx) {
            roundedBoxRGBA(g_ren, 20, (Sint16)(y - 6), WIN_W - 40,
                           (Sint16)(y - 6 + row_h - 4), 8,
                           COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
        }

        char label[64];
        snprintf(label, sizeof(label), "%s", set_label(i));
        ts = TTF_RenderUTF8_Blended(g_font_hint, label, COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 40, y, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }

        char value[512];
        set_value(i, value, sizeof(value));
        draw_trunc(g_font_hint, value, COL_ACCENT, 420, y, WIN_W - 420 - 40);
    }

    const char *hint = "方向键 选择    A 修改/切换    B 保存并返回    + 退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_HINT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 40, WIN_H - 64, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
}

/* 前置声明 */
static void enter_sessions(int from_startup);
static void render_sessions(void);
static void enter_workspaces(int from_startup);
static void render_workspaces(void);
static void enter_ws_sessions(int ws_idx);
static void render_ws_sessions(void);
static void enter_models(void);
static void render_models(void);
static void search_start(void);
static void search_load(void);
static void search_input(u64 kDown);
static void render_search(void);

/* ---------- 启动后端选择界面 ---------- */

static void choice_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    /* 触摸:点卡片 = 选中并确认 */
    if (g_tap_x >= 0) {
        for (int i = 0; i < 2; i++) {
            SDL_Rect card = { 140, 210 + i * 190, WIN_W - 280, 160 };
            if (g_tap_x >= card.x && g_tap_x <= card.x + card.w &&
                g_tap_y >= card.y && g_tap_y <= card.y + card.h) {
                g_choice_idx = i;
                kDown |= HidNpadButton_A;
                break;
            }
        }
        g_tap_x = -1;
        g_tap_y = -1;
    }
    if ((kDown & (HidNpadButton_Up | HidNpadButton_Down)) ||
        (kDown & (HidNpadButton_Left | HidNpadButton_Right))) {
        g_choice_idx = 1 - g_choice_idx;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_A) {
        const char *be = g_choice_idx == 0 ? "harness" : "deepseek";
        free(g_cfg.backend);
        g_cfg.backend = strdup(be);
        config_save(&g_cfg);
        printf("choice: backend=%s\n", g_cfg.backend);
        if (g_choice_idx == 0) {
            /* Harness:恢复本地缓冲,进聊天后自动拉服务端历史 */
            restore_active(g_buf_h, &g_bufn_h, g_bufscroll_h, g_buffirst_h);
            snprintf(g_sess_title, sizeof(g_sess_title), "上次会话");
            g_chat_resumed = 0;
        } else {
            restore_active(g_buf_d, &g_bufn_d, g_bufscroll_d, g_buffirst_d);
            snprintf(g_sess_title, sizeof(g_sess_title), "DeepSeek 对话");
            g_chat_resumed = 1;
        }
        g_focus = 0;
        g_sb_idx = 0;
        g_sb_loaded = 0;
        g_screen = SCREEN_CHAT;
        g_dirty = 1;
    }
}

static void render_choice(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    /* LOGO(居中) */
    if (g_logo_tex) {
        SDL_Rect ld = { (WIN_W - 96) / 2, 36, 96, 96 };
        SDL_RenderCopy(g_ren, g_logo_tex, NULL, &ld);
    }

    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, "选择后端", COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { (WIN_W - ts->w) / 2, 146, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    const char *titles[2] = { "Harness(局域网)", "DeepSeek(官方 API)" };
    const char *descs[2] = {
        "连接本机 DeepSeek Harness",
        "直连 api.deepseek.com",
    };
    const char *subs[2] = {
        "需要 PC 端运行 dsh-bridge 桥接",
        "需要 API Key(设置界面填写)",
    };

    for (int i = 0; i < 2; i++) {
        SDL_Rect card = { 140, 210 + i * 190, WIN_W - 280, 160 };
        roundedBoxRGBA(g_ren, (Sint16)card.x, (Sint16)card.y,
                       (Sint16)(card.x + card.w), (Sint16)(card.y + card.h), 12,
                       COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
        if (i == g_choice_idx) {
            /* 选中:DeepSeek 蓝描边 */
            roundedRectangleRGBA(g_ren, (Sint16)card.x, (Sint16)card.y,
                                 (Sint16)(card.x + card.w), (Sint16)(card.y + card.h), 12,
                                 COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);
            roundedRectangleRGBA(g_ren, (Sint16)(card.x + 1), (Sint16)(card.y + 1),
                                 (Sint16)(card.x + card.w - 1), (Sint16)(card.y + card.h - 1), 11,
                                 COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);
        } else {
            roundedRectangleRGBA(g_ren, (Sint16)card.x, (Sint16)card.y,
                                 (Sint16)(card.x + card.w), (Sint16)(card.y + card.h), 12,
                                 COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
        }

        /* 选中标记 */
        ts = TTF_RenderUTF8_Blended(g_font_title,
                                    i == g_choice_idx ? "> " : "  ", COL_BRAND);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { card.x + 28, card.y + 20, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }

        ts = TTF_RenderUTF8_Blended(g_font, titles[i], COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { card.x + 84, card.y + 16, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }

        ts = TTF_RenderUTF8_Blended(g_font_hint, descs[i], COL_TEXT2);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { card.x + 84, card.y + 66, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }

        ts = TTF_RenderUTF8_Blended(g_font_hint, subs[i], COL_HINT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { card.x + 84, card.y + 108, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
    }

    const char *hint = "方向键 选择    A 确定    + 退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_HINT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { (WIN_W - ts->w) / 2, WIN_H - 56, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
}

/* ---------- 会话列表界面 ---------- */

static void clear_msgs(void) {
    while (g_nmsgs > 0) {
        g_nmsgs--;
        msg_tex_destroy(&g_msgs[g_nmsgs]);
        free(g_msgs[g_nmsgs].text);
        free(g_msgs[g_nmsgs].think);
        free(g_msgs[g_nmsgs].tools);
        free(g_msgs[g_nmsgs].notice);
        g_msgs[g_nmsgs].text = NULL;
        g_msgs[g_nmsgs].think = NULL;
        g_msgs[g_nmsgs].tools = NULL;
        g_msgs[g_nmsgs].notice = NULL;
    }
    g_todos_str[0] = '\0';
    g_scroll_offset = 0;
    g_first_seq = -1;
}

static void enter_sessions(int from_startup) {
    g_sess_from_startup = from_startup;
    g_sess_loaded = 0;
    g_sess_idx = 0;
    g_sess_err[0] = '\0';
    harness_sessions_free(g_sessions, g_sessions_n);
    g_sessions = NULL;
    g_sessions_n = 0;
    g_screen = SCREEN_SESSIONS;
    g_dirty = 1;
}

static void fmt_time(long long epoch_ms, char *out, size_t outsz) {
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, outsz, "%02d-%02d %02d:%02d",
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

static void sessions_load(void) {
    render_sessions(); /* 先渲染加载态 */
    g_dirty = 0;
    char err[256] = {0};
    if (harness_list_sessions(&g_cfg, &g_sessions, &g_sessions_n,
                              err, sizeof(err)) != 0) {
        snprintf(g_sess_err, sizeof(g_sess_err), "%s", err[0] ? err : "加载失败");
    }
    g_sess_loaded = 1;
    g_dirty = 1;
}

static void sessions_pick(void) {
    char err[256] = {0};
    const char *sid = NULL;
    if (g_sess_idx > 0 && (size_t)g_sess_idx <= g_sessions_n)
        sid = g_sessions[g_sess_idx - 1].session_id;

    if (harness_use_session(&g_cfg, sid, err, sizeof(err)) != 0) {
        snprintf(g_sess_err, sizeof(g_sess_err), "%s", err[0] ? err : "切换失败");
        g_dirty = 1;
        return;
    }

    clear_msgs();
    if (sid) {
        chat_message_t *msgs = NULL;
        size_t n = 0;
        long long first = -1;
        if (harness_fetch_history_ex(&g_cfg, 0, &msgs, &n, &first,
                                     err, sizeof(err)) == 0) {
            for (size_t i = 0; i < n; i++)
                add_msg(msgs[i].role, msgs[i].content ? msgs[i].content : "", 1);
            for (size_t i = 0; i < n; i++) free(msgs[i].content);
            free(msgs);
            g_first_seq = first;
        } else {
            add_msg(ROLE_ASSISTANT, "历史加载失败,从新消息开始。", 1);
        }
        if (g_nmsgs == 0)
            add_msg(ROLE_ASSISTANT, "(该会话暂无聊天记录)\n按 A 开始输入。", 1);
    } else {
        add_msg(ROLE_ASSISTANT,
                "已新建会话。\nA 输入消息;X 切换会话;Y 设置;+ 退出。", 1);
        snprintf(g_sess_title, sizeof(g_sess_title), "新会话");
    }
    if (sid && g_sess_idx > 0 && (size_t)g_sess_idx <= g_sessions_n)
        snprintf(g_sess_title, sizeof(g_sess_title), "%s",
                 g_sessions[g_sess_idx - 1].title);
    g_screen = SCREEN_CHAT;
    g_dirty = 1;
}

static void sessions_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    if (kDown & HidNpadButton_B) {
        g_screen = g_sess_from_startup ? SCREEN_CHOICE : g_sess_back;
        g_dirty = 1;
        return;
    }
    if (!g_sess_loaded) return;
    int max_idx = (int)g_sessions_n; /* 0 = 新建会话 */
    int tidx = tap_row_index(HEADER_H + 16, 64, 8);
    if (tidx >= 0 && tidx <= max_idx) {
        g_sess_idx = tidx;
        kDown |= HidNpadButton_A;
    }
    if (kDown & HidNpadButton_Down) {
        if (g_sess_idx < max_idx) g_sess_idx++;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        if (g_sess_idx > 0) g_sess_idx--;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Y) {
        if (g_sess_idx > 0 && (size_t)g_sess_idx <= g_sessions_n) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", g_sessions[g_sess_idx - 1].title);
            if (textinput_prompt("会话标题", buf, 1, 0, buf, sizeof(buf)) == 1) {
                char err2[256] = {0};
                if (harness_rename_session(&g_cfg, g_sessions[g_sess_idx - 1].session_id,
                                           buf, err2, sizeof(err2)) == 0) {
                    g_sess_loaded = 0;
                    sessions_load();
                } else {
                    snprintf(g_sess_err, sizeof(g_sess_err), "%s",
                             err2[0] ? err2 : "重命名失败");
                }
            }
        }
        g_dirty = 1;
        return;
    }
    if (kDown & HidNpadButton_X) {
        if (g_sess_idx > 0 && (size_t)g_sess_idx <= g_sessions_n) {
            char fid[128] = "";
            char err2[256] = {0};
            if (harness_fork_session(&g_cfg, g_sessions[g_sess_idx - 1].session_id,
                                     fid, sizeof(fid), err2, sizeof(err2)) == 0 && fid[0]) {
                if (harness_use_session(&g_cfg, fid, err2, sizeof(err2)) == 0) {
                    clear_msgs();
                    add_msg(ROLE_ASSISTANT, "已分叉新会话(复制当前会话)。\nA 输入消息开始。", 1);
                    g_screen = SCREEN_CHAT;
                }
            } else {
                snprintf(g_sess_err, sizeof(g_sess_err), "%s",
                         err2[0] ? err2 : "分叉失败");
            }
        }
        g_dirty = 1;
        return;
    }
    if (kDown & HidNpadButton_A) sessions_pick();
}

static void render_sessions(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, "会话", COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
    ts = TTF_RenderUTF8_Blended(g_font_hint, "电脑端 Harness 的会话列表", COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 160, (HEADER_H - ts->h) / 2 + 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    const int row_h = 64;
    int y = HEADER_H + 16;

    if (!g_sess_loaded && !g_sess_err[0]) {
        draw_trunc(g_font, "正在加载会话列表…", COL_TEXT2, 60, y + 30, WIN_W - 120);
        return;
    }

    if (g_sess_err[0]) {
        draw_trunc(g_font, g_sess_err, COL_RED, 60, y + 30, WIN_W - 120);
        const char *hint = "B 返回重试(请确认桥接已启动、地址正确)";
        ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_TEXT3);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 110, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        return;
    }

    /* 第一行:新建会话 */
    {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        if (g_sess_idx == 0)
            roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                           (Sint16)(row.y + row.h), 10,
                           COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
        ts = TTF_RenderUTF8_Blended(g_font, "＋ 新建会话", COL_BRAND);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + (row_h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
    }

    for (size_t i = 0; i < g_sessions_n; i++) {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        if ((int)i + 1 == g_sess_idx)
            roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                           (Sint16)(row.y + row.h), 10,
                           COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
        else
            roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                           (Sint16)(row.y + row.h), 10,
                           COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);

        /* 标题(像素级截断,右侧留状态栏空间) */
        draw_trunc(g_font, g_sessions[i].title, COL_TEXT,
                   row.x + 20, row.y + 10, WIN_W - 48 - 240);

        /* 右侧:状态 + 时间 */
        char meta[64];
        if (g_sessions[i].running) snprintf(meta, sizeof(meta), "运行中");
        else fmt_time(g_sessions[i].updated_at, meta, sizeof(meta));
        ts = TTF_RenderUTF8_Blended(g_font_hint, meta,
                                    g_sessions[i].running ? COL_GREEN : COL_TEXT3);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + row.w - ts->w - 20, row.y + (row_h - ts->h) / 2,
                           ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
        if (y > WIN_H - 100) break; /* 一屏装不下就截断 */
    }

    const char *hint = "方向键 选择    A 打开    Y 重命名    X 分叉    B 返回    + 退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, WIN_H - 48, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
}

/* ---------- 工作区界面 ---------- */

static void enter_workspaces(int from_startup) {
    g_sess_from_startup = from_startup; /* 复用:B 返回 */
    g_ws_loaded = 0;
    g_ws_idx = 0;
    g_ws_err[0] = '\0';
    g_ws_confirm = -1;
    harness_workspaces_free(g_wss, g_wss_n);
    g_wss = NULL;
    g_wss_n = 0;
    g_screen = SCREEN_WORKSPACES;
    g_dirty = 1;
}

static void ws_load(void) {
    render_workspaces();
    g_dirty = 0;
    char err[256] = {0};
    if (harness_list_workspaces(&g_cfg, &g_wss, &g_wss_n, err, sizeof(err)) != 0) {
        snprintf(g_ws_err, sizeof(g_ws_err), "%s", err[0] ? err : "加载失败");
    }
    g_ws_loaded = 1;
    g_dirty = 1;
}

static void ws_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    if (g_ws_confirm >= 0) {
        /* 删除确认态 */
        if (kDown & HidNpadButton_A) {
            size_t wi = (size_t)g_ws_confirm - 2;
            if (wi < g_wss_n) {
                char err2[256] = {0};
                if (harness_delete_workspace(&g_cfg, g_wss[wi].workspace_id,
                                             err2, sizeof(err2)) == 0) {
                    g_ws_loaded = 0;
                    g_ws_idx = 0;
                    ws_load();
                } else {
                    snprintf(g_ws_err, sizeof(g_ws_err), "%s",
                             err2[0] ? err2 : "删除失败");
                }
            }
            g_ws_confirm = -1;
            g_dirty = 1;
        } else if (kDown & (HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y)) {
            g_ws_confirm = -1;
            g_dirty = 1;
        }
        return;
    }
    if (kDown & HidNpadButton_B) {
        g_screen = g_sess_from_startup ? SCREEN_CHOICE : SCREEN_CHAT;
        g_dirty = 1;
        return;
    }
    if (!g_ws_loaded) return;
    int ws_count = (int)g_wss_n;
    int max_idx = ws_count + 3; /* 0 新建 1 新工作区 2..n+1 工作区 n+2 搜索 n+3 全部 */
    int tidx = tap_row_index(HEADER_H + 16, 64, 8);
    if (tidx >= 0 && tidx <= max_idx) {
        g_ws_idx = tidx;
        kDown |= HidNpadButton_A;
    }
    if (kDown & HidNpadButton_Down) {
        if (g_ws_idx < max_idx) g_ws_idx++;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        if (g_ws_idx > 0) g_ws_idx--;
        g_dirty = 1;
    }

    /* Y:重命名工作区 */
    if (kDown & HidNpadButton_Y) {
        if (g_ws_idx >= 2 && g_ws_idx <= ws_count + 1) {
            size_t wi = (size_t)g_ws_idx - 2;
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", g_wss[wi].title);
            if (textinput_prompt("工作区标题", buf, 1, 0, buf, sizeof(buf)) == 1) {
                char err2[256] = {0};
                if (harness_rename_workspace(&g_cfg, g_wss[wi].workspace_id,
                                             buf, err2, sizeof(err2)) == 0) {
                    g_ws_loaded = 0;
                    ws_load();
                } else {
                    snprintf(g_ws_err, sizeof(g_ws_err), "%s",
                             err2[0] ? err2 : "重命名失败");
                }
            }
        }
        g_dirty = 1;
        return;
    }
    /* X:删除工作区(需再按 A 确认) */
    if (kDown & HidNpadButton_X) {
        if (g_ws_idx >= 2 && g_ws_idx <= ws_count + 1) g_ws_confirm = g_ws_idx;
        g_dirty = 1;
        return;
    }
    /* ZL/ZR:排序 */
    if (g_ws_idx >= 2 && g_ws_idx <= ws_count + 1) {
        size_t wi = (size_t)g_ws_idx - 2;
        const char *before = NULL;
        int do_it = 0;
        if (kDown & HidNpadButton_ZL) {
            do_it = 1;
            before = (wi > 0) ? g_wss[wi - 1].workspace_id : g_wss[wi].workspace_id;
        } else if (kDown & HidNpadButton_ZR) {
            do_it = 1;
            before = (wi + 2 < g_wss_n) ? g_wss[wi + 2].workspace_id : NULL;
        }
        if (do_it) {
            char err2[256] = {0};
            if (harness_reorder_workspace(&g_cfg, g_wss[wi].workspace_id,
                                          before, err2, sizeof(err2)) == 0) {
                g_ws_loaded = 0;
                ws_load();
            } else {
                snprintf(g_ws_err, sizeof(g_ws_err), "%s",
                         err2[0] ? err2 : "排序失败");
            }
            g_dirty = 1;
        }
        return;
    }

    if (!(kDown & HidNpadButton_A)) return;

    if (g_ws_idx == 0) {
        char err[256] = {0};
        if (harness_new_session_in(&g_cfg, NULL, err, sizeof(err)) == 0) {
            clear_msgs();
            add_msg(ROLE_ASSISTANT, "已新建会话。\nA 输入消息;X 工作区;L 模型;+ 退出。", 1);
            g_screen = SCREEN_CHAT;
        } else {
            snprintf(g_ws_err, sizeof(g_ws_err), "%s", err[0] ? err : "新建失败");
        }
        g_dirty = 1;
        return;
    }
    if (g_ws_idx == 1) {
        char path[512] = "";
        if (textinput_prompt("新工作区:输入已有目录绝对路径",
                             "C:\\", 0, 0,
                             path, sizeof(path)) == 1) {
            char err[256] = {0};
            if (harness_create_workspace(&g_cfg, path, err, sizeof(err)) == 0) {
                g_ws_loaded = 0; /* 重新加载列表 */
                g_ws_idx = 0;
                ws_load();
            } else {
                snprintf(g_ws_err, sizeof(g_ws_err), "%s", err[0] ? err : "创建失败");
            }
        }
        g_dirty = 1;
        return;
    }
    if (g_ws_idx == ws_count + 2) {
        search_start();
        return;
    }
    if (g_ws_idx == ws_count + 3) {
        g_sess_back = SCREEN_WORKSPACES;
        enter_sessions(0);
        return;
    }
    enter_ws_sessions(g_ws_idx - 2);
}

static void render_workspaces(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, "工作区", COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
    ts = TTF_RenderUTF8_Blended(g_font_hint, "Harness 工作区与会话", COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 160, (HEADER_H - ts->h) / 2 + 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    const int row_h = 64;
    int y = HEADER_H + 16;

    if (!g_ws_loaded && !g_ws_err[0]) {
        draw_trunc(g_font, "正在加载工作区…", COL_TEXT2, 60, y + 30, WIN_W - 120);
        return;
    }
    if (g_ws_err[0]) {
        draw_trunc(g_font, g_ws_err, COL_RED, 60, y + 30, WIN_W - 120);
        return;
    }

    /* 行 0:新建会话;行 1:新建工作区 */
    const char *rows0[2] = { "＋ 新建会话", "＋ 新建工作区(采纳已有目录)" };
    for (int i = 0; i < 2; i++) {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        if (g_ws_idx == i)
            roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                           (Sint16)(row.y + row.h), 10,
                           COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
        ts = TTF_RenderUTF8_Blended(g_font, rows0[i], COL_BRAND);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + (row_h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
    }

    for (size_t i = 0; i < g_wss_n; i++) {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int sel = (int)i + 2 == g_ws_idx;
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       sel ? COL_SURF2.r : COL_SURF.r,
                       sel ? COL_SURF2.g : COL_SURF.g,
                       sel ? COL_SURF2.b : COL_SURF.b, 255);

        char label[200];
        snprintf(label, sizeof(label), "%s", g_wss[i].title);
        draw_trunc(g_font, label, COL_TEXT, row.x + 20, row.y + 10, WIN_W - 48 - 60);

        char meta[128];
        snprintf(meta, sizeof(meta), "%zu 会话 · %s", g_wss[i].session_count,
                 g_wss[i].path ? g_wss[i].path : "");
        draw_trunc(g_font_hint, meta, COL_TEXT3,
                   row.x + 20, row.y + row_h - TTF_FontHeight(g_font_hint) - 8,
                   WIN_W - 48 - 60);
        y += row_h + 8;
        if (y > WIN_H - 140) break;
    }

    /* 搜索会话 */
    {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int sel = g_ws_idx == (int)g_wss_n + 2;
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       sel ? COL_SURF2.r : COL_SURF.r,
                       sel ? COL_SURF2.g : COL_SURF.g,
                       sel ? COL_SURF2.b : COL_SURF.b, 255);
        ts = TTF_RenderUTF8_Blended(g_font, "搜索会话", COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + (row_h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
    }

    /* 全部会话(平铺) */
    {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int sel = g_ws_idx == (int)g_wss_n + 3;
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       sel ? COL_SURF2.r : COL_SURF.r,
                       sel ? COL_SURF2.g : COL_SURF.g,
                       sel ? COL_SURF2.b : COL_SURF.b, 255);
        ts = TTF_RenderUTF8_Blended(g_font, "全部会话(平铺)", COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + (row_h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
    }

    /* 删除确认态:选中行加红框 + 提示 */
    if (g_ws_confirm >= 0 && g_ws_confirm <= (int)g_wss_n + 1) {
        int cy = HEADER_H + 16 + g_ws_confirm * (row_h + 8);
        roundedRectangleRGBA(g_ren, 24, (Sint16)cy, WIN_W - 24,
                             (Sint16)(cy + row_h), 10,
                             COL_RED.r, COL_RED.g, COL_RED.b, 255);
    }

    const char *hint = g_ws_confirm >= 0
                           ? "再按 A 确认删除该工作区    B/X/Y 取消"
                           : "方向键选择 A打开 Y改名 X删除 ZL/ZR排序 B返回 +退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, WIN_H - 48, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
}

/* ---------- 工作区内会话 ---------- */

static int g_ws_sel = -1;

static void enter_ws_sessions(int ws_idx) {
    g_ws_sel = ws_idx;
    g_wss_loaded = 0;
    g_wss_idx = 0;
    g_wss_err[0] = '\0';
    harness_sessions_free(g_wss_sessions, g_wss_sessions_n);
    g_wss_sessions = NULL;
    g_wss_sessions_n = 0;
    g_screen = SCREEN_WS_SESSIONS;
    g_dirty = 1;
}

static void ws_sessions_load(void) {
    render_ws_sessions();
    g_dirty = 0;
    char err[256] = {0};
    harness_session_t *all = NULL;
    size_t all_n = 0;
    if (harness_list_sessions(&g_cfg, &all, &all_n, err, sizeof(err)) != 0) {
        snprintf(g_wss_err, sizeof(g_wss_err), "%s", err[0] ? err : "加载失败");
    } else {
        const char *path = (g_ws_sel >= 0 && (size_t)g_ws_sel < g_wss_n)
                               ? g_wss[g_ws_sel].path : NULL;
        size_t k = 0;
        harness_session_t *sel = calloc(all_n ? all_n : 1, sizeof(*sel));
        if (sel) {
            for (size_t i = 0; i < all_n; i++) {
                if (path && (!all[i].cwd || strcmp(all[i].cwd, path) != 0)) continue;
                sel[k].session_id = all[i].session_id;
                sel[k].title = all[i].title;
                sel[k].cwd = all[i].cwd;
                sel[k].running = all[i].running;
                sel[k].updated_at = all[i].updated_at;
                all[i].session_id = NULL;
                all[i].title = NULL;
                all[i].cwd = NULL;
                k++;
            }
            g_wss_sessions = sel;
            g_wss_sessions_n = k;
        } else {
            snprintf(g_wss_err, sizeof(g_wss_err), "内存不足");
        }
        harness_sessions_free(all, all_n);
    }
    g_wss_loaded = 1;
    g_dirty = 1;
}

static void ws_sessions_pick(int row_idx) {
    char err[256] = {0};
    if (row_idx == 0) {
        const char *wid = (g_ws_sel >= 0 && (size_t)g_ws_sel < g_wss_n)
                              ? g_wss[g_ws_sel].workspace_id : NULL;
        if (harness_new_session_in(&g_cfg, wid, err, sizeof(err)) != 0) {
            snprintf(g_wss_err, sizeof(g_wss_err), "%s", err[0] ? err : "新建失败");
            g_dirty = 1;
            return;
        }
        clear_msgs();
        add_msg(ROLE_ASSISTANT, "已在本工作区新建会话。\nA 输入消息开始。", 1);
        g_screen = SCREEN_CHAT;
        g_dirty = 1;
        return;
    }
    if (harness_use_session(&g_cfg, g_wss_sessions[row_idx - 1].session_id,
                            err, sizeof(err)) != 0) {
        snprintf(g_wss_err, sizeof(g_wss_err), "%s", err[0] ? err : "切换失败");
        g_dirty = 1;
        return;
    }
    snprintf(g_sess_title, sizeof(g_sess_title), "%s",
             g_wss_sessions[row_idx - 1].title);
    clear_msgs();
    chat_message_t *msgs = NULL;
    size_t n = 0;
    long long first = -1;
    if (harness_fetch_history_ex(&g_cfg, 0, &msgs, &n, &first,
                                 err, sizeof(err)) == 0) {
        for (size_t i = 0; i < n; i++)
            add_msg(msgs[i].role, msgs[i].content ? msgs[i].content : "", 1);
        for (size_t i = 0; i < n; i++) free(msgs[i].content);
        free(msgs);
        g_first_seq = first;
    } else {
        add_msg(ROLE_ASSISTANT, "历史加载失败,从新消息开始。", 1);
    }
    if (g_nmsgs == 0)
        add_msg(ROLE_ASSISTANT, "(该会话暂无聊天记录)\n按 A 开始输入。", 1);
    g_screen = SCREEN_CHAT;
    g_dirty = 1;
}

static void ws_sessions_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    if (kDown & HidNpadButton_B) {
        enter_workspaces(g_sess_from_startup);
        return;
    }
    if (!g_wss_loaded) return;
    int max_idx = (int)g_wss_sessions_n;
    int tidx = tap_row_index(HEADER_H + 16, 64, 8);
    if (tidx >= 0 && tidx <= max_idx) {
        g_wss_idx = tidx;
        kDown |= HidNpadButton_A;
    }
    if (kDown & HidNpadButton_Down) {
        if (g_wss_idx < max_idx) g_wss_idx++;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        if (g_wss_idx > 0) g_wss_idx--;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Y) {
        if (g_wss_idx > 0 && (size_t)g_wss_idx <= g_wss_sessions_n) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", g_wss_sessions[g_wss_idx - 1].title);
            if (textinput_prompt("会话标题", buf, 1, 0, buf, sizeof(buf)) == 1) {
                char err2[256] = {0};
                if (harness_rename_session(&g_cfg,
                                           g_wss_sessions[g_wss_idx - 1].session_id,
                                           buf, err2, sizeof(err2)) == 0) {
                    g_wss_loaded = 0;
                    ws_sessions_load();
                } else {
                    snprintf(g_wss_err, sizeof(g_wss_err), "%s",
                             err2[0] ? err2 : "重命名失败");
                }
            }
        }
        g_dirty = 1;
        return;
    }
    if (kDown & HidNpadButton_X) {
        if (g_wss_idx > 0 && (size_t)g_wss_idx <= g_wss_sessions_n) {
            char fid[128] = "";
            char err2[256] = {0};
            if (harness_fork_session(&g_cfg,
                                     g_wss_sessions[g_wss_idx - 1].session_id,
                                     fid, sizeof(fid), err2, sizeof(err2)) == 0 && fid[0]) {
                if (harness_use_session(&g_cfg, fid, err2, sizeof(err2)) == 0) {
                    clear_msgs();
                    add_msg(ROLE_ASSISTANT, "已分叉新会话(复制当前会话)。\nA 输入消息开始。", 1);
                    g_screen = SCREEN_CHAT;
                }
            } else {
                snprintf(g_wss_err, sizeof(g_wss_err), "%s",
                         err2[0] ? err2 : "分叉失败");
            }
        }
        g_dirty = 1;
        return;
    }
    if (kDown & HidNpadButton_A) ws_sessions_pick(g_wss_idx);
}

static void render_ws_sessions(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    char hdr_title[200];
    if (g_ws_sel >= 0 && (size_t)g_ws_sel < g_wss_n)
        snprintf(hdr_title, sizeof(hdr_title), "%s", g_wss[g_ws_sel].title);
    else
        snprintf(hdr_title, sizeof(hdr_title), "工作区会话");
    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, hdr_title, COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    const int row_h = 64;
    int y = HEADER_H + 16;

    if (!g_wss_loaded && !g_wss_err[0]) {
        draw_trunc(g_font, "正在加载会话…", COL_TEXT2, 60, y + 30, WIN_W - 120);
        return;
    }
    if (g_wss_err[0]) {
        draw_trunc(g_font, g_wss_err, COL_RED, 60, y + 30, WIN_W - 120);
        return;
    }

    {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        if (g_wss_idx == 0)
            roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                           (Sint16)(row.y + row.h), 10,
                           COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
        ts = TTF_RenderUTF8_Blended(g_font, "＋ 在此工作区新建会话", COL_BRAND);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + (row_h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
    }

    for (size_t i = 0; i < g_wss_sessions_n; i++) {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int sel = (int)i + 1 == g_wss_idx;
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       sel ? COL_SURF2.r : COL_SURF.r,
                       sel ? COL_SURF2.g : COL_SURF.g,
                       sel ? COL_SURF2.b : COL_SURF.b, 255);
        draw_trunc(g_font, g_wss_sessions[i].title, COL_TEXT,
                   row.x + 20, row.y + 10, WIN_W - 48 - 240);
        char meta[64];
        if (g_wss_sessions[i].running) snprintf(meta, sizeof(meta), "运行中");
        else fmt_time(g_wss_sessions[i].updated_at, meta, sizeof(meta));
        ts = TTF_RenderUTF8_Blended(g_font_hint, meta,
                                    g_wss_sessions[i].running ? COL_GREEN : COL_TEXT3);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + row.w - ts->w - 20, row.y + (row_h - ts->h) / 2,
                           ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
        if (y > WIN_H - 100) break;
    }

    const char *hint = "方向键 选择    A 打开    Y 重命名    X 分叉    B 返回工作区    + 退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, WIN_H - 48, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
}

/* ---------- 模型选择界面 ---------- */

static void enter_models(void) {
    g_models_loaded = 0;
    g_model_idx = 0;
    g_models_err[0] = '\0';
    backend_models_free(g_models, g_models_n);
    g_models = NULL;
    g_models_n = 0;
    g_screen = SCREEN_MODELS;
    g_dirty = 1;
}

static void models_load(void) {
    render_models();
    g_dirty = 0;
    char err[256] = {0};
    if (backend_list_models(&g_cfg, &g_models, &g_models_n,
                            g_cur_model, sizeof(g_cur_model),
                            g_cur_effort, sizeof(g_cur_effort),
                            err, sizeof(err)) != 0) {
        snprintf(g_models_err, sizeof(g_models_err), "%s", err[0] ? err : "加载失败");
    } else {
        /* 高亮当前模型 */
        g_model_idx = 0;
        for (size_t i = 0; i < g_models_n; i++) {
            if (strcmp(g_models[i].id, g_cur_model) == 0) {
                g_model_idx = (int)i;
                break;
            }
        }
    }
    g_models_loaded = 1;
    g_dirty = 1;
}

static void models_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    if (kDown & HidNpadButton_B) {
        g_screen = SCREEN_CHAT;
        g_dirty = 1;
        return;
    }
    if (!g_models_loaded || g_models_n == 0) return;
    int max_idx = (int)g_models_n + 1; /* 模型行 0..n-1,强度行 n/n+1 */
    int tidx = tap_row_index(HEADER_H + 16, 64, 8);
    if (tidx >= 0 && tidx <= max_idx) {
        g_model_idx = tidx;
        kDown |= HidNpadButton_A;
    }
    if (kDown & HidNpadButton_Down) {
        if (g_model_idx < max_idx) g_model_idx++;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        if (g_model_idx > 0) g_model_idx--;
        g_dirty = 1;
    }
    if (!(kDown & HidNpadButton_A)) return;

    if ((size_t)g_model_idx >= g_models_n) {
        /* 推理强度 */
        const char *effort = (g_model_idx == (int)g_models_n) ? "low" : "high";
        char err[256] = {0};
        if (backend_apply_effort(&g_cfg, effort, err, sizeof(err)) != 0) {
            snprintf(g_models_err, sizeof(g_models_err), "%s",
                     err[0] ? err : "设置失败");
        } else {
            snprintf(g_cur_effort, sizeof(g_cur_effort), "%s", effort);
            if (strcmp(g_cfg.backend, "deepseek") == 0) config_save(&g_cfg);
            models_load(); /* 刷新当前标记 */
        }
        g_dirty = 1;
        return;
    }

    char err[256] = {0};
    if (backend_apply_model(&g_cfg, g_models[g_model_idx].id, err, sizeof(err)) != 0) {
        snprintf(g_models_err, sizeof(g_models_err), "%s", err[0] ? err : "切换失败");
        g_dirty = 1;
        return;
    }
    snprintf(g_cur_model, sizeof(g_cur_model), "%s", g_models[g_model_idx].id);
    if (strcmp(g_cfg.backend, "deepseek") == 0) config_save(&g_cfg);
    printf("models: selected %s\n", g_cur_model);
    g_screen = SCREEN_CHAT;
    g_dirty = 1;
}

static void render_models(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, "选择模型", COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
    char sub[200];
    snprintf(sub, sizeof(sub), "当前: %s · %s", g_cur_model,
             strcmp(g_cfg.backend, "deepseek") == 0 ? "DeepSeek(全局)" : "Harness(本会话)");
    draw_trunc(g_font_hint, sub, COL_TEXT3, 200,
               (HEADER_H - TTF_FontHeight(g_font_hint)) / 2 + 2, WIN_W - 200 - 40);

    const int row_h = 64;
    int y = HEADER_H + 16;

    if (!g_models_loaded && !g_models_err[0]) {
        draw_trunc(g_font, "正在读取模型列表…", COL_TEXT2, 60, y + 30, WIN_W - 120);
        return;
    }
    if (g_models_err[0]) {
        draw_trunc(g_font, g_models_err, COL_RED, 60, y + 30, WIN_W - 120);
        return;
    }

    for (size_t i = 0; i < g_models_n; i++) {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int sel = (int)i == g_model_idx;
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       sel ? COL_SURF2.r : COL_SURF.r,
                       sel ? COL_SURF2.g : COL_SURF.g,
                       sel ? COL_SURF2.b : COL_SURF.b, 255);
        if (strcmp(g_models[i].id, g_cur_model) == 0)
            roundedRectangleRGBA(g_ren, (Sint16)row.x, (Sint16)row.y,
                                 (Sint16)(row.x + row.w), (Sint16)(row.y + row.h), 10,
                                 COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);

        char label[200];
        snprintf(label, sizeof(label), "%s%s", g_models[i].name,
                 strcmp(g_models[i].id, g_cur_model) == 0 ? " (当前)" : "");
        draw_trunc(g_font, label,
                   strcmp(g_models[i].id, g_cur_model) == 0 ? COL_BRAND : COL_TEXT,
                   row.x + 20, row.y + 10, WIN_W - 48 - 60);
        char meta[160];
        if (g_models[i].provider && g_models[i].provider[0])
            snprintf(meta, sizeof(meta), "%s · %s", g_models[i].provider, g_models[i].id);
        else
            snprintf(meta, sizeof(meta), "%s", g_models[i].id);
        draw_trunc(g_font_hint, meta, COL_TEXT3,
                   row.x + 20, row.y + row_h - TTF_FontHeight(g_font_hint) - 8,
                   WIN_W - 48 - 60);
        y += row_h + 8;
        if (y > WIN_H - 100) break;
    }

    /* 推理强度两行 */
    for (int e = 0; e < 2; e++) {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int idx = (int)g_models_n + e;
        int sel = g_model_idx == idx;
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       sel ? COL_SURF2.r : COL_SURF.r,
                       sel ? COL_SURF2.g : COL_SURF.g,
                       sel ? COL_SURF2.b : COL_SURF.b, 255);
        const char *eff = e == 0 ? "low" : "high";
        int is_cur = strcmp(g_cur_effort, eff) == 0;
        if (is_cur)
            roundedRectangleRGBA(g_ren, (Sint16)row.x, (Sint16)row.y,
                                 (Sint16)(row.x + row.w), (Sint16)(row.y + row.h), 10,
                                 COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);
        char label[200];
        snprintf(label, sizeof(label), "推理强度:%s(%s)%s",
                 e == 0 ? "低" : "高",
                 e == 0 ? "少思考,响应快" : "多思考,更深入",
                 is_cur ? " (当前)" : "");
        ts = TTF_RenderUTF8_Blended(g_font, label, is_cur ? COL_BRAND : COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + (row_h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
    }

    const char *hint = "方向键 选择    A 应用    B 返回    + 退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, WIN_H - 48, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
}

/* ---------- 搜索会话 ---------- */

static harness_search_hit_t *g_search = NULL;
static size_t g_search_n = 0;
static int g_search_loaded = 0;
static int g_search_idx = 0;
static char g_search_err[256];
static char g_last_query[256];

static void search_start(void) {
    char query[256] = "";
    if (textinput_prompt("搜索会话内容", NULL, 1, 0, query, sizeof(query)) != 1)
        return;
    snprintf(g_last_query, sizeof(g_last_query), "%s", query);
    g_search_loaded = 0;
    g_search_idx = 0;
    g_search_err[0] = '\0';
    harness_search_free(g_search, g_search_n);
    g_search = NULL;
    g_search_n = 0;
    g_screen = SCREEN_SEARCH;
    g_dirty = 1;
    search_load();
}

static void search_load(void) {
    render_search();
    g_dirty = 0;
    char err[256] = {0};
    if (harness_search_sessions(&g_cfg, g_last_query, &g_search, &g_search_n,
                                err, sizeof(err)) != 0)
        snprintf(g_search_err, sizeof(g_search_err), "%s", err[0] ? err : "搜索失败");
    g_search_loaded = 1;
    g_dirty = 1;
}

static void search_pick(int idx) {
    if (idx < 0 || (size_t)idx >= g_search_n) return;
    char err[256] = {0};
    if (harness_use_session(&g_cfg, g_search[idx].session_id,
                            err, sizeof(err)) != 0) {
        snprintf(g_search_err, sizeof(g_search_err), "%s", err[0] ? err : "打开失败");
        g_dirty = 1;
        return;
    }
    snprintf(g_sess_title, sizeof(g_sess_title), "搜索结果");
    clear_msgs();
    chat_message_t *msgs = NULL;
    size_t n = 0;
    long long first = -1;
    if (harness_fetch_history_ex(&g_cfg, 0, &msgs, &n, &first,
                                 err, sizeof(err)) == 0) {
        for (size_t i = 0; i < n; i++)
            add_msg(msgs[i].role, msgs[i].content ? msgs[i].content : "", 1);
        for (size_t i = 0; i < n; i++) free(msgs[i].content);
        free(msgs);
        g_first_seq = first;
    } else {
        add_msg(ROLE_ASSISTANT, "历史加载失败,从新消息开始。", 1);
    }
    if (g_nmsgs == 0)
        add_msg(ROLE_ASSISTANT, "(该会话暂无聊天记录)\n按 A 开始输入。", 1);
    g_screen = SCREEN_CHAT;
    g_dirty = 1;
}

static void search_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    if (kDown & HidNpadButton_B) {
        enter_workspaces(g_sess_from_startup);
        return;
    }
    if (!g_search_loaded) return;
    int max_idx = (int)g_search_n - 1;
    int tidx = tap_row_index(HEADER_H + 16, 64, 8);
    if (tidx >= 0 && tidx <= max_idx) {
        g_search_idx = tidx;
        kDown |= HidNpadButton_A;
    }
    if (kDown & HidNpadButton_Down) {
        if (g_search_idx < max_idx) g_search_idx++;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        if (g_search_idx > 0) g_search_idx--;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_A) search_pick(g_search_idx);
}

static void render_search(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    char hdr_title[300];
    snprintf(hdr_title, sizeof(hdr_title), "搜索结果:%s", g_last_query);
    draw_trunc(g_font_title, hdr_title, COL_TEXT, 24,
               (HEADER_H - TTF_FontHeight(g_font_title)) / 2, WIN_W - 48 - 60);

    const int row_h = 64;
    int y = HEADER_H + 16;

    if (!g_search_loaded && !g_search_err[0]) {
        draw_trunc(g_font, "正在搜索…", COL_TEXT2, 60, y + 30, WIN_W - 120);
        return;
    }
    if (g_search_err[0]) {
        draw_trunc(g_font, g_search_err, COL_RED, 60, y + 30, WIN_W - 120);
        return;
    }
    if (g_search_n == 0) {
        draw_trunc(g_font, "没有匹配的会话", COL_TEXT2, 60, y + 30, WIN_W - 120);
        return;
    }

    for (size_t i = 0; i < g_search_n; i++) {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int sel = (int)i == g_search_idx;
        roundedBoxRGBA(g_ren, (Sint16)row.x, (Sint16)row.y, (Sint16)(row.x + row.w),
                       (Sint16)(row.y + row.h), 10,
                       sel ? COL_SURF2.r : COL_SURF.r,
                       sel ? COL_SURF2.g : COL_SURF.g,
                       sel ? COL_SURF2.b : COL_SURF.b, 255);
        char text[512];
        snprintf(text, sizeof(text), "%s", g_search[i].snippet);
        draw_trunc(g_font, text, COL_TEXT,
                   row.x + 20, row.y + (row_h - TTF_FontHeight(g_font)) / 2,
                   WIN_W - 48 - 60);
        y += row_h + 8;
        if (y > WIN_H - 100) break;
    }

    const char *hint = "方向键 选择    A 打开    B 返回工作区    + 退出";
    draw_trunc(g_font_hint, hint, COL_TEXT3, 24, WIN_H - 48, WIN_W - 60);
}

/* ---------- 历史翻页 ---------- */

static void prepend_msgs(chat_message_t *msgs, size_t n) {
    size_t keep = n;
    if (g_nmsgs + keep > MAX_MSGS) keep = MAX_MSGS - g_nmsgs;
    if (keep == 0) {
        for (size_t i = 0; i < n; i++) free(msgs[i].content);
        return;
    }
    for (size_t i = g_nmsgs; i-- > 0;) {
        g_msgs[i + keep] = g_msgs[i];
    }
    g_nmsgs += keep;
    size_t skip = n - keep;
    for (size_t i = 0; i < keep; i++) {
        msg_t *m = &g_msgs[i];
        m->text = strdup(msgs[skip + i].content ? msgs[skip + i].content : "");
        if (m->text) utf8_sanitize(m->text);
        m->think = NULL;
        m->tools = NULL;
        m->notice = NULL;
        m->role = msgs[skip + i].role;
        m->done = 1;
        m->tex = NULL;
        m->tex_w = 0;
        m->tex_h = 0;
        m->dirty = 1;
    }
    for (size_t i = 0; i < n; i++) free(msgs[i].content);
}

static void chat_load_older(void) {
    if (strcmp(g_cfg.backend, "harness") != 0) return;
    chat_message_t *msgs = NULL;
    size_t n = 0;
    long long first = -1;
    char err[256] = {0};
    if (harness_fetch_history_ex(&g_cfg, g_first_seq, &msgs, &n, &first,
                                 err, sizeof(err)) == 0 && n > 0) {
        prepend_msgs(msgs, n);
        g_first_seq = first;
        g_scroll_offset = 1000000; /* 渲染时 clamp 到顶部 */
        free(msgs);
    }
    g_dirty = 1;
}

/* ---------- 生命周期 ---------- */

int app_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("app: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    g_win = SDL_CreateWindow("DSH Switch Client", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, WIN_W, WIN_H, 0);
    if (!g_win) {
        printf("app: window failed: %s\n", SDL_GetError());
        return -1;
    }
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) {
        printf("app: renderer failed: %s\n", SDL_GetError());
        return -1;
    }
    if (TTF_Init() != 0) {
        printf("app: TTF_Init failed: %s\n", TTF_GetError());
        return -1;
    }
    IMG_Init(IMG_INIT_PNG);

    g_font       = open_font(30);
    g_font_title = open_font(36);
    g_font_hint  = open_font(22);
    if (!g_font || !g_font_title || !g_font_hint) {
        printf("app: font load failed: %s\n", TTF_GetError());
        return -1;
    }

    /* LOGO:romfs 优先,否则嵌入内存 */
    {
        SDL_Surface *logo_surf = IMG_Load("romfs:/logo.png");
        if (!logo_surf && _binary_logo_png_start != NULL) {
            size_t size = (size_t)(_binary_logo_png_end - _binary_logo_png_start);
            if (size > 0 && size < 0x7FFFFFFF) {
                SDL_RWops *rw = SDL_RWFromConstMem(_binary_logo_png_start, (int)size);
                if (rw) logo_surf = IMG_Load_RW(rw, 1);
            }
        }
        if (logo_surf) {
            g_logo_tex = SDL_CreateTextureFromSurface(g_ren, logo_surf);
            SDL_FreeSurface(logo_surf);
        }
        if (!g_logo_tex) printf("app: logo load skipped\n");
    }

    config_load(&g_cfg);
    printf("app: backend=%s model=%s base=%s\n", g_cfg.backend, g_cfg.model,
           g_cfg.harness_base_url);
    g_choice_idx = (strcmp(g_cfg.backend, "deepseek") == 0) ? 1 : 0;
    snprintf(g_cur_model, sizeof(g_cur_model), "%s", g_cfg.model ? g_cfg.model : "");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();

    g_q_mtx = SDL_CreateMutex();
    if (!g_q_mtx) return -1;

    /* 本地离线 TTS(espeak-ng)。失败(无音频/缺数据)时静默禁用,不影响聊天。 */
    if (tts_init() != 0)
        printf("app: TTS 不可用(语音朗读禁用)\n");
    else
        tts_set_params(g_cfg.tts_rate, g_cfg.tts_volume, g_cfg.tts_pitch);

    /* 语音输入 STT(audin 麦克风)。失败时静默禁用。 */
    if (stt_init() != 0)
        printf("app: STT 不可用(语音输入禁用,请检查 3.5mm 头戴麦克风)\n");

    /* 恢复两套后端的本地聊天记录 */
    history_load(HIST_HARNESS, g_buf_h, &g_bufn_h);
    history_load(HIST_DEEPSEEK, g_buf_d, &g_bufn_d);

    add_msg(ROLE_ASSISTANT,
            "欢迎使用 DSH Switch 客户端。\n"
            "A 输入消息;X 切换侧栏焦点;Y 设置;L 模型;R 切后端;+ 退出。\n"
            "历史保存在 sdmc:/switch/switch-dsh-client/", 1);
    return 0;
}

int app_frame(void) {
    poll_events();
    padUpdate(&g_pad);
    u64 kDown = padGetButtonsDown(&g_pad);

    if (kDown & HidNpadButton_Plus) g_want_exit = 1;
    if (g_want_exit) return -1;

    if (g_screen == SCREEN_CHOICE) {
        choice_input(kDown);
    } else if (g_screen == SCREEN_SETTINGS) {
        settings_input(kDown);
    } else if (g_screen == SCREEN_SESSIONS) {
        if (!g_sess_loaded && !g_sess_err[0]) sessions_load();
        sessions_input(kDown);
    } else if (g_screen == SCREEN_WORKSPACES) {
        if (!g_ws_loaded && !g_ws_err[0]) ws_load();
        ws_input(kDown);
    } else if (g_screen == SCREEN_WS_SESSIONS) {
        if (!g_wss_loaded && !g_wss_err[0]) ws_sessions_load();
        ws_sessions_input(kDown);
    } else if (g_screen == SCREEN_MODELS) {
        if (!g_models_loaded && !g_models_err[0]) models_load();
        models_input(kDown);
    } else if (g_screen == SCREEN_SEARCH) {
        if (!g_search_loaded && !g_search_err[0]) search_load();
        search_input(kDown);
    } else {
        /* 侧栏数据懒加载(后台线程)+ 自动恢复上次会话 */
        if (g_prev_screen != SCREEN_CHAT) g_sb_loaded = 0;
        if (!g_sb_loaded) sb_load();
        sb_apply_ready();
        if (!g_chat_resumed && strcmp(g_cfg.backend, "harness") == 0 &&
            !g_worker_busy) {
            g_chat_resumed = 1;
            chat_resume_last();
        }

        /* 触摸:侧栏点选;对话区点底栏 = 输入消息 */
        if (g_tap_x >= 0) {
            if (g_tap_x < SB_W) {
                sb_tap(g_tap_y);
            } else if (g_tap_y >= WIN_H - FOOTER_H) {
                kDown |= HidNpadButton_A;
            }
            g_tap_x = -1;
            g_tap_y = -1;
        }

        /* 侧栏焦点导航(公共按键不拦截) */
        if (g_focus) {
            if (kDown & HidNpadButton_Up) sb_nav(-1);
            if (kDown & HidNpadButton_Down) sb_nav(1);
            if (kDown & HidNpadButton_A) sb_activate(g_sb_idx);
            if (kDown & HidNpadButton_B) {
                g_focus = 0;
                g_dirty = 1;
            }
        }

        if (!g_focus && (kDown & HidNpadButton_A) && !g_worker_busy) {
            if (textinput_prompt("输入消息", NULL, 1, 0, g_input, sizeof(g_input)) == 1) {
                add_msg(ROLE_USER, g_input, 1);
                add_msg(ROLE_ASSISTANT, "", 0); /* 流式占位 */
                start_worker();
            }
        }
        if ((kDown & HidNpadButton_X) && !g_worker_busy) {
            g_focus = !g_focus;
            g_dirty = 1;
        }
        if ((kDown & HidNpadButton_L) && !g_worker_busy) {
            enter_models();
        }
        /* R:一键切换后端(独立会话 + 本地持久化) */
        if ((kDown & HidNpadButton_R) && !g_worker_busy) {
            save_current_history();
            if (strcmp(g_cfg.backend, "deepseek") == 0)
                store_active(g_buf_d, &g_bufn_d, &g_bufscroll_d, &g_buffirst_d);
            else
                store_active(g_buf_h, &g_bufn_h, &g_bufscroll_h, &g_buffirst_h);

            const char *nb = strcmp(g_cfg.backend, "deepseek") == 0
                                 ? "harness" : "deepseek";
            free(g_cfg.backend);
            g_cfg.backend = strdup(nb);
            config_save(&g_cfg);
            snprintf(g_cur_model, sizeof(g_cur_model), "%s",
                     g_cfg.model ? g_cfg.model : "");
            printf("toggle backend -> %s\n", g_cfg.backend);

            if (strcmp(nb, "harness") == 0) {
                restore_active(g_buf_h, &g_bufn_h, g_bufscroll_h, g_buffirst_h);
                snprintf(g_sess_title, sizeof(g_sess_title), "上次会话");
                g_chat_resumed = 0;
            } else {
                restore_active(g_buf_d, &g_bufn_d, g_bufscroll_d, g_buffirst_d);
                snprintf(g_sess_title, sizeof(g_sess_title), "DeepSeek 对话");
                g_chat_resumed = 1;
            }
            g_focus = 0;
            g_sb_idx = 0;
            g_sb_loaded = 0;
            g_dirty = 1;
        }
        /* B:停止生成 */
        if ((kDown & HidNpadButton_B) && g_worker_busy) {
            net_sse_cancel();
            if (strcmp(g_cfg.backend, "harness") == 0) {
                char cerr[256];
                if (harness_cancel(&g_cfg, cerr, sizeof(cerr)) != 0)
                    printf("cancel: %s\n", cerr);
            }
            printf("cancel requested\n");
        }
        /* ZL:加载更早历史 / 右摇杆按下:回到最新 */
        if ((kDown & HidNpadButton_ZL) && !g_worker_busy) chat_load_older();
        if ((kDown & HidNpadButton_StickR) && !g_worker_busy) {
            g_scroll_offset = 0;
            g_dirty = 1;
        }
        /* ZR 按住:语音输入(录音),松开转写并发送(仅 Harness + 已配 STT) */
        if (!g_focus && strcmp(g_cfg.backend, "harness") == 0 &&
            stt_available() && !g_worker_busy && !g_stt_busy) {
            u64 heldstt = padGetButtons(&g_pad);
            if (heldstt & HidNpadButton_ZR) {
                if (!stt_recording()) stt_begin();
            } else if (stt_recording()) {
                stt_end();
                stt_transcribe_async();
            }
        }
        /* - :朗读最后一条助手消息(语音开启 + TTS 可用,不限后端) */
        if ((kDown & HidNpadButton_Minus) && !g_worker_busy) {
            if (g_voice_enabled && tts_available()) {
                for (int i = g_nmsgs - 1; i >= 0; i--) {
                    if (g_msgs[i].role == ROLE_ASSISTANT &&
                        g_msgs[i].text && g_msgs[i].text[0]) {
                        tts_speak(g_msgs[i].text);
                        break;
                    }
                }
            }
        }
        /* 方向键 / 右摇杆滚动历史(侧栏焦点时不滚) */
        if (!g_focus) {
            u64 held = padGetButtons(&g_pad);
            int step = 0;
            if (held & HidNpadButton_Up) step += 14;
            if (held & HidNpadButton_Down) step -= 14;
            HidAnalogStickState stickr = padGetStickPos(&g_pad, 1); /* 右摇杆 */
            if (stickr.y > 8000 || stickr.y < -8000) step += stickr.y / 1200;
            if (step != 0) {
                g_scroll_offset += step;
                g_dirty = 1;
            }
        }
        if ((kDown & HidNpadButton_Y) && !g_worker_busy) {
            g_set_idx = 0;
            g_screen = SCREEN_SETTINGS;
            g_dirty = 1;
        }
    }

    drain_queue();
    tts_poll(); /* 每帧:取合成结果、向音频设备喂数据 */
    stt_poll(); /* 每帧:录音期间收集已释放的麦克风缓冲 */

    if (g_dirty) {
        if (g_screen == SCREEN_CHOICE) render_choice();
        else if (g_screen == SCREEN_SETTINGS) render_settings();
        else if (g_screen == SCREEN_SESSIONS) render_sessions();
        else if (g_screen == SCREEN_WORKSPACES) render_workspaces();
        else if (g_screen == SCREEN_WS_SESSIONS) render_ws_sessions();
        else if (g_screen == SCREEN_MODELS) render_models();
        else if (g_screen == SCREEN_SEARCH) render_search();
        else render_chat();
        g_dirty = 0;
    }
    SDL_RenderPresent(g_ren);
    g_prev_screen = g_screen;
    return 0;
}

void app_exit(void) {
    for (int i = 0; i < g_nmsgs; i++) {
        msg_tex_destroy(&g_msgs[i]);
        free(g_msgs[i].text);
        free(g_msgs[i].think);
        free(g_msgs[i].tools);
        free(g_msgs[i].notice);
    }
    g_nmsgs = 0;
    if (g_hdr_tex) SDL_DestroyTexture(g_hdr_tex);
    if (g_ftr_tex) SDL_DestroyTexture(g_ftr_tex);
    g_hdr_tex = NULL;
    g_ftr_tex = NULL;
    harness_sessions_free(g_sessions, g_sessions_n);
    g_sessions = NULL;
    g_sessions_n = 0;
    harness_workspaces_free(g_wss, g_wss_n);
    g_wss = NULL;
    g_wss_n = 0;
    harness_sessions_free(g_wss_sessions, g_wss_sessions_n);
    g_wss_sessions = NULL;
    g_wss_sessions_n = 0;
    backend_models_free(g_models, g_models_n);
    g_models = NULL;
    g_models_n = 0;
    harness_search_free(g_search, g_search_n);
    g_search = NULL;
    g_search_n = 0;
    if (g_sb_tex) SDL_DestroyTexture(g_sb_tex);
    g_sb_tex = NULL;
    save_current_history();
    buf_clear(g_buf_h, &g_bufn_h);
    buf_clear(g_buf_d, &g_bufn_d);
    app_event_t ev;
    while (pop_event(&ev)) free(ev.text);
    if (g_q_mtx) SDL_DestroyMutex(g_q_mtx);
    if (g_logo_tex) SDL_DestroyTexture(g_logo_tex);
    g_logo_tex = NULL;
    if (g_font) TTF_CloseFont(g_font);
    if (g_font_title) TTF_CloseFont(g_font_title);
    if (g_font_hint) TTF_CloseFont(g_font_hint);
    TTF_Quit();
    tts_quit();
    stt_quit();
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
    config_free(&g_cfg);
}
