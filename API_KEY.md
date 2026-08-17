# API Key 存放位置 / Where the API Key Lives / APIキーの保存場所

## 中文

DeepSeek API Key **不存储在仓库中**,而是存放在 Nintendo Switch 的 SD 卡上:

```
sdmc:/switch/switch-dsh-client/deepseek_api_key.txt
```

- 文件内容:一行 API Key(前后空白会被自动忽略)
- 优先级:该文件存在且非空时,优先于 `config.json` 中的 `deepseek_api_key` 字段
- 在 Switch 的「设置 → API Key」子菜单中可选择「从 key.txt 读取」;修改后会自动同步写回
- 🔒 **切勿**把此文件提交到 Git 或公开分享(仓库 `.gitignore` 已排除)

## English

The DeepSeek API key is **not stored in this repository**. It lives on the
Nintendo Switch SD card:

```
sdmc:/switch/switch-dsh-client/deepseek_api_key.txt
```

- Content: a single line with the key (leading/trailing whitespace is ignored)
- Precedence: if the file exists and is non-empty, it overrides the
  `deepseek_api_key` field in `config.json`
- On the Switch, open Settings → API Key → "Load from key.txt"; edits made in
  the menu are written back to the file automatically
- 🔒 **Never** commit this file or share it publicly (it is already excluded
  by `.gitignore`)

## 日本語

DeepSeek APIキーは**このリポジトリには含まれません**。Nintendo Switch の
SDカードに保存されます:

```
sdmc:/switch/switch-dsh-client/deepseek_api_key.txt
```

- 内容:キーを1行で記載(前後の空白は自動的に無視されます)
- 優先順位:ファイルが存在し空でない場合、`config.json` の
  `deepseek_api_key` より優先されます
- Switch 側では「設定 → API Key」メニューの「key.txt から読み込む」を選択。
  メニューで編集した内容は自動的にファイルへ書き戻されます
- 🔒 このファイルを Git にコミットしたり公開したり**しないでください**
  (`.gitignore` で除外済み)
