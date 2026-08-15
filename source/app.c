#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <switch.h>

#include "app.h"
#include "backend.h"
#include "backend_harness.h"
#include "config.h"
#include "textinput.h"
#include "util.h"

#define WIN_W        1280
#define WIN_H        720
#define HEADER_H     64
#define FOOTER_H     44
#define MAX_MSGS     256
#define MAX_LINE_LEN 4096
#define MAX_LINES    128
#define MAX_QUEUE    512

typedef struct {
    char *text; /* malloc, UTF-8 */
    int role;   /* ROLE_USER / ROLE_ASSISTANT */
    int done;   /* 助手消息是否已结束(流式标记) */
} msg_t;

/* 工作线程 -> 主线程 事件队列 */
typedef struct {
    int kind;   /* 1=正文增量 2=完成 3=失败 */
    char *text; /* kind==1 增量文本;kind==3 错误文本;kind==2 NULL */
} app_event_t;

typedef enum {
    SCREEN_CHOICE, SCREEN_CHAT, SCREEN_SETTINGS,
    SCREEN_SESSIONS, SCREEN_WORKSPACES, SCREEN_WS_SESSIONS, SCREEN_MODELS,
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

static void add_msg(int role, const char *text, int done) {
    if (!text) text = "";
    if (g_nmsgs >= MAX_MSGS) {
        free(g_msgs[0].text);
        memmove(&g_msgs[0], &g_msgs[1], sizeof(msg_t) * (MAX_MSGS - 1));
        g_nmsgs = MAX_MSGS - 1;
    }
    msg_t *m = &g_msgs[g_nmsgs++];
    m->text = malloc(strlen(text) + 1);
    if (m->text) strcpy(m->text, text);
    m->role = role;
    m->done = done;
    g_dirty = 1;
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

static void backend_chunk(const char *delta, void *ud) {
    (void)ud;
    push_event(1, delta);
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
                }
            }
            g_dirty = 1;
        } else if (ev.kind == 2) {
            if (g_nmsgs > 0) {
                g_msgs[g_nmsgs - 1].done = 1;
                if (g_msgs[g_nmsgs - 1].text[0] == '\0')
                    strcpy(g_msgs[g_nmsgs - 1].text, "(无回复内容)");
            }
            g_dirty = 1;
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
            }
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

/* ---------- 渲染 ---------- */

static void render_chat(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    /* 顶栏(sidebar-fill + 底部发丝线) */
    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect hair = { 0, HEADER_H - 1, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &hair);

    char title[128];
    snprintf(title, sizeof(title), "DSH Switch 客户端");
    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, title, COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
    snprintf(title, sizeof(title), "%s · %s",
             strcmp(g_cfg.backend, "deepseek") == 0 ? "DeepSeek" : "Harness",
             g_cur_model[0] ? g_cur_model : (g_cfg.model ? g_cfg.model : ""));
    ts = TTF_RenderUTF8_Blended(g_font_hint, title, COL_ACCENT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { WIN_W - ts->w - 24, (HEADER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    /* 消息区 */
    const int area_y0 = HEADER_H + 10;
    const int area_y1 = WIN_H - FOOTER_H - 8;
    const int mx = 40;
    const int maxw = WIN_W - mx * 2 - 40; /* 文本最大宽度 */
    const int lineh = TTF_FontHeight(g_font) + 6;
    const int pad_x = 14;
    const int pad_y = 10;

    /* 先测总高,自动贴底 */
    int total_h = 0;
    for (int i = 0; i < g_nmsgs; i++) {
        int nl = wrap_text(g_font, maxw, g_msgs[i].text, NULL, MAX_LINES);
        total_h += nl * lineh + pad_y * 2 + 12;
    }
    int y = area_y0;
    if (total_h > (area_y1 - area_y0)) y -= (total_h - (area_y1 - area_y0));

    for (int i = 0; i < g_nmsgs; i++) {
        msg_t *m = &g_msgs[i];
        wrap_line_t lines[MAX_LINES];
        int nl = wrap_text(g_font, maxw, m->text, lines, MAX_LINES);
        if (nl <= 0) continue;

        /* 气泡宽 = 最宽行 */
        int bw = 0;
        for (int j = 0; j < nl; j++) {
            char buf[MAX_LINE_LEN];
            size_t len = lines[j].len;
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, m->text + lines[j].off, len);
            buf[len] = '\0';
            int w = 0, h = 0;
            TTF_SizeUTF8(g_font, buf, &w, &h);
            if (w > bw) bw = w;
        }
        int bh = nl * lineh + pad_y * 2;
        int bx = (m->role == ROLE_USER) ? (WIN_W - mx - bw - pad_x * 2) : mx;

        /* Harness 风格圆角气泡:用户=品牌蓝,助手=surface+发丝边框 */
        if (y > -32000 && y < 32000) {
            if (m->role == ROLE_USER) {
                roundedBoxRGBA(g_ren, (Sint16)bx, (Sint16)y,
                               (Sint16)(bx + bw + pad_x * 2), (Sint16)(y + bh), 12,
                               COL_BRAND.r, COL_BRAND.g, COL_BRAND.b, 255);
            } else {
                roundedBoxRGBA(g_ren, (Sint16)bx, (Sint16)y,
                               (Sint16)(bx + bw + pad_x * 2), (Sint16)(y + bh), 12,
                               COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
                roundedRectangleRGBA(g_ren, (Sint16)bx, (Sint16)y,
                                     (Sint16)(bx + bw + pad_x * 2), (Sint16)(y + bh), 12,
                                     COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
            }
        }

        SDL_Color text_col = (m->role == ROLE_USER) ? COL_WHITE : COL_TEXT;
        for (int j = 0; j < nl; j++) {
            draw_line(g_font, text_col, bx + pad_x, y + pad_y + j * lineh,
                      m->text, lines[j].off, lines[j].len);
        }
        y += bh + 12;
    }

    /* 底栏(composer 风格:surface + 顶部发丝线) */
    SDL_SetRenderDrawColor(g_ren, COL_SURF.r, COL_SURF.g, COL_SURF.b, 255);
    SDL_Rect ftr = { 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H };
    SDL_RenderFillRect(g_ren, &ftr);
    SDL_SetRenderDrawColor(g_ren, COL_SURF2.r, COL_SURF2.g, COL_SURF2.b, 255);
    SDL_Rect fhair = { 0, WIN_H - FOOTER_H, WIN_W, 1 };
    SDL_RenderFillRect(g_ren, &fhair);

    const char *hint = g_worker_busy ? "回复中…请稍候    + 退出"
                       : (strcmp(g_cfg.backend, "harness") == 0
                          ? "A 输入  X 工作区  Y 设置  L 模型  R 切后端  + 退出"
                          : "A 输入  X 清屏  Y 设置  L 模型  R 切后端  + 退出");
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_HINT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, WIN_H - FOOTER_H + (FOOTER_H - ts->h) / 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
    if (g_nmsgs > 0 && g_msgs[g_nmsgs - 1].role == ROLE_ASSISTANT &&
        !g_msgs[g_nmsgs - 1].done) {
        ts = TTF_RenderUTF8_Blended(g_font_hint, "● 回复中…", COL_ACCENT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { WIN_W - ts->w - 24, WIN_H - FOOTER_H + (FOOTER_H - ts->h) / 2,
                           ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
    }
}

/* ---------- 输入:手柄 + 触摸屏 ---------- */

static int g_tap_x = -1;
static int g_tap_y = -1;

static void poll_events(void) {
    g_tap_x = -1;
    g_tap_y = -1;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_MOUSEBUTTONDOWN &&
            ev.button.button == SDL_BUTTON_LEFT) {
            g_tap_x = ev.button.x;
            g_tap_y = ev.button.y;
        }
#ifdef SDL_FINGERDOWN
        else if (ev.type == SDL_FINGERDOWN) {
            g_tap_x = (int)(ev.tfinger.x * (float)WIN_W);
            g_tap_y = (int)(ev.tfinger.y * (float)WIN_H);
        }
#endif
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

/* ---------- 设置界面 ---------- */

#define SET_COUNT 7

static const char *set_label(int i) {
    switch (i) {
        case 0: return "后端";
        case 1: return "Harness 地址";
        case 2: return "DeepSeek 地址";
        case 3: return "API Key(key.txt 优先)";
        case 4: return "模型";
        case 5: return "思考模式";
        case 6: return "系统提示词";
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
                snprintf(g_key_menu_msg, sizeof(g_key_menu_msg), "已从 key.txt 加载 Key ✓");
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
        ts = TTF_RenderUTF8_Blended(g_font_hint, g_key_menu_msg, COL_TEXT2);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 8, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
    }

    const char *hint = "↑↓/点击 选择    A 执行    B 返回设置    + 退出";
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
    for (int i = 0; i < SET_COUNT; i++) {
        int y = HEADER_H + 20 + i * row_h;

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

        char value[200];
        set_value(i, value, sizeof(value));
        ts = TTF_RenderUTF8_Blended(g_font_hint, value, COL_ACCENT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 420, y, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
    }

    const char *hint = "↑↓ 选择    A 修改/切换    B 保存并返回    + 退出";
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

/* ---------- 启动后端选择界面 ---------- */

static void choice_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
    }
    /* 触摸:点卡片 = 选中并确认 */
    if (g_tap_x >= 0) {
        for (int i = 0; i < 2; i++) {
            SDL_Rect card = { 140, 200 + i * 190, WIN_W - 280, 160 };
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
        if (g_choice_idx == 0) enter_workspaces(1); /* Harness:先看工作区 */
        else g_screen = SCREEN_CHAT;
        g_dirty = 1;
    }
}

static void render_choice(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    SDL_Surface *ts = TTF_RenderUTF8_Blended(g_font_title, "选择后端", COL_TEXT);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { (WIN_W - ts->w) / 2, 60, ts->w, ts->h };
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
        SDL_Rect card = { 140, 200 + i * 190, WIN_W - 280, 160 };
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
                                    i == g_choice_idx ? "▶" : "  ", COL_BRAND);
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

    const char *hint = "↑↓/←→ 选择    A 确定    + 退出";
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
        free(g_msgs[g_nmsgs].text);
        g_msgs[g_nmsgs].text = NULL;
    }
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
        if (harness_fetch_history(&g_cfg, &msgs, &n, err, sizeof(err)) == 0) {
            for (size_t i = 0; i < n; i++)
                add_msg(msgs[i].role, msgs[i].content ? msgs[i].content : "", 1);
            for (size_t i = 0; i < n; i++) free(msgs[i].content);
            free(msgs);
        } else {
            add_msg(ROLE_ASSISTANT, "历史加载失败,从新消息开始。", 1);
        }
        if (g_nmsgs == 0)
            add_msg(ROLE_ASSISTANT, "(该会话暂无聊天记录)\n按 A 开始输入。", 1);
    } else {
        add_msg(ROLE_ASSISTANT,
                "已新建会话。\nA 输入消息;X 切换会话;Y 设置;+ 退出。", 1);
    }
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
        ts = TTF_RenderUTF8_Blended(g_font, "正在加载会话列表…", COL_TEXT2);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        return;
    }

    if (g_sess_err[0]) {
        ts = TTF_RenderUTF8_Blended(g_font, g_sess_err, COL_RED);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
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

        /* 标题(截断) */
        char title[128];
        snprintf(title, sizeof(title), "%s", g_sessions[i].title);
        ts = TTF_RenderUTF8_Blended(g_font, title, COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + 10, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }

        /* 右侧:状态 + 时间 */
        char meta[64];
        if (g_sessions[i].running) snprintf(meta, sizeof(meta), "● 运行中");
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

    const char *hint = "↑↓ 选择    A 打开    B 返回    + 退出";
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
    if (kDown & HidNpadButton_B) {
        g_screen = g_sess_from_startup ? SCREEN_CHOICE : SCREEN_CHAT;
        g_dirty = 1;
        return;
    }
    if (!g_ws_loaded) return;
    int max_idx = (int)g_wss_n + 2; /* 0=新建会话 1=新建工作区 2..=工作区 +1=全部会话 */
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
                             "C:\\Users\\USER\\Documents\\Codex", 0, 0,
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
    if (g_ws_idx == max_idx) {
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
        ts = TTF_RenderUTF8_Blended(g_font, "正在加载工作区…", COL_TEXT2);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        return;
    }
    if (g_ws_err[0]) {
        ts = TTF_RenderUTF8_Blended(g_font, g_ws_err, COL_RED);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
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
        snprintf(label, sizeof(label), "📁 %s", g_wss[i].title);
        ts = TTF_RenderUTF8_Blended(g_font, label, COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + 10, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }

        char meta[128];
        snprintf(meta, sizeof(meta), "%zu 会话 · %s", g_wss[i].session_count,
                 g_wss[i].path ? g_wss[i].path : "");
        ts = TTF_RenderUTF8_Blended(g_font_hint, meta, COL_TEXT3);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + row_h - ts->h - 8, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
        if (y > WIN_H - 140) break;
    }

    /* 全部会话(平铺) */
    {
        SDL_Rect row = { 24, y, WIN_W - 48, row_h };
        int sel = g_ws_idx == (int)g_wss_n + 2;
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

    const char *hint = "↑↓ 选择    A 打开/新建    B 返回    + 退出";
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
    clear_msgs();
    chat_message_t *msgs = NULL;
    size_t n = 0;
    if (harness_fetch_history(&g_cfg, &msgs, &n, err, sizeof(err)) == 0) {
        for (size_t i = 0; i < n; i++)
            add_msg(msgs[i].role, msgs[i].content ? msgs[i].content : "", 1);
        for (size_t i = 0; i < n; i++) free(msgs[i].content);
        free(msgs);
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
        snprintf(hdr_title, sizeof(hdr_title), "📁 %s", g_wss[g_ws_sel].title);
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
        ts = TTF_RenderUTF8_Blended(g_font, "正在加载会话…", COL_TEXT2);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        return;
    }
    if (g_wss_err[0]) {
        ts = TTF_RenderUTF8_Blended(g_font, g_wss_err, COL_RED);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
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
        ts = TTF_RenderUTF8_Blended(g_font, g_wss_sessions[i].title, COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + 10, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        char meta[64];
        if (g_wss_sessions[i].running) snprintf(meta, sizeof(meta), "● 运行中");
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

    const char *hint = "↑↓ 选择    A 打开/新建    B 返回工作区    + 退出";
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
    int tidx = tap_row_index(HEADER_H + 16, 64, 8);
    if (tidx >= 0 && tidx < (int)g_models_n) {
        g_model_idx = tidx;
        kDown |= HidNpadButton_A;
    }
    if (kDown & HidNpadButton_Down) {
        if (g_model_idx < (int)g_models_n - 1) g_model_idx++;
        g_dirty = 1;
    }
    if (kDown & HidNpadButton_Up) {
        if (g_model_idx > 0) g_model_idx--;
        g_dirty = 1;
    }
    if (!(kDown & HidNpadButton_A)) return;

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
    ts = TTF_RenderUTF8_Blended(g_font_hint, sub, COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 200, (HEADER_H - ts->h) / 2 + 2, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }

    const int row_h = 64;
    int y = HEADER_H + 16;

    if (!g_models_loaded && !g_models_err[0]) {
        ts = TTF_RenderUTF8_Blended(g_font, "正在读取模型列表…", COL_TEXT2);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        return;
    }
    if (g_models_err[0]) {
        ts = TTF_RenderUTF8_Blended(g_font, g_models_err, COL_RED);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { 60, y + 30, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
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
                 strcmp(g_models[i].id, g_cur_model) == 0 ? "  ✓ 当前" : "");
        ts = TTF_RenderUTF8_Blended(g_font, label,
                                    strcmp(g_models[i].id, g_cur_model) == 0
                                        ? COL_BRAND : COL_TEXT);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + 10, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        char meta[160];
        if (g_models[i].provider && g_models[i].provider[0])
            snprintf(meta, sizeof(meta), "%s · %s", g_models[i].provider, g_models[i].id);
        else
            snprintf(meta, sizeof(meta), "%s", g_models[i].id);
        ts = TTF_RenderUTF8_Blended(g_font_hint, meta, COL_TEXT3);
        if (ts) {
            SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
            SDL_Rect d = { row.x + 20, row.y + row_h - ts->h - 8, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
        y += row_h + 8;
        if (y > WIN_H - 100) break;
    }

    const char *hint = "↑↓ 选择    A 应用    B 返回    + 退出";
    ts = TTF_RenderUTF8_Blended(g_font_hint, hint, COL_TEXT3);
    if (ts) {
        SDL_Texture *tt = SDL_CreateTextureFromSurface(g_ren, ts);
        SDL_Rect d = { 24, WIN_H - 48, ts->w, ts->h };
        SDL_RenderCopy(g_ren, tt, NULL, &d);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(ts);
    }
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

    g_font       = open_font(30);
    g_font_title = open_font(36);
    g_font_hint  = open_font(22);
    if (!g_font || !g_font_title || !g_font_hint) {
        printf("app: font load failed: %s\n", TTF_GetError());
        return -1;
    }

    config_load(&g_cfg);
    printf("app: backend=%s model=%s base=%s\n", g_cfg.backend, g_cfg.model,
           g_cfg.harness_base_url);
    g_choice_idx = (strcmp(g_cfg.backend, "deepseek") == 0) ? 1 : 0;
    snprintf(g_cur_model, sizeof(g_cur_model), "%s", g_cfg.model ? g_cfg.model : "");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    g_q_mtx = SDL_CreateMutex();
    if (!g_q_mtx) return -1;

    add_msg(ROLE_ASSISTANT,
            "欢迎使用 DSH Switch 客户端。\n"
            "A 输入消息;X 会话列表/清屏;Y 设置;+ 退出。\n"
            "配置也会保存在 sdmc:/switch/switch-dsh-client/config.json", 1);
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
    } else {
        /* 触摸:点底栏 = 输入消息 */
        if (g_tap_x >= 0) {
            if (g_tap_y >= WIN_H - FOOTER_H) kDown |= HidNpadButton_A;
            g_tap_x = -1;
            g_tap_y = -1;
        }
        if ((kDown & HidNpadButton_A) && !g_worker_busy) {
            if (textinput_prompt("输入消息", NULL, 1, 0, g_input, sizeof(g_input)) == 1) {
                add_msg(ROLE_USER, g_input, 1);
                add_msg(ROLE_ASSISTANT, "", 0); /* 流式占位 */
                start_worker();
            }
        }
        if ((kDown & HidNpadButton_X) && !g_worker_busy) {
            if (strcmp(g_cfg.backend, "harness") == 0) enter_workspaces(0);
            else clear_msgs();
            g_dirty = 1;
        }
        if ((kDown & HidNpadButton_L) && !g_worker_busy) {
            enter_models();
        }
        /* R:一键切换后端(DSH <-> DeepSeek) */
        if ((kDown & HidNpadButton_R) && !g_worker_busy) {
            const char *nb = strcmp(g_cfg.backend, "deepseek") == 0
                                 ? "harness" : "deepseek";
            free(g_cfg.backend);
            g_cfg.backend = strdup(nb);
            config_save(&g_cfg);
            snprintf(g_cur_model, sizeof(g_cur_model), "%s",
                     g_cfg.model ? g_cfg.model : "");
            printf("toggle backend -> %s\n", g_cfg.backend);
            add_msg(ROLE_ASSISTANT,
                    strcmp(nb, "deepseek") == 0
                        ? "已切换后端:DeepSeek(官方 API)。\nX 清屏;L 选模型。"
                        : "已切换后端:Harness(局域网)。\nX 工作区;L 选模型。",
                    1);
        }
        if ((kDown & HidNpadButton_Y) && !g_worker_busy) {
            g_set_idx = 0;
            g_screen = SCREEN_SETTINGS;
            g_dirty = 1;
        }
    }

    drain_queue();

    if (g_dirty) {
        if (g_screen == SCREEN_CHOICE) render_choice();
        else if (g_screen == SCREEN_SETTINGS) render_settings();
        else if (g_screen == SCREEN_SESSIONS) render_sessions();
        else if (g_screen == SCREEN_WORKSPACES) render_workspaces();
        else if (g_screen == SCREEN_WS_SESSIONS) render_ws_sessions();
        else if (g_screen == SCREEN_MODELS) render_models();
        else render_chat();
        g_dirty = 0;
    }
    SDL_RenderPresent(g_ren);
    return 0;
}

void app_exit(void) {
    for (int i = 0; i < g_nmsgs; i++) free(g_msgs[i].text);
    g_nmsgs = 0;
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
    app_event_t ev;
    while (pop_event(&ev)) free(ev.text);
    if (g_q_mtx) SDL_DestroyMutex(g_q_mtx);
    if (g_font) TTF_CloseFont(g_font);
    if (g_font_title) TTF_CloseFont(g_font_title);
    if (g_font_hint) TTF_CloseFont(g_font_hint);
    TTF_Quit();
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
    config_free(&g_cfg);
}
