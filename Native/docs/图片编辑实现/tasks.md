# 图片编辑实现任务清单

> 对应 `Native/docs/图片编辑实现/spec.md`。
> 规则：按 M1 → M2 → M3 → M4 顺序推进；每个里程碑完成即构建并汇报，不攒到最后。

## M1 窗口与入口

- [x] T1.1 新增 `Native/Src/Win/WinImageEdit.h`：窗口类、`init()` / `dispose()`、`Mode` 枚举、`setMode()`、`hasContent()`、`rebuildForMode()`。
- [x] T1.2 新增 `Native/Src/Win/WinImageEdit.cpp` 构造与 `onCreated()`：1159×807、居中、最小尺寸、原生标题栏深色、窗口底 `#262626`、body 内边距 19/19/16/19。
- [x] T1.3 左列：画布宿主（`#171717`、圆角 20）、全尺寸 `Ling::Canvas`、空态提示、素材条占位（拼图）与合并模式铺满。
- [x] T1.4 底部工具栏占位：`left 80 / bottom 19 / 740×42`，分割 `\ue68b`、布局 `\ue689`、撤销 `\ue63f`、重做 `\ue60a`，点击仅切换选中态。
- [x] T1.5 右列：模式胶囊（206×32，选中 102×32）、尺寸行（86×28 ×2 + 中间互换按钮）、模式属性占位、取消/保存按钮 70×32 右对齐。
- [x] T1.6 模式切换：无内容直接切；有内容走 `SettingUi::ConfirmDialog::ask`；M1 `hasContent()` 返回 false。
- [x] T1.7 关闭生命周期：静态实例 + `onDestroy` 延迟 `reset()`；窗口全关后释放 D2D 与工作集。
- [x] T1.8 `WinSettingHotkeys.cpp`「其他」组在 `show_main` 后插入 `make_collage`。
- [x] T1.9 `Setting.cpp`：`kHotkeys` 追加 `{ L"make_collage", 113 }`；`runHotkeyAction` 派发到 `WinImageEdit::init()`。
- [x] T1.10 `Tray.cpp`：`MenuCmd` 增 `kCmdMakeCollage`；`kMenuRows` 在快捷翻译后插入；`runTrayCommand` 派发。
- [x] T1.11 `IconCodes.h` 增 `Split` / `Layout` / `Dimension01`。
- [x] T1.12 用新 `iconfont.ttf` 覆盖 `Native/Src/Res/SnowAirIcons.ttf`。
- [x] T1.13 `lang.zh-CN.json` / `lang.en-US.json` 增加 spec 3.5 的键。
- [x] T1.14 `SnowAir.vcxproj` 登记 `Win\WinImageEdit.cpp` 与 `.h`。
- [x] T1.15 构建 Release x64；按 spec 7 M1 验收；单独汇报。

## M2 拼图模式

- [x] T2.1 新增 `Native/Src/Win/ImageEditCollage.h/.cpp`：格子模型、选中、工具、缩放、背景、边距/间距/圆角、撤销栈。
- [x] T2.2 右侧拼图属性区：外边框/间距/圆角（数值 60×28 + 滑条 202×4）、色板、图片背景、布局卡片、行/列。
- [x] T2.3 布局卡片应用与拖出格子（行/列为上限，Shift 锁拖拽起始宽高比）。
- [x] T2.4 填图：单击选图、双击替换、拖入格子；等比铺满裁切，Shift 允许拉伸。
- [x] T2.5 分割：任意角度、Shift 8 向吸附、松手裁两格；二次调整拖动已有分割线。
- [x] T2.6 滚轮缩放（以鼠标为中心，20%–1000%，左上角临时显示）与尺寸显示/PX 百分比切换。
- [x] T2.7 顶部胶囊：选中显隐，旋转与居中（居中图标自绘）。
- [x] T2.8 撤销/重做接入工具栏。
- [x] T2.9 构建并验收，单独汇报。

## M3 合并文档模式

- [x] T3.1 新增 `Native/Src/Win/ImageEditMerge.h/.cpp`：路径列表、卡片布局、排序、属性。
- [x] T3.2 多选导入与空态「添加图片」。
- [x] T3.3 拖拽排序与插入位置提示。
- [x] T3.4 右侧属性：外边框、背景色/图、格式下拉。
- [x] T3.5 尺寸 0 与指定尺寸的页面规则。
- [x] T3.6 构建并验收，单独汇报。

## M4 导出

- [x] T4.1 拼图位图渲染与 `Util::saveToFile` 落盘（PNG/JPEG/WebP/BMP）。
- [x] T4.2 `Export/PdfWriter.h/.cpp`：每图一页，JPEG + DCTDecode，页面留白。
- [x] T4.3 `Export/PptxWriter.h/.cpp`：ZIP + 必需 XML + 媒体，每图一页。
- [x] T4.4 保存入口与默认桌面路径、命名。
- [x] T4.5 构建并验收，单独汇报。
