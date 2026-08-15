#ifndef SWITCH_DSH_APP_H
#define SWITCH_DSH_APP_H

/* 应用骨架:初始化 / 每帧(输入+绘制)/ 清理 */

int app_init(void);   /* 0 成功 */
int app_frame(void);  /* 0 继续运行,-1 退出 */
void app_exit(void);

#endif /* SWITCH_DSH_APP_H */
