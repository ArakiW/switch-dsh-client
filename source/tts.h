#ifndef SWITCH_DSH_TTS_H
#define SWITCH_DSH_TTS_H

/*
 * 本地离线语音朗读(TTS):
 *   - 合成引擎:espeak-ng(交叉编译进 .nro,数据在 romfs:/espeak-ng-data)
 *   - 输出:SDL2 音频(SDL_NewAudioStream 做重采样/声道转换)
 * 仅在 Harness 后端由上层调用(tts 本身不感知后端)。
 */

int  tts_init(void);              /* 0 成功,-1 不可用 */
int  tts_available(void);         /* 初始化是否成功 */
int  tts_speak(const char *utf8); /* 异步朗读一段 UTF-8 文本;0 已提交,-1 失败/忙 */
int  tts_playing(void);           /* 是否正在播放(或在途合成) */
void tts_stop(void);              /* 停止播放并丢弃待播/在途结果 */
void tts_poll(void);              /* 每帧调用:取合成结果、向音频设备喂数据 */
void tts_quit(void);              /* 释放资源 */

#endif /* SWITCH_DSH_TTS_H */
