#ifndef SWITCH_DSH_STT_H
#define SWITCH_DSH_STT_H

#include <stddef.h>

/*
 * 语音输入(STT):libnx audin 采集麦克风 PCM,打成 WAV 后 POST 到外部
 * 转写服务(如 whisper.cpp server / faster-whisper),返回文本经事件队列
 * 注入聊天输入框。仅在 Harness 后端由上层启用。
 */

int  stt_init(void);              /* 0 成功,-1 不可用(无麦克风/初始化失败) */
int  stt_available(void);         /* 初始化是否成功 */

int  stt_begin(void);             /* 开始录音(按住 ZR) */
int  stt_recording(void);         /* 是否正在录音 */
void stt_poll(void);              /* 每帧调用:收集已释放的音频缓冲 */
void stt_end(void);               /* 结束录音(松开 ZR) */
void stt_cancel(void);            /* 丢弃本次录音 */

/* 把本次录音转写为文本(阻塞,建议在后台线程调用)。out 为 UTF-8 文本。 */
int  stt_transcribe(const char *stt_url, char *out, size_t outsz,
                    char *err, size_t errsz);

void stt_quit(void);              /* 释放资源 */

#endif /* SWITCH_DSH_STT_H */
