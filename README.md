# switch-dsh-client

> A native DeepSeek AI chat client for Nintendo Switch homebrew.

`switch-dsh-client` 是一个运行在**已破解 Nintendo Switch** 上的 DeepSeek AI 聊天客户端，使用 **libnx + SDL2 + libcurl** 原生实现。

项目支持两种后端，可在客户端内一键切换，并分别保存各自的对话记录：

* **局域网 DeepSeek Harness**：通过零依赖 Node 桥接脚本 `dsh-bridge` 转发 HTTP RPC，并将 WebSocket 转换为 SSE。
* **DeepSeek 官方 API**：直接通过 HTTPS + SSE 连接，使用 Bearer API Key 认证。

---

## ✨ 特性

* **双后端支持**

  * 局域网 DeepSeek Harness
  * DeepSeek 官方 API
  * 客户端内一键切换
  * 两套后端的对话记录独立保存

* **SSE 流式响应**

  * 支持逐字流式输出
  * DeepSeek 官方 API 使用 HTTPS + SSE
  * Harness 通过 `dsh-bridge` 将 WebSocket 转换为 SSE

* **思考过程显示**

  * 思考内容使用暗色、缩进样式展示
  * 与最终回答保持视觉区分

* **任务状态展示**

  * 工具活动行
  * 审批提醒
  * 提问提醒
  * 任务清单条

* **双栏工作区界面**

  * 左侧：会话列表，并按工作区分组
  * 右侧：当前对话流
  * 整体交互风格对齐桌面版 DeepSeek 工作区

* **会话与工作区管理**

  * 新建会话
  * 分叉会话
  * 重命名会话
  * 工作区管理

* **聊天页模型设置**

  * `deepseek-v4-pro`
  * `deepseek-v4-flash`
  * 思考模式 `enabled / disabled`
  * 可直接在聊天页面切换模型与思考模式

* **中文输入**

  * 调用 Nintendo Switch 系统软键盘 `swkbd`
  * 支持简体中文拼音输入

* **触摸 + 手柄双操作**

  * 触摸拖动滚动
  * 触摸点选
  * 手柄操作
  * 60 FPS 平滑滚动

* **本地数据保存**

  * 配置保存在用户自己的 SD 卡
  * 对话历史保存在用户自己的 SD 卡
  * API Key 保存在用户自己的 SD 卡

---

## 📸 截图

> 待补充。

---

## 🚀 快速开始

### 1. 安装到 Nintendo Switch

准备一台能够运行 homebrew 的 Nintendo Switch。

获取构建完成的 `.nro` 文件后，将其放入 Switch SD 卡上的 homebrew 应用目录，并通过 Homebrew Menu 启动 `switch-dsh-client`。

GitHub Actions 会自动构建 `.nro` 成品；创建 Git tag 时会自动生成对应 Release。

---

### 2. 选择后端

`switch-dsh-client` 支持两种后端，两者可以在客户端内切换，且对话记录彼此独立。

#### 方式 A：局域网 DeepSeek Harness

适合已经在局域网内运行 DeepSeek Harness 的环境。

在与 Switch 同一局域网中的设备上运行 `dsh-bridge`。

`dsh-bridge` 是一个**零依赖 Node 桥接脚本**，负责：

1. 接收 Switch 客户端请求
2. 转发 Harness HTTP RPC
3. 将 Harness WebSocket 数据转换为 SSE
4. 在局域网端口 `8765` 提供服务

例如，运行桥接服务的设备地址为：

```text
192.168.1.10
```

则 Switch 端连接目标为：

```text
http://192.168.1.10:8765
```

Switch 与运行 `dsh-bridge` 的设备需要能够通过局域网互相访问。

---

#### 方式 B：DeepSeek 官方 API

客户端也可以直接连接 DeepSeek 官方 API，无需局域网 Harness。

该模式使用：

```text
HTTPS + SSE
```

认证方式为：

```text
Bearer API Key
```

将自己的 DeepSeek API Key 保存到客户端使用的 API Key 文本文件中，然后在客户端切换至官方 API 后端即可。

API Key 仅作为本地配置持久化在用户自己的 SD 卡中。

---

## ⌨️ 操作指南

`switch-dsh-client` 同时支持触摸屏与手柄操作。

| 操作   | 功能                                             |
| ---- | ---------------------------------------------- |
| 触摸点选 | 选择会话、工作区或界面项目                                  |
| 触摸拖动 | 滚动会话列表或对话内容                                    |
| 手柄导航 | 在界面项目之间移动与选择                                   |
| 文本输入 | 调用 Switch 系统软键盘 `swkbd`                        |
| 中文输入 | 使用系统软键盘进行简体中文拼音输入                              |
| 后端切换 | 在 Harness 与 DeepSeek 官方 API 之间切换               |
| 模型切换 | 在 `deepseek-v4-pro` 与 `deepseek-v4-flash` 之间切换 |
| 思考模式 | 在 `enabled` 与 `disabled` 之间切换                  |

---

## ⚙️ 配置

客户端的配置、对话历史和 API Key 均保存在用户自己的 SD 卡上。

### `config.json`

客户端配置保存在 SD 卡的 `sdmc:/switch/switch-dsh-client/config.json`；仓库内的 `config.json.example` 为参考模板。

| 字段                  | 说明                                | 示例                                 |
| ------------------- | --------------------------------- | ---------------------------------- |
| `backend`           | 后端类型：`harness` 或 `deepseek`         | `harness`                          |
| `harness_base_url`  | Harness 桥接地址（主机 + 端口）              | `http://192.168.1.10:8765`         |
| `deepseek_base_url` | DeepSeek 官方 API 地址                 | `https://api.deepseek.com`         |
| `deepseek_api_key`  | 官方 API 密钥（可留空，改用 key.txt）         |                                    |
| `model`             | 模型                                | `deepseek-v4-pro`                  |
| `deepseek_thinking` | 思考模式                              | `enabled` / `disabled`             |
| `system_prompt`     | 系统提示词（可选）                         |                                    |

`model` 与 `deepseek_thinking` 也可以直接在聊天页面中切换。

### API Key 文本文件

使用 DeepSeek 官方 API 后端时，需要提供自己的 API Key。

API Key 使用独立的纯文本文件保存在 SD 卡：

```text
sdmc:/switch/switch-dsh-client/deepseek_api_key.txt
```

- 文件内容为一行 API Key（前后空白会被自动忽略）
- 该文件存在且非空时，优先于 `config.json` 中的 `deepseek_api_key` 字段
- 在 Switch 的「设置 → API Key」菜单可选择「从 key.txt 读取」
- 该文件已被 `.gitignore` 排除，请勿提交或公开

Harness 后端不使用 DeepSeek 官方 API Key。

---

## 🔨 构建

项目支持两种构建方式。

### Windows：clang + ld.lld

Windows 环境下可以使用：

* `clang`
* `ld.lld`

进行 Nintendo Switch 目标的交叉编译。

依赖目录通过环境变量：

```text
DSH_SWITCH_DEPS
```

指定。

例如：

```powershell
$env:DSH_SWITCH_DEPS = "<依赖目录>"
```

随后使用仓库对应的 Windows 构建入口进行编译。

---

### devkitPro

项目同时支持标准 devkitPro 构建环境。

在配置好 Nintendo Switch homebrew 开发环境后，可以使用：

```bash
make
```

进行构建。

---

### GitHub Actions

仓库配置了 GitHub Actions 自动构建：

* 自动生成 `.nro` 构建产物
* 创建 Git tag 时自动发布对应 Release

---

## 🛠 技术栈

| 组件                   | 用途                            |
| -------------------- | ----------------------------- |
| **libnx**            | Nintendo Switch homebrew 原生接口 |
| **SDL2**             | 界面、输入与渲染基础                    |
| **SDL2_ttf**         | 字体渲染                          |
| **SDL2_gfx**         | SDL2 图形辅助                     |
| **SDL2_image**       | 图像处理                          |
| **libcurl**          | HTTP / HTTPS 与 SSE 网络通信       |
| **cJSON**            | JSON 解析，vendored              |
| **Noto Sans CJK SC** | 内嵌简体中文字体                      |

客户端主体使用原生 C/C++ homebrew 技术栈实现。

Harness 模式额外使用零依赖 Node 脚本 `dsh-bridge`，负责局域网 RPC 转发以及 WebSocket 到 SSE 的转换。

---

## 📄 许可

### 项目代码

`switch-dsh-client` 项目代码采用：

**MIT License**

### Noto Sans CJK SC

项目内嵌的 **Noto Sans CJK SC** 字体采用：

**SIL Open Font License 1.1**

### cJSON

项目中 vendored 的 **cJSON** 采用：

**MIT License**

使用、修改或重新分发时，请同时遵守对应组件的许可条款。
