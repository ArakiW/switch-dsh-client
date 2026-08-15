#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <switch.h>

#include "app.h"
#include "backend.h"
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

typedef enum { SCREEN_CHOICE, SCREEN_CHAT, SCREEN_SETTINGS } screen_t;

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
             g_cfg.model ? g_cfg.model : "");
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
                                     : "A 输入消息    X 清屏    Y 设置    + 退出";
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

/* ---------- 设置界面 ---------- */

#define SET_COUNT 7

static const char *set_label(int i) {
    switch (i) {
        case 0: return "后端";
        case 1: return "Harness 地址";
        case 2: return "DeepSeek 地址";
        case 3: return "API Key";
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

static void settings_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
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
            case 3: set_edit_field(&g_cfg.deepseek_api_key, "API Key", 0, 1); break;
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

/* ---------- 启动后端选择界面 ---------- */

static void choice_input(u64 kDown) {
    if (kDown & HidNpadButton_Plus) {
        g_want_exit = 1;
        return;
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
        g_screen = SCREEN_CHAT;
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

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    g_q_mtx = SDL_CreateMutex();
    if (!g_q_mtx) return -1;

    add_msg(ROLE_ASSISTANT,
            "欢迎使用 DSH Switch 客户端。\n"
            "A 输入消息;X 清屏;Y 设置;+ 退出。\n"
            "配置也会保存在 sdmc:/switch/switch-dsh-client/config.json", 1);
    return 0;
}

int app_frame(void) {
    padUpdate(&g_pad);
    u64 kDown = padGetButtonsDown(&g_pad);

    if (kDown & HidNpadButton_Plus) g_want_exit = 1;
    if (g_want_exit) return -1;

    if (g_screen == SCREEN_CHOICE) {
        choice_input(kDown);
    } else if (g_screen == SCREEN_SETTINGS) {
        settings_input(kDown);
    } else {
        if ((kDown & HidNpadButton_A) && !g_worker_busy) {
            if (textinput_prompt("输入消息", NULL, 1, 0, g_input, sizeof(g_input)) == 1) {
                add_msg(ROLE_USER, g_input, 1);
                add_msg(ROLE_ASSISTANT, "", 0); /* 流式占位 */
                start_worker();
            }
        }
        if ((kDown & HidNpadButton_X) && !g_worker_busy) {
            while (g_nmsgs > 0) {
                g_nmsgs--;
                free(g_msgs[g_nmsgs].text);
                g_msgs[g_nmsgs].text = NULL;
            }
            g_dirty = 1;
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
        else render_chat();
        g_dirty = 0;
    }
    SDL_RenderPresent(g_ren);
    return 0;
}

void app_exit(void) {
    for (int i = 0; i < g_nmsgs; i++) free(g_msgs[i].text);
    g_nmsgs = 0;
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
