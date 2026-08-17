# switch-dsh-client

> 在 Nintendo Switch 上使用 DeepSeek 的 AI 聊天客户端(homebrew 自制程序)。
> *A DeepSeek AI chat client for the Nintendo Switch (homebrew).*

`switch-dsh-client` 是一个运行在已破解 Switch 上的自制程序,让你用手柄和触摸屏与 DeepSeek 对话。基于 libnx + SDL2 + libcurl 原生实现,支持两个后端一键切换、流式输出、中文拼音输入,界面风格对齐桌面版 DeepSeek Harness 的对话工作区。

## ✨ 特性

- **双后端一键切换** —— ① 局域网 DeepSeek Harness(经 `dsh-bridge` 桥接)② DeepSeek 官方 API 直连;两套对话独立保存、互不干扰
- **流式输出** —— SSE 逐字渲染,60fps 平滑滚动;支持**思考过程**(暗色缩进)、工具活动行、审批/提问提醒、任务清单条
- **桌面同款工作区** —— 左侧会话栏(按工作区分组)、新建/分叉/重命名会话、工作区管理、模型与推理强度切换
- **中文输入** —— 调用系统软键盘 swkbd,原生简体中文拼音输入,免外接键盘
- **触摸 + 手柄双操作** —— 手指拖动滚动/点选、方向键、右摇杆、ZL/ZR 翻页,手感对齐桌面滚动
- **数据本地化** —— 配置、对话历史、API Key 都只保存在你自己的 SD 卡上
- **零配置发布** —— GitHub Actions 自动构建 `.nro`,Release 附带成品,解压即装

## 📸 截图

> 待补充:启动后端选择界面、对话界面(双栏)、设置界面。

## 🚀 快速开始

### 安装到 Switch

1. 下载最新 Release 的 `switch-dsh-client.nro`(或自行构建,见下文「构建」)
2. 放入 SD 卡 `switch/` 目录,在 hbmenu 里直接运行(或用 NRO 前向器做成桌面图标)
3. 首次启动使用内置默认配置;配置自动保存到 `sdmc:/switch/switch-dsh-client/config.json`

### 选择后端

| 后端 | 说明 | 适合场景 |
| --- | --- | --- |
| **Harness** | 连你电脑上运行的 DeepSeek Harness,复用现有会话/模型/工作区 | 已有 Harness 环境,想续用桌面端会话 |
| **DeepSeek 官方 API** | 直连 `api.deepseek.com`,只填 API Key 即可 | 无自建服务,开箱即用 |

### 后端 A:连本机 DeepSeek Harness(推荐)

先在 Harness 所在的电脑上启动桥接(任选其一):

- **一键脚本**:双击 `start-bridge.bat`,窗口会显示本机局域网 IP;
- **Harness 网页按钮(推荐)**:安装本仓库附带的一键启动插件:

  ```bash
  dsh plugin --profile web add github:ArakiW/switch-dsh-client
  # 重启 DSH 后,网页右下角出现可拖动的桥接启停按钮
  ```

- **手动运行**:

  ```bash
  node bridge/dsh-bridge.js [--dsh http://127.0.0.1:3080] [--host 0.0.0.0] [--port 8765]
  ```

然后在 Switch 端:启动选 `Harness`,设置里 Harness 地址填 `http://<电脑局域网IP>:8765`。

> ⚠️ 安全提示:Harness API 无鉴权,桥接**只应在可信局域网**运行,切勿暴露到公网。防火墙拦截时放行 Node 的 8765 端口即可。

### 后端 B:直连 DeepSeek 官方 API

设置界面里:后端切到 `DeepSeek` → 填入 API Key → 选择模型(`deepseek-v4-pro` / `deepseek-v4-flash`)与思考模式(`enabled` 显示思维链,`disabled` 响应更快)。HTTPS 走 Switch 系统 SSL,无需配置证书。

## ⌨️ 操作指南

| 按键 | 启动界面 | 聊天界面(双栏) | 设置界面 |
| --- | --- | --- | --- |
| A | 确定后端 | 输入消息;侧栏焦点时打开会话 | 修改/切换当前项 |
| B | — | 停止生成;侧栏焦点时回对话 | 保存并返回 |
| X | — | 对话 / 侧栏焦点切换 | — |
| Y | — | 打开设置 | — |
| L | — | 切换模型 + 推理强度 | — |
| R | — | 切换后端(两套对话独立) | — |
| ZL | — | 加载更早历史 | — |
| ZR | — | 回到最新 | — |
| ↑/↓/←/→ | 切换后端 | 侧栏移动 / 滚动对话 | 选择设置项 |
| + | 退出 | 退出 | 退出 |

聊天界面触摸操作:手指按住上下划动滚动、轻点点选、点底部栏输入。

## ⚙️ 配置

### config.json 字段

| 字段 | 说明 |
| --- | --- |
| `backend` | `"harness"` 或 `"deepseek"` |
| `harness_base_url` | 指向 dsh-bridge,如 `http://192.168.1.10:8765` |
| `deepseek_base_url` | 默认 `https://api.deepseek.com` |
| `deepseek_api_key` | DeepSeek API Key |
| `model` | 模型名(默认 `deepseek-v4-pro`) |
| `deepseek_thinking` | `"enabled"` / `"disabled"` |
| `system_prompt` | 系统提示词(可空) |

### API Key 免手输

在 Switch 上敲 Key 太麻烦,可在电脑上直接写好文本文件:

1. 在 `sdmc:/switch/switch-dsh-client/` 新建 `deepseek_api_key.txt`,内容就是一行 Key(前后空白自动忽略)
2. 应用每次启动自动读取;该文件非空时优先于 config.json
3. 设置界面里仍可修改,修改后自动同步写回 key.txt

> 🔒 Key 只保存在你自己的 SD 卡上,不会同步到别处。**不要把它提交到 git 或发给别人。**

## 🔨 构建

### 本机 Windows

前置:devkitA64 sysroot + LLVM + switch-tools(依赖目录用 `DSH_SWITCH_DEPS` 环境变量指定,详见 `scripts/build.ps1`)。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
# 产物:switch-dsh-client.nro(约 25MB,内置 Noto Sans CJK 字体)
```

### 标准 devkitPro / CI

```bash
export DEVKITPRO=/opt/devkitpro
make -j$(nproc)
# 字体走 romfs 路径(app.c 已做 romfs→嵌入内存 两级回退)
```

GitHub Actions 见 `.github/workflows/build.yml`,推送即自动出 `.nro`,打 tag 自动发 Release。

## 🛠 技术栈

- **平台**:libnx(自制程序框架)
- **渲染/输入**:SDL2、SDL2_ttf、SDL2_gfx、SDL2_image
- **网络**:libcurl(HTTPS + SSE 流式)
- **字体**:Noto Sans CJK SC(SIL OFL 1.1)
- **JSON**:vendored cJSON
- **桥接**:`bridge/dsh-bridge.js`(零依赖 Node,把 Harness 回环 API + WebSocket 转成局域网可达的 HTTP + SSE)

## 📄 许可

- 项目代码:**MIT** — 见 [`LICENSE`](LICENSE)
- 嵌入字体 Noto Sans CJK SC:**SIL OFL 1.1** — 见 `LICENSE-FONTS.txt`
- vendored cJSON:**MIT** — 见 `libs/cjson/`

---

*项目由 [ArakiW](https://github.com/ArakiW) 维护,欢迎 issue 与 PR。*
