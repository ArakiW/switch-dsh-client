# DEEPSEEK HARNESS：PC 端 Web GUI vs Switch 端客户端 — 完整对比与改进方案

> 生成时间：2026-08-18  
> 基于当前 `switch-dsh-client` 仓库 `master` 分支代码、DSH Harness 运行时、以及本次开发过程的实际验证。

---

## 1. 系统架构对比

| 维度 | PC 端 DeepSeek Harness Web GUI | Switch 端客户端（`.nro`） |
|---|---|---|
| 运行环境 | Windows PC, Node.js, DSH 进程 | Nintendo Switch, libnx + SDL2, Homebrew |
| 渲染 | React SPA (Vite, Chromium WebView2) | 原生 SDL2 + SDL2_ttf + 8-bit 纹理缓存 |
| 与 Harness 通信 | 直连 `127.0.0.1:3080` | 经 `dsh-bridge.js` 局域网转发 (`<IP>:8765`) |
| DeepSeek API 直连 | 支持（同一 SPA 切换后端） | 支持（设置里切后端） |
| 插件系统 | Cordis 插件 (host + client, `dsh.plugin add`) | 无插件，单体 `.nro` |
| 文件系统 | PC 本地文件系统 | Switch SD 卡 (`sdmc:/`) |

---

## 2. 功能对比（逐项）

### 2.1 后端切换

| 功能 | PC 端 | Switch 端 | 差异说明 |
|---|---|---|---|
| Harness 桥接 | ✅ 直连（无桥接） | ✅ 经 `dsh-bridge.js` | Switch 依赖桥接；PC 无需桥接 |
| DeepSeek API 直连 | ✅ | ✅ | 一致 |
| 一键切后端 | ✅ 设置页切换 | ✅ `R` 键 / 设置 | 一致（双缓冲会话） |
| 工作区（Harness） | ✅ 侧边栏拖拽/排序 | ✅ 侧边栏切换（DeepSeek 模式下已隐藏+提示） | Switch DeepSeek 模式无此功能（已加提示） |
| 会话列表（Harness） | ✅ 侧边栏 + 分组 | ✅ 侧边栏 | 一致 |

### 2.2 聊天与 AI

| 功能 | PC 端 | Switch 端 | 差异说明 |
|---|---|---|---|
| 流式逐字输出 | ✅ | ✅ (SSE) | 一致 |
| 思考过程（暗色） | ✅ | ✅ | 一致 |
| 工具活动行 | ✅ | ✅ | 一致 |
| 审批/提问提醒 | ✅ 弹窗 UI | ✅ 醒目色文本行 | PC 更友好 |
| 任务清单（todo） | ✅ 实时更新 | ✅ 实时更新 | 一致 |
| 模型切换 | ✅ 选择器 | ✅ `L` 键选择器 | PC UI 更丰富；Switch 已覆盖核心 |
| 推理强度 | ✅ | ✅ (low/high) | 一致 |
| 历史翻页 | ✅ 滚动到最旧自动加载 | ✅ `ZL` 加载更早 | 一致 |

### 2.3 输入

| 功能 | PC 端 | Switch 端 | 差异说明 |
|---|---|---|---|
| 键盘文本输入 | ✅ 物理键盘 | ✅ 系统软键盘 (swkbd, 中文拼音) | Switch 3.5mm 耳机不影响输入 |
| 语音输入 STT | ✅ 浏览器 Web Speech API (🎤 按钮) | ✅ `ZR` 按住录音 → POST whisper 服务 | PC 即时；Switch 需外部 whisper 服务 |
| 语音朗读 TTS | ✅ 本机 Windows SAPI (`tts_speak`) | ✅ espeak-ng 本地合成 (SDL2 播放) | PC 音质更好（Huihui）；Switch 机器人腔 |
| 触摸输入 | ✅ | ✅ (拖动滚动/点选) | Switch 触摸优化 (60fps) |
| 手柄输入 | — | ✅ (A/B/X/Y/L/R/ZL/ZR/摇杆/+) | Switch 独有 |

### 2.4 UI / 显示

| 功能 | PC 端 | Switch 端 | 差异说明 |
|---|---|---|---|
| 布局 | SPA 流式，侧边栏可折叠 | 双栏（侧栏 360px + 对话区） | Switch 对齐桌面版 |
| 60fps 渲染 | ✅ (Chromium) | ✅ (SDL2 + 纹理缓存 + vsync) | 一致（Switch 已优化） |
| Markdown-lite | ✅ (React 渲染器) | ✅ (C 渲染：粗体/标题/代码块) | PC 更完整；Switch 已覆盖核心 |
| 缺字处理 | ✅ (Web 字体) | ✅ (NotoSans CJK SC + utf8_sanitize) | Switch 过滤 emoji/dingbat 等无法渲染字符 |
| 暗色主题 | ✅ (web 切换) | ✅ (固定暗色，对齐 DeepSeek 设计) | 一致 |

### 2.5 桥接 / 网络

| 功能 | PC 端 | Switch 端 | 差异说明 |
|---|---|---|---|
| 启动桥接 | ✅ 一键按钮（插件；当前有加载问题，待修） | — | Switch 不启动桥接（桥接在 PC） |
| 启动状态 | ✅ 绿/灰 + 点击 | — | 同上 |
| /stt 转发 | ✅ (dsh-bridge 已内置) | — | Switch 只负责发音频，PC 端转发 |

### 2.6 设置 / 配置

| 功能 | PC 端 | Switch 端 | 差异说明 |
|---|---|---|---|
| 后端地址设置 | ✅ 设置页 | ✅ 设置页 | 一致 |
| API Key | ✅ 设置页 | ✅ 设置页 + key.txt 优先 | Switch 更方便（PC 上直接写 key.txt） |
| 模型选择 | ✅ 设置页 + 工具栏 | ✅ `L` 键选择器 | 一致 |
| 思考模式 | ✅ 设置页 | ✅ 设置页 / 聊天页 | 一致 |
| 系统提示词 | ✅ 设置页 | ✅ 设置页 (中文软键盘) | 一致 |
| 语音朗读开关 | — | ✅ 设置页 | Switch 独有（PC 端 TTS 是工具调用，非 UI 开关） |
| STT 服务地址 | — | ✅ 设置页 `stt_url` | Switch 独有 |
| 导出/分享 | ✅ 原生文件系统 | ❌ | Switch 不支持导出（无文件选择器） |

### 2.7 历史持久化

| 功能 | PC 端 | Switch 端 | 差异说明 |
|---|---|---|---|
| Harness 对话 | ✅ 服务端管理 | ✅ 服务端管理 + 本地 `history_harness.json` 备份 | 一致 |
| DeepSeek 对话 | ✅ 本地持久化 | ✅ `history_deepseek.json` | 一致 |
| 双后端缓冲 | — | ✅ 切后端不丢历史（独立缓冲） | Switch 独有 |

---

## 3. 已知问题与差距

### 3.1 PC 端（DeepSeek Harness Web GUI）

| 问题 | 状态 | 原因 |
|---|---|---|
| 桥接按钮不显示 | ❌ 未修 | DSH 的 bundle/patch-insert 加载机制对 `file:`/`github:` 依赖的 HOST 插件有问题；VOICE-INPUT（CLIENT 插件）能加载，但 BRIDGE-LAUNCHER（HOST 插件）加载不上 |
| 超级注入器（super-injector）坏 | ❌ | `lib/` 目录缺失，被 DSH Desktop 每次启动禁用，但其存在可能干扰其它 bundle 加载 |

### 3.2 Switch 端（`.nro` 客户端）

| 问题 | 状态 | 说明 |
|---|---|---|
| 部分 UI 文字重叠 | ❌ 待修 | 需实机验证；代码审查未发现明显 bug（换行/截断逻辑看着对） |
| STT 需外部 whisper 服务 | 设计如此 | Switch 本地无 Whisper 能力；需 PC 运行 whisper 服务并配 `stt_url` |
| TTS 机器人腔（espeak-ng） | 设计如此 | 本地 espeak-ng 中文音质有限；用户选了本地方案 |
| `stt_url` 空字符串 = 禁用 | 已实现 | 用户需手动配置 whisper 服务地址 |

---

## 4. 改进方案（按优先级排序）

### 优先级 P0：修复现有问题

| 编号 | 任务 | 涉及端 | 预估难度 |
|---|---|---|---|
| 0-1 | **PC 桥接按钮修复**：DSH HOST 插件加载机制需排查（可能需要用 CLIENT 插件 + host.call 方案替代，或修改 dist/index.html 直接注入） | PC | 中高 |
| 0-2 | **Switch UI 文字重叠**：实机复现 → 定位渲染函数 → 修复（可能是某段超长文本的换行/截断边界） | Switch | 中 |
| 0-3 | **TTS / STT 实机验证**：用户需在真 Switch 上验证 espeak-ng 出声 + audin 麦克风采集 | Switch | 用户操作 |

### 优先级 P1：功能增强

| 编号 | 任务 | 涉及端 | 预估难度 |
|---|---|---|---|
| 1-1 | **TTS 解除 Harness 限制**：espeak-ng 是本地合成，不依赖 PC；应可在 DeepSeek 直连模式也生效 | Switch | 低（改条件判断） |
| 1-2 | **STT 推理强度 / 声音调节**：让用户在 Switch 上调 espeak-ng 语速/音量/音高 | Switch | 低 |
| 1-3 | **导出对话到 SD 卡**：把当前对话导出为 JSON/TXT 文件 | Switch | 低 |
| 1-4 | **桥接状态指示**：Switch 侧栏底部显示桥接连接状态（在线/离线/延迟） | Switch | 中 |
| 1-5 | **手柄按键提示动态化**：按住 ZR 录音时，底栏变成「松开发送」 | Switch | 低 |

### 优先级 P2：体验优化

| 编号 | 任务 | 涉及端 | 预估难度 |
|---|---|---|---|
| 2-1 | **Markdown 渲染增强**：Switch 端支持列表/链接/表格（目前只支持粗体/标题/代码块） | Switch | 中 |
| 2-2 | **设置页增加 STT 地址说明**：显示当前 stt_url 配置的 whisper 服务是否可达（ping 检查） | Switch | 低 |
| 2-3 | **PC 端桥接按钮方案**：如果 DSH HOST 插件加载机制短期无法修，采用 dist/index.html 直注入方案 | PC | 中 |
| 2-4 | **TTS 音量/语速持久化**：用户设定的语速/音量保存到 config.json | Switch | 低 |
| 2-5 | **安全提醒**：在 DeepSeek 直连模式显示「API Key 存在 SD 卡，请注意安全」 | Switch | 低 |

### 优先级 P3：未来方向（第二阶段规划）

| 编号 | 任务 | 说明 |
|---|---|---|
| 3-1 | **IR Scanner 集成**：把 Joy-Con 红外扫描器作为 Switch 端的附加功能模块 |
| 3-2 | **Switch 端 Agent UI**：将 Switch 作为 AI Agent 的物理控制台（审批/提问/语音指令） |
| 3-3 | **Haptic 反馈**：HD Rumble 指示工具执行/完成/错误等状态 |
| 3-4 | **IMU 辅助**：Joy-Con 陀螺仪用于消息滚动或导航 |
| 3-5 | **多端同步**：Switch 端与 PC 端实时状态同步（跨设备继续对话） |

---

## 5. 附录：桥接按钮问题技术细节

### 现象
DSH Desktop 重启后，`cordis.patch.yml` 里的 `dsh-bridge-launcher`（HOST 插件）加载不上：`webServer` 路由不注册，`tapIndex` 不生效，`<script>` 不注入。

### 排除
- 包代码正确（`apply(ctx)` / `inject` / `webServer.register` API 匹配 DSH 类型定义）。
- 包文件存在（`lib/index.js`、`bridge/dsh-bridge.js` 均完整）。
- `clientModules` 能加载 CLIENT 插件（`dsh-voice-input` 在 `__DSH_BOOT__` 中）。
- `compat.patch.yml` 只禁用了坏的 `dsh-super-injector`，未影响 bridge-launcher。
- 曾经从 bundle 加载改为 patch-insert，仍未加载。
- 用动态插件（`cordis_define`/`cordis_run`）能正常注册路由（但会话级，重启消失）。

### 可能原因
DSH 的 HOST 插件组合机制对 `file:`/`github:` 依赖的外部包支持有限或存在 bug；CLIENT 插件走另一条加载路径（`clientModules` 扫描 + 注入），不受影响。

### 下一步建议
1. 检查 DSH Desktop / `@deepseek-ai/dsh` 的宿主插件加载日志（需要更高级的调试手段）。
2. 备选方案：修改 `dsh-web-frontend/dist/index.html`，把按钮脚本直接内联进去（绕过插件机制，但不持久）。
3. 用 `host.call` 方案：桥接按钮做 CLIENT 插件，`host.call` 调 host 端的 `handle()` 注册，但 host 加载不上时 `host.call` 也失效——循环问题。

---

## 6. 结论

Switch 端客户端的核心功能（双后端、流式聊天、TTS、STT、工作区、模型切换）已基本对齐 PC 端，差距主要在：
- PC 端桥接按钮（插件加载问题）
- Switch 端 UI 文字重叠（需实机）
- TTS/STT 实机验证

改进应优先修复上述三个问题，然后推进「功能增强」列表中的低难度项。
