/*
 * 二期:本地 Whisper 语音输入接口(预留,当前未实现)。
 *
 * 规划链路(见 README「二期:AI 语音输入」):
 *   USB 耳麦(audin,libnx 提供 audin 服务绑定)
 *     -> 采集 PCM -> 上传本机 Whisper HTTP 服务(whisper.cpp server 或
 *        faster-whisper,监听局域网端口)
 *     -> 返回转写文本 -> 注入聊天输入框(复用 textinput 的 initial text,
 *        或直接作为消息发送)
 *
 * Switch 本体无内置麦克风;audin 可采集 USB 音频类(UAC)耳麦。
 * 建议 Whisper 服务与 dsh-bridge 部署在同一台 PC 上。
 */
#ifndef SWITCH_DSH_STT_H
#define SWITCH_DSH_STT_H

#include <stddef.h>

/* 转写服务配置(二期加入 config.json) */
typedef struct {
    char *whisper_url; /* 如 http://192.168.1.10:9000 */
} stt_config_t;

/*
 * 发送 PCM 音频到转写服务,返回文本。
 * pcm_path 为音频文件路径(或 NULL 表示内存缓冲,由实现定义);
 * out 为转写文本(UTF-8)。返回 0 成功,-1 失败(错误信息写入 err)。
 * 未实现:恒返回 -1 并提示 "语音功能将在二期上线"。
 */
int stt_transcribe(const stt_config_t *cfg, const char *pcm_path,
                   char *out, size_t outsz, char *err, size_t errsz);

#endif /* SWITCH_DSH_STT_H */
