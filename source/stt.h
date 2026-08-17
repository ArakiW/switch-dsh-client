/*
 * 可选的语音转写接口(预留,当前未实现)。
 *
 * 可将采集到的 PCM 音频发送到外部语音转写 HTTP 服务,
 * 返回的转写文本经 textinput 注入聊天输入框。
 */
#ifndef SWITCH_DSH_STT_H
#define SWITCH_DSH_STT_H

#include <stddef.h>

/* 转写服务配置 */
typedef struct {
    char *whisper_url; /* 如 http://192.168.1.10:9000 */
} stt_config_t;

/*
 * 发送 PCM 音频到转写服务,返回文本。
 * pcm_path 为音频文件路径(或 NULL 表示内存缓冲,由实现定义);
 * out 为转写文本(UTF-8)。返回 0 成功,-1 失败(错误信息写入 err)。
 * 未实现:恒返回 -1 并提示语音功能暂不可用。
 */
int stt_transcribe(const stt_config_t *cfg, const char *pcm_path,
                   char *out, size_t outsz, char *err, size_t errsz);

#endif /* SWITCH_DSH_STT_H */
