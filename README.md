# switch-dsh-client

Nintendo Switch 自制程序(homebrew):DeepSeek AI 聊天客户端。libnx + SDL2 + libcurl 实现,后端可在「局域网 DeepSeek Harness」与「DeepSeek 官方 API」之间一键切换,流式输出,系统软键盘支持中文拼音输入。

## 一期功能(0.1.0)

- 文字聊天,SSE 流式渲染(逐字显示)
- 双后端切换:① Harness(经 dsh-bridge)② DeepSeek 官方 API
- **启动时后端二选一选择界面**(每次打开 APP 先选后端,选择自动保存)
- 系统软键盘 swkbd,原生简体中文拼音输入(`SwkbdType_ZhHans`)
- 设备端设置界面(后端/地址/API Key/模型/思考模式/系统提示词,保存到 SD 卡)
- Harness 模式会话 ID 持久化(重启续聊)
- 错误展示(网络/协议错误直接显示在气泡里)

## 架构

```
Switch(本应用)
  ├─ backend "harness":HTTP POST /api/<method> + SSE
  │     └─▶ dsh-bridge(0.0.0.0:8765,跑在 DSH 所在 PC)
  │           ├─ POST /api/*  ──转发──▶ DSH(127.0.0.1:3080)
  │           └─ /api/events.sse(WS→SSE 翻译 + 会话过滤)◀── DSH WS /api/events.mux
  └─ backend "deepseek":HTTPS POST /chat/completions(SSE)──直连──▶ api.deepseek.com
```

为什么需要 dsh-bridge:DSH 的 Web 服务只监听 `127.0.0.1`(CLI 明确拒绝 `0.0.0.0`),且下行事件只有 WebSocket;桥接进程既解决局域网可达性,又把 WS 翻译成 SSE,让 Switch 端网络层只需 libcurl 一种代码。协议细节见 `DSH_API_SPEC.md`。

## 构建

### 本机 Windows(已验证)

前置:本机存在 devkitA64 sysroot + LLVM + switch-tools(本机沿用 F1RaceWatch 项目的工具链,路径硬编码在 `scripts/build.ps1` 里,可自行修改)。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
# 产物:switch-dsh-client.nro(约 25MB,含 Noto Sans CJK 字体)
```

注:该 sysroot 的 gcc 是 Linux ELF 二进制,所以走 clang + ld.lld 路线;资源经 llvm-objcopy 嵌入 rodata(此 elf2nro 的 romfs 支持不可靠)。脚本必须保持纯 ASCII 注释(PS 5.1 对无 BOM 的 UTF-8 中文注释解析会错乱)。

### 标准 devkitPro / CI

装有官方 devkitPro(Windows 安装版或 Linux)时可直接:

```bash
export DEVKITPRO=/opt/devkitpro
make -j$(nproc)
# 字体走 romfs 路径(app.c 已做 romfs→嵌入内存 两级回退)
```

GitHub Actions 见 `.github/workflows/build.yml`(devkitpro/devkitarm 镜像自动出 .nro)。

## 安装到 Switch

1. 把 `switch-dsh-client.nro` 放进 SD 卡 `switch/` 目录(可在 hbmenu 里直接运行,或用 NRO 前向器做成桌面图标)
2. 首次启动用内置默认配置;配置会保存到 `sdmc:/switch/switch-dsh-client/config.json`
3. 需要联网(局域网或公网)

## 后端部署

### 方式 A:连本机 DeepSeek Harness(推荐,复用现有会话/模型)

**一键启动桥接(任选其一):**

1. **双击 `start-bridge.bat`**:自动启动桥接并显示本机局域网 IP(把 `http://<IP>:8765` 填到 Switch);关窗口即停止。
2. **本网页侧栏底部按钮**(动态插件 `dsh-bridge launcher`):绿色 ● = 运行中,点击切换启停。插件需要你在界面上批准后生效;DSH 重启后插件消失,.bat 始终可用。

也可以手动运行:

```bash
node bridge/dsh-bridge.js [--dsh http://127.0.0.1:3080] [--host 0.0.0.0] [--port 8765]
```

Switch 端:打开 APP 在启动界面选 `Harness`,设置界面里 Harness 地址填 `http://<电脑局域网IP>:8765`。

> 安全提示:DSH API 无鉴权且可执行远程代码,桥接**只应在可信局域网**运行,勿暴露公网。Windows 防火墙如拦截,放行 Node 的 8765 端口即可。

### 方式 B:直连 DeepSeek 官方 API

设置界面里:
- 后端切到 `DeepSeek`
- API Key 填入你的 key
- 模型默认 `deepseek-v4-pro`(也可 `deepseek-v4-flash`;`deepseek-chat`/`deepseek-reasoner` 已弃用)
- 思考模式 `disabled`(关闭思维链,响应更快)/ `enabled`(开启,一期暂不显示思考过程)

HTTPS 走 Switch 系统 SSL(系统 CA),无需配置证书。

## 配置字段(config.json)

| 字段 | 说明 |
| --- | --- |
| `backend` | `"harness"` 或 `"deepseek"` |
| `harness_base_url` | 指向 dsh-bridge,如 `http://192.168.1.10:8765` |
| `deepseek_base_url` | 默认 `https://api.deepseek.com` |
| `deepseek_api_key` | DeepSeek API Key(见下方 key.txt 快捷方式) |
| `model` | 模型名(默认 `deepseek-v4-pro`) |
| `deepseek_thinking` | `"enabled"` / `"disabled"` |
| `system_prompt` | 系统提示词(可空) |

加载顺序:SD 卡 config.json → 内置默认值。

### API Key 免手输:独立文本文件

在 Switch 上敲 API Key 太麻烦,可以**在电脑上直接把 Key 写进一个文本文件**:

1. 在 SD 卡 `sdmc:/switch/switch-dsh-client/` 目录新建 `deepseek_api_key.txt`,内容就是一行 API Key(前后空白/换行会被自动忽略;仓库里有 `deepseek_api_key.txt.example` 参考)
2. 应用每次启动自动读取;**该文件存在且非空时优先于 config.json 里的值**
3. 设置界面里的"API Key"项仍然可以修改(修改后会自动同步写回 key.txt);PC 上手动改 key.txt 后,重启应用即生效

> 提示:key.txt 只保存在你自己的 SD 卡上,不会同步到别处。**不要把这个文件提交到 git 或发给别人。**

## 操作

| 按键 | 启动选择界面 | 聊天界面 | 设置界面 |
| --- | --- | --- | --- |
| A | 确定所选后端 | 输入消息(中文拼音键盘) | 修改/切换当前项 |
| X | — | Harness:工作区;DeepSeek:清屏 | — |
| Y | — | 打开设置 | — |
| L | — | 切换模型(对话页直接选版本) | — |
| R | — | **一键切换后端(DSH ↔ DeepSeek)** | — |
| B | — | — | 保存并返回 |
| ↑/↓/←/→ | 切换后端 | — | 选择设置项 |
| + | 退出 | 退出 | 退出 |

**触摸屏**:所有列表/卡片界面(后端选择、工作区、会话、模型、设置)都可以**直接点击** —— 点击卡片即选中并执行(等同 A 键);聊天界面点底部栏 = 弹出输入框。

Harness 模式补充:启动选 Harness 后进入**工作区界面**(新建会话 / 新建工作区 / 按工作区浏览会话 / 全部会话平铺);会话切换后自动载入历史;`L` 打开模型选择(Harness 为会话级 `session.selectModel`,DeepSeek 为全局 `cfg.model` 并持久化)。

## 测试状态

- ✅ dsh-bridge 三项能力(POST 转发 / WS→SSE / 会话过滤)对真实 DSH 实测通过
- ✅ Harness 后端端到端实测(`tests/host_test.exe`,本机编译 C 网络/协议代码,走真实 bridge+DSH 完成流式对话,`HOST TEST: PASS`)
- ✅ 全量交叉编译通过,.nro 结构(魔数/段链/嵌入字体)校验通过
- ⏳ 真机/模拟器运行验证(Ryujinx 支持 bsd socket,局域网 HTTP 可跑;nxlink 可看日志)

## 二期:AI 语音输入(规划)

- Switch 本体**无麦克风**,方案:USB 耳麦(UAC)经 libnx `audin` 采集 PCM
- 本机跑 Whisper 转写服务(whisper.cpp server 或 faster-whisper,与 dsh-bridge 同机部署)
- 应用内接口已预留:`source/stt.h`(`stt_transcribe`);采集→转写→文本自动注入输入框

## 许可

- 本项目代码:MIT
- 嵌入字体 Noto Sans CJK SC:SIL OFL 1.1(`LICENSE-FONTS.txt`)
- vendored cJSON:MIT(`libs/cjson/`)
