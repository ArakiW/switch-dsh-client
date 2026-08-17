/*
 * STT:麦克风采集(audin)→ WAV → POST 到外部转写服务 → 文本。
 *
 * 说明:
 *   - 采集:audin 打开音频输入,48kHz/2ch/Int16;录音期间逐块取已释放缓冲,
 *     把立体声下混成单声道存入内存。
 *   - 转写:把单声道 PCM 打成 WAV,POST 到 stt_url(Content-Type: audio/wav),
 *     响应为 JSON,取其中的 "text" 字段。
 *   - 真机依赖:Switch 的音频输入来自 3.5mm TRRS 头戴麦克风;audin 在本机
 *     上的实际设备名/可用性需在实机验证(见 README)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <switch/services/audin.h>

#include "stt.h"
#include "net.h"
#include "cJSON.h"

#define STT_BUF_SIZE   0x8000          /* 单个采集缓冲 32KB */
#define STT_MAX_PCM    (16u * 1024u * 1024u) /* 最大 16MB 单声道 PCM */

static int g_stt_ok = 0;
static u32 g_rate = 48000;
static u32 g_channels = 2;

static AudioInBuffer g_ain_buf;
static void *g_ain_data = NULL;        /* 0x1000 对齐 */
static int g_recording = 0;

static short *g_pcm = NULL;            /* 下混后的单声道 S16 */
static size_t  g_pcm_len = 0;          /* 样本数 */
static size_t  g_pcm_cap = 0;

static void stt_reset_pcm(void) {
    g_pcm_len = 0;
}

static void stt_append_mono(const void *data, size_t bytes) {
    /* 输入为立体声交错 S16;下混为单声道 (L+R)/2 */
    const short *s = (const short *)data;
    size_t samples = bytes / 4; /* 每帧 2 个 s16 */
    if (g_pcm_len + samples > g_pcm_cap) {
        size_t nc = g_pcm_cap ? g_pcm_cap : 4096;
        while (nc < g_pcm_len + samples) nc *= 2;
        if (nc > STT_MAX_PCM / sizeof(short)) return; /* 超出上限,丢弃 */
        short *np = (short *)realloc(g_pcm, nc * sizeof(short));
        if (!np) return;
        g_pcm = np;
        g_pcm_cap = nc;
    }
    for (size_t i = 0; i < samples; i++) {
        int l = s[i * 2];
        int r = s[i * 2 + 1];
        g_pcm[g_pcm_len + i] = (short)((l + r) / 2);
    }
    g_pcm_len += samples;
}

/* ---------- 生命周期 ---------- */

int stt_init(void) {
    Result rc = audinInitialize();
    if (R_FAILED(rc)) {
        printf("stt: audinInitialize: 0x%x\n", rc);
        return -1;
    }

    /* 枚举音频输入设备,取第一个 */
    u32 count = 0;
    audinListAudioIns(NULL, 0, &count);
    char *names = count > 0 ? (char *)calloc(count, 0x100) : NULL;
    if (names) {
        u32 got = 0;
        audinListAudioIns(names, (s32)count, &got);
    }

    char name_out[0x100];
    u32 sr_out = 0, ch_out = 0;
    PcmFormat fmt = PcmFormat_Invalid;
    AudioInState state = AudioInState_Stopped;
    rc = audinOpenAudioIn(names ? names : NULL, name_out,
                          g_rate, g_channels, &sr_out, &ch_out, &fmt, &state);
    free(names);
    if (R_FAILED(rc)) {
        printf("stt: audinOpenAudioIn: 0x%x\n", rc);
        audinExit();
        return -1;
    }
    g_rate = sr_out;
    g_channels = ch_out;
    (void)fmt;

    /* 采集缓冲(0x1000 对齐) */
    g_ain_data = aligned_alloc(0x1000, STT_BUF_SIZE);
    if (!g_ain_data) {
        audinExit();
        return -1;
    }
    memset(&g_ain_buf, 0, sizeof(g_ain_buf));
    g_ain_buf.buffer = g_ain_data;
    g_ain_buf.buffer_size = STT_BUF_SIZE;
    g_ain_buf.data_size = 0;
    g_ain_buf.data_offset = 0;

    g_stt_ok = 1;
    printf("stt: ready (%uHz %uch)\n", g_rate, g_channels);
    return 0;
}

int stt_available(void) {
    return g_stt_ok;
}

/* ---------- 录音 ---------- */

int stt_begin(void) {
    if (!g_stt_ok) return -1;
    stt_reset_pcm();
    audinStartAudioIn();
    audinAppendAudioInBuffer(&g_ain_buf);
    g_recording = 1;
    return 0;
}

int stt_recording(void) {
    return g_recording;
}

void stt_poll(void) {
    if (!g_recording) return;
    AudioInBuffer *rel = NULL;
    u32 rel_count = 0;
    if (R_FAILED(audinGetReleasedAudioInBuffer(&rel, &rel_count)) || !rel)
        return;
    if (rel_count > 0) {
        stt_append_mono(rel->buffer, (size_t)rel->data_size);
        rel->data_size = 0;
        audinAppendAudioInBuffer(rel);
    }
}

void stt_end(void) {
    if (!g_recording) return;
    audinStopAudioIn();
    g_recording = 0;
}

void stt_cancel(void) {
    stt_end();
    stt_reset_pcm();
}

/* ---------- 转写 ---------- */

static void put_le16(u8 *p, u16 v) { p[0] = (u8)(v & 0xff); p[1] = (u8)(v >> 8); }
static void put_le32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xff); p[1] = (u8)((v >> 8) & 0xff);
    p[2] = (u8)((v >> 16) & 0xff); p[3] = (u8)((v >> 24) & 0xff);
}

int stt_transcribe(const char *stt_url, char *out, size_t outsz,
                   char *err, size_t errsz) {
    if (out && outsz) out[0] = '\0';
    if (err && errsz) err[0] = '\0';
    if (!g_stt_ok) {
        if (err && errsz) snprintf(err, errsz, "语音输入不可用");
        return -1;
    }
    if (!stt_url || !stt_url[0]) {
        if (err && errsz) snprintf(err, errsz, "未配置 STT 服务地址");
        return -1;
    }
    if (g_pcm_len == 0) {
        if (err && errsz) snprintf(err, errsz, "没有录到音频(请确认麦克风已连接)");
        return -1;
    }

    /* 构建 WAV(单声道 16-bit PCM) */
    u32 data_len = (u32)(g_pcm_len * 2);
    size_t wav_len = 44 + data_len;
    u8 *wav = (u8 *)malloc(wav_len);
    if (!wav) {
        if (err && errsz) snprintf(err, errsz, "内存不足");
        return -1;
    }
    memcpy(wav, "RIFF", 4);
    put_le32(wav + 4, 36 + data_len);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    put_le32(wav + 16, 16);            /* fmt chunk size */
    put_le16(wav + 20, 1);             /* PCM */
    put_le16(wav + 22, 1);             /* mono */
    put_le32(wav + 24, g_rate);
    put_le32(wav + 28, g_rate * 2);    /* byte rate */
    put_le16(wav + 32, 2);             /* block align */
    put_le16(wav + 34, 16);            /* bits */
    memcpy(wav + 36, "data", 4);
    put_le32(wav + 40, data_len);
    memcpy(wav + 44, g_pcm, data_len);

    /* POST 到转写服务 */
    net_buffer_t resp = {0};
    long code = 0;
    int rc = net_post_audio(stt_url, wav, wav_len, &resp, &code, err, errsz);
    free(wav);
    if (rc != 0) {
        net_buffer_free(&resp);
        return -1;
    }
    if (code != 200) {
        if (err && errsz)
            snprintf(err, errsz, "转写服务返回 HTTP %ld", code);
        net_buffer_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.data);
    net_buffer_free(&resp);
    if (!root) {
        if (err && errsz) snprintf(err, errsz, "转写响应不是合法 JSON");
        return -1;
    }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(t) && t->valuestring[0]) {
        if (out && outsz) snprintf(out, outsz, "%s", t->valuestring);
        cJSON_Delete(root);
        return 0;
    }
    cJSON_Delete(root);
    if (err && errsz) snprintf(err, errsz, "转写响应缺少 text 字段");
    return -1;
}

/* ---------- 清理 ---------- */

void stt_quit(void) {
    stt_end();
    if (g_ain_data) { free(g_ain_data); g_ain_data = NULL; }
    free(g_pcm);
    g_pcm = NULL;
    g_pcm_len = 0;
    g_pcm_cap = 0;
    if (g_stt_ok) { audinExit(); g_stt_ok = 0; }
}
