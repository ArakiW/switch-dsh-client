#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <switch.h>

#include "app.h"
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

typedef struct {
    char *text; /* malloc, UTF-8 */
    int role;   /* ROLE_USER / ROLE_ASSISTANT */
    int done;   /* 助手消息是否已结束(流式标记) */
} msg_t;

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

static const SDL_Color COL_BG     = {  24,  26,  38, 255 };
static const SDL_Color COL_HEADER = {  40,  44,  66, 255 };
static const SDL_Color COL_USER   = {  66, 133, 244, 255 };
static const SDL_Color COL_ASST   = {  58,  62,  84, 255 };
static const SDL_Color COL_TEXT   = { 236, 238, 246, 255 };
static const SDL_Color COL_HINT   = { 150, 156, 176, 255 };
static const SDL_Color COL_ACCENT = { 120, 180, 255, 255 };

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

static void render(void) {
    SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(g_ren);

    /* 顶栏 */
    SDL_SetRenderDrawColor(g_ren, COL_HEADER.r, COL_HEADER.g, COL_HEADER.b, 255);
    SDL_Rect hdr = { 0, 0, WIN_W, HEADER_H };
    SDL_RenderFillRect(g_ren, &hdr);

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

        SDL_SetRenderDrawColor(g_ren,
                               m->role == ROLE_USER ? COL_USER.r : COL_ASST.r,
                               m->role == ROLE_USER ? COL_USER.g : COL_ASST.g,
                               m->role == ROLE_USER ? COL_USER.b : COL_ASST.b,
                               255);
        SDL_Rect bubble = { bx, y, bw + pad_x * 2, bh };
        SDL_RenderFillRect(g_ren, &bubble);

        for (int j = 0; j < nl; j++) {
            draw_line(g_font, COL_TEXT, bx + pad_x, y + pad_y + j * lineh,
                      m->text, lines[j].off, lines[j].len);
        }
        y += bh + 12;
    }

    /* 底栏 */
    SDL_SetRenderDrawColor(g_ren, COL_HEADER.r, COL_HEADER.g, COL_HEADER.b, 255);
    SDL_Rect ftr = { 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H };
    SDL_RenderFillRect(g_ren, &ftr);

    const char *hint = "A 输入消息    X 清屏    + 退出";
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
            SDL_Rect d = { WIN_W - ts->w - 24, WIN_H - FOOTER_H + (FOOTER_H - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(g_ren, tt, NULL, &d);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
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

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    add_msg(ROLE_ASSISTANT,
            "欢迎使用 DSH Switch 客户端。\n按 A 输入消息发送给 AI。(网络后端接入中)", 1);
    return 0;
}

int app_frame(void) {
    padUpdate(&g_pad);
    u64 kDown = padGetButtonsDown(&g_pad);

    if (kDown & HidNpadButton_Plus) g_want_exit = 1;
    if (g_want_exit) return -1;

    if (kDown & HidNpadButton_A) {
        if (textinput_prompt("输入消息", NULL, 1, g_input, sizeof(g_input)) == 1) {
            add_msg(ROLE_USER, g_input, 1);
            /* 占位:后端接入后由 backend 线程流式填充 */
            char reply[512];
            snprintf(reply, sizeof(reply), "(后端接入中)已收到:%s", g_input);
            add_msg(ROLE_ASSISTANT, reply, 1);
        }
    }
    if (kDown & HidNpadButton_X) {
        while (g_nmsgs > 0) {
            g_nmsgs--;
            free(g_msgs[g_nmsgs].text);
            g_msgs[g_nmsgs].text = NULL;
        }
        g_dirty = 1;
    }

    if (g_dirty) {
        render();
        g_dirty = 0;
    }
    SDL_RenderPresent(g_ren);
    return 0;
}

void app_exit(void) {
    for (int i = 0; i < g_nmsgs; i++) free(g_msgs[i].text);
    g_nmsgs = 0;
    if (g_font) TTF_CloseFont(g_font);
    if (g_font_title) TTF_CloseFont(g_font_title);
    if (g_font_hint) TTF_CloseFont(g_font_hint);
    TTF_Quit();
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
    config_free(&g_cfg);
}
