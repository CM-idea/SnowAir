# SnowAir

轻雪 — Windows 原生截图工具（C++20 / Ling / D2D）。

## 特性

- 区域截图、标注、长截图、录屏（GIF / MP4）、OCR、二维码识别
- 托盘常驻、全局热键（默认 `F1`）
- 单 exe、静态链接 CRT，无额外运行库依赖（OCR 插件除外）

## 目录

| 路径 | 说明 |
|------|------|
| [`Src/`](Src/) | 主程序源码 |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | **架构速查**（Ling 框架 / SettingUi 浮层 / WinSetting 布局 / 待修 BUG），改代码前先读 |
| [`Lang/`](Lang/) | 多语言 JSON |
| `deps/Ling/` | 定制版 Ling GUI 框架（未随仓库发布） |

## 开发环境

如需阅读或修改源码，建议安装：

- Windows 10 1803+
- Visual Studio 2026（v145 工具集，含「使用 C++ 的桌面开发」）
- Windows 10 SDK

## 构建说明

SnowAir 本体以 **Apache License 2.0** 开源；运行依赖定制版 Ling（含未公开实现），该分支不随仓库发布，因此不提供从源码直接构建。

需要运行请使用 [Releases](https://github.com/CM-idea/SnowAir/releases) 中的 `SnowAir.exe`。

## 命令行

不带参数启动时**只挂托盘图标待命**，不会自动进入截图模式：

```text
SnowAir.exe                     # 托盘待命（默认）
SnowAir.exe --auto-quit=true          # 截完即退出
SnowAir.exe --enter=pin               # 框选后直接进入钉图
SnowAir.exe --enter=long              # 长截图
SnowAir.exe --enter=video             # 录屏
SnowAir.exe --enter=tray              # 仅托盘（等同于默认行为）
```

截图入口：托盘图标单击（按「功能设置 → 单击托盘执行」：截图 / 打开设置 / 无操作）、
全局热键 `F1`、或再次运行 exe（打开设置中心）。

便携模式：在 exe 同目录放置空文件 `config.json`，配置与语言改从 exe 目录读取。

## 数据目录

默认：`%AppData%\SnowAir\`（配置、语言、插件、历史）

## OCR 插件

将 [ImageReader.exe](https://github.com/xland/ImageReader/releases) 放到 `%AppData%\SnowAir\plugin` 或 exe 同目录。

由 [CM-idea](https://github.com/CM-idea) 维护。
