/*
 * 本地离线 TTS:espeak-ng(合成)+ SDL2 音频(输出)。
 *
 * 流程:tts_speak() 起一个后台线程用 espeak-ng 把整段文本合成成 S16 单声道 PCM
 * (RETRIEVAL 模式,经 SynthCallback 收集),合成完毕把缓冲交给主线程;
 * tts_poll() 每帧取走缓冲,打开 SDL 音频设备并用 SDL_NewAudioStream 重采样/
 * 声道转换后逐块 SDL_QueueAudio 播放。全程不阻塞 UI。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "tts.h"
#include "espeak-ng/speak_lib.h"

#define TTS_CHUNK_SAMPLES  4096   /* 每帧向流写入的样本数 */
#define TTS_QUEUE_MAX     262144  /* 设备队列水位上限(字节) */

static int g_tts_ok   = 0;
static int g_tts_rate = 22050;

static SDL_mutex *g_tts_mtx = NULL;

/* 合成线程 -> 主线程 交接 */
static short *g_pending_pcm   = NULL;
static size_t g_pending_n     = 0;
static int    g_pending_rate  = 0;
static int    g_pending_ready = 0;
static volatile int g_tts_synth_busy = 0; /* 是否还有在途合成 */
static volatile int g_tts_gen = 0;         /* 代际,失效在途合成 */

/* 合成线程内部积累缓冲(同一时刻只有一个合成线程在用) */
static short *g_synth_pcm = NULL;
static size_t g_synth_len = 0;
static size_t g_synth_cap = 0;

/* 播放状态(仅主线程访问) */
static SDL_AudioDeviceID g_dev = 0;
static SDL_AudioStream  *g_stream = NULL;
static short *g_play_pcm = NULL;
static size_t g_play_n   = 0;
static size_t g_play_pos = 0;

/* ---------- espeak 合成回调 ---------- */

static int tts_cb(short *wav, int numsamples, espeak_EVENT *events) {
    (void)events;
    if (!wav || numsamples <= 0) return 0;
    if (g_synth_len + (size_t)numsamples > g_synth_cap) {
        size_t nc = g_synth_cap ? g_synth_cap : 16384;
        while (nc < g_synth_len + (size_t)numsamples) nc *= 2;
        short *np = (short *)realloc(g_synth_pcm, nc * sizeof(short));
        if (!np) return 1; /* 中止 */
        g_synth_pcm = np;
        g_synth_cap = nc;
    }
    memcpy(g_synth_pcm + g_synth_len, wav, (size_t)numsamples * sizeof(short));
    g_synth_len += (size_t)numsamples;
    return 0;
}

/* ---------- 合成线程 ---------- */

static int tts_thread(void *arg) {
    char *text = (char *)arg;
    int gen = g_tts_gen;

    g_synth_len = 0;
    espeak_Synth(text, strlen(text) + 1, 0, POS_CHARACTER, 0,
                 espeakCHARS_UTF8, NULL, NULL);
    espeak_Synchronize();
    free(text);

    SDL_LockMutex(g_tts_mtx);
    if (gen == g_tts_gen) {
        if (g_pending_ready) { /* 上一份未取走(理论不会发生),释放 */
            free(g_pending_pcm);
            g_pending_pcm = NULL;
            g_pending_n = 0;
        }
        g_pending_pcm   = g_synth_pcm;
        g_pending_n     = g_synth_len;
        g_pending_rate  = g_tts_rate;
        g_pending_ready = 1;
        g_synth_pcm = NULL;
        g_synth_len = 0;
        g_synth_cap = 0;
    } else {
        free(g_synth_pcm);
        g_synth_pcm = NULL;
        g_synth_len = 0;
        g_synth_cap = 0;
    }
    SDL_UnlockMutex(g_tts_mtx);

    g_tts_synth_busy = 0;
    return 0;
}

/* ---------- 播放 ---------- */

static void tts_stop_playback(void) {
    if (g_stream) { SDL_FreeAudioStream(g_stream); g_stream = NULL; }
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
    free(g_play_pcm);
    g_play_pcm = NULL;
    g_play_n = 0;
    g_play_pos = 0;
}

static void tts_begin_playback(short *pcm, size_t n, int rate) {
    tts_stop_playback();

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = rate;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 2048;
    want.callback = NULL;

    SDL_AudioSpec got;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!g_dev) {
        printf("tts: open audio device failed: %s\n", SDL_GetError());
        free(pcm);
        return;
    }
    g_stream = SDL_NewAudioStream(AUDIO_S16SYS, 1, rate,
                                  got.format, got.channels, got.freq);
    if (!g_stream) {
        printf("tts: audio stream failed: %s\n", SDL_GetError());
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
        free(pcm);
        return;
    }
    g_play_pcm = pcm;
    g_play_n   = n;
    g_play_pos = 0;
    SDL_PauseAudioDevice(g_dev, 0);
}

void tts_poll(void) {
    /* 1) 取走合成结果并开始播放 */
    if (g_pending_ready) {
        SDL_LockMutex(g_tts_mtx);
        short *pcm  = g_pending_pcm;
        size_t n    = g_pending_n;
        int    rate = g_pending_rate;
        g_pending_pcm = NULL;
        g_pending_n = 0;
        g_pending_ready = 0;
        SDL_UnlockMutex(g_tts_mtx);
        if (pcm && n) tts_begin_playback(pcm, n, rate);
        else if (pcm) free(pcm);
    }

    if (!g_dev || !g_stream || !g_play_pcm) return;

    /* 2) 向转换流喂 PCM */
    if (g_play_pos < g_play_n) {
        size_t remain = g_play_n - g_play_pos;
        size_t chunk  = remain < TTS_CHUNK_SAMPLES ? remain : TTS_CHUNK_SAMPLES;
        if (SDL_AudioStreamPut(g_stream, g_play_pcm + g_play_pos,
                               (int)(chunk * sizeof(short))) == 0)
            g_play_pos += chunk;
    }

    /* 3) 从转换流取出并排队到设备(控制设备队列水位,避免过量排队) */
    if (SDL_GetQueuedAudioSize(g_dev) < TTS_QUEUE_MAX) {
        char tmp[16384];
        for (;;) {
            int avail = SDL_AudioStreamAvailable(g_stream);
            if (avail <= 0) break;
            int n = avail > (int)sizeof(tmp) ? (int)sizeof(tmp) : avail;
            if (SDL_AudioStreamGet(g_stream, tmp, n) != n) break;
            SDL_QueueAudio(g_dev, tmp, (Uint32)n);
            if (SDL_GetQueuedAudioSize(g_dev) >= TTS_QUEUE_MAX) break;
        }
    }

    /* 4) 全部播完:收尾 */
    if (g_play_pos >= g_play_n &&
        SDL_AudioStreamAvailable(g_stream) == 0 &&
        SDL_GetQueuedAudioSize(g_dev) == 0) {
        tts_stop_playback();
    }
}

void tts_stop(void) {
    g_tts_gen++; /* 失效在途合成结果 */
    SDL_LockMutex(g_tts_mtx);
    if (g_pending_ready) {
        free(g_pending_pcm);
        g_pending_pcm = NULL;
        g_pending_n = 0;
        g_pending_ready = 0;
    }
    SDL_UnlockMutex(g_tts_mtx);
    tts_stop_playback();
}

int tts_playing(void) {
    return (g_dev != 0) || g_pending_ready || g_tts_synth_busy;
}

int tts_available(void) {
    return g_tts_ok;
}

int tts_speak(const char *utf8) {
    if (!g_tts_ok) return -1;
    if (!utf8 || !utf8[0]) return -1;
    if (g_tts_synth_busy) return -1; /* 在途合成未结束 */

    /* 停掉当前播放 + 清待播 */
    tts_stop_playback();
    SDL_LockMutex(g_tts_mtx);
    if (g_pending_ready) {
        free(g_pending_pcm);
        g_pending_pcm = NULL;
        g_pending_n = 0;
        g_pending_ready = 0;
    }
    SDL_UnlockMutex(g_tts_mtx);

    char *copy = strdup(utf8);
    if (!copy) return -1;
    g_tts_synth_busy = 1;
    SDL_Thread *th = SDL_CreateThread(tts_thread, "tts", copy);
    if (th) {
        SDL_DetachThread(th);
        return 0;
    }
    free(copy);
    g_tts_synth_busy = 0;
    return -1;
}

/* ---------- 生命周期 ---------- */

int tts_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("tts: SDL audio init failed: %s\n", SDL_GetError());
        return -1;
    }
    g_tts_mtx = SDL_CreateMutex();
    if (!g_tts_mtx) return -1;

    int rate = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0,
                                 "romfs:/espeak-ng-data",
                                 espeakINITIALIZE_DONT_EXIT);
    if (rate <= 0) {
        printf("tts: espeak_Initialize failed (rate=%d)\n", rate);
        return -1;
    }
    g_tts_rate = rate;

    espeak_SetSynthCallback(tts_cb);
    if (espeak_SetVoiceByName("cmn") != EE_OK)
        espeak_SetVoiceByName("zh"); /* 兜底 */

    espeak_SetParameter(espeakRATE, 175, 0);
    espeak_SetParameter(espeakVOLUME, 100, 0);
    espeak_SetParameter(espeakPITCH, 50, 0);

    g_tts_ok = 1;
    printf("tts: ready (rate=%d)\n", rate);
    return 0;
}

void tts_quit(void) {
    tts_stop();
    if (g_tts_ok) {
        espeak_Terminate();
        g_tts_ok = 0;
    }
    if (g_tts_mtx) {
        SDL_DestroyMutex(g_tts_mtx);
        g_tts_mtx = NULL;
    }
}
