# 图片编辑实现规格

> 依据：`Native/docs/图片编辑-需求存档.md`（用户确认稿 + 两份 SVG 布局）。
> 范围：按里程碑实现独立「图片编辑」窗口，含拼图、合并文档两个模式与两处入口。
> 约束：不改可执行文件名（SnowAir.exe）；不破坏截图、标注、热键、托盘等现有功能；不执行 git 操作。

---

## 1. 里程碑与交付边界

| 里程碑 | 交付内容 | 完成判据 |
|---|---|---|
| M1 窗口与入口 | 独立窗口骨架、模式切换、两处入口、工具栏占位 | 构建通过；托盘与热键均可打开窗口；两模式可切换且视觉分区正确 |
| M2 拼图模式 | 布局格子、填图、分割、缩放、背景、边距/间距/圆角、撤销重做 | 能在画布中完成一次拼图并保留状态 |
| M3 合并文档 | 图片导入、卡片排序、文档属性、尺寸规则 | 可导入多图并调整顺序、设置属性 |
| M4 导出 | 拼图位图导出、PDF / PPTX 生成、另存为 | 导出的文件可被系统与外部程序正常打开 |

每个里程碑单独汇报：做了什么、可验证程度、下一步、需用户拍板的点。

---

## 2. 已自取默认值的 4 处未定项

| 编号 | 默认取值 | 理由 |
|---|---|---|
| 1 分割二次调整 | 选中格子并激活分割工具后，显示该格子已有的分割线；拖动端点调整角度与位置，松手按新线重新裁切 | 保留可回退的编辑感，避免“再切一刀”导致格子数量不可控 |
| 2 两格选择单元 | 分割产生的左右两格可分别单击并各自填图；整体移动/缩放时按一个单元处理 | 满足“分别填不同图片”，同时不破坏非矩形区域的对齐 |
| 3 Shift 约束比例基准 | 约束拖出矩形自身的宽高比（拖拽开始时锁定的比例） | 布局格子与图片内容解耦，符合“拖出格子时等比缩放”的描述 |
| 4 合并外边框 | 计入页面留白：图片在去掉外边框后的区域内居中 | 与“外边框：页面四周留白”的语义一致 |

---

## 3. M1 详细规格

### 3.1 新增与修改文件

| 文件 | 操作 | 内容 |
|---|---|---|
| `Native/Src/Win/WinImageEdit.h` | 新增 | 窗口类声明：`init()`、`dispose()`、模式枚举、模式切换与内容判定 |
| `Native/Src/Win/WinImageEdit.cpp` | 新增 | 窗口构造、1159×807 布局、模式胶囊、画布宿主、工具栏占位、属性栏骨架、取消/保存 |
| `Native/Src/Win/WinSettingHotkeys.cpp` | 修改 | 「其他」组在 `show_main` 之后插入 `make_collage` |
| `Native/Src/Setting.cpp` | 修改 | `kHotkeys` 追加 `{ L"make_collage", 113 }`；`runHotkeyAction` 增加分支调用 `WinImageEdit::init()` |
| `Native/Src/Tray.cpp` | 修改 | `MenuCmd` 增加 `kCmdMakeCollage`（紧随 `kCmdQuickTranslate`）；`kMenuRows` 在 `kCmdQuickTranslate` 之后、分隔行之前插入该行；`runTrayCommand` 增加分支 |
| `Native/Src/Tool/IconCodes.h` | 修改 | 新增 `Split = L"\ue68b"`、`Layout = L"\ue689"`、`Dimension01 = L"\ue68a"` |
| `Native/Src/Res/SnowAirIcons.ttf` | 替换 | 用 `font_5228143_0xa8f7ae9okk/iconfont.ttf` 覆盖（新字库为原字库超集，已核对 `iconfont.json` 含全部旧码位） |
| `Native/Src/Res/lang.zh-CN.json` / `lang.en-US.json` | 修改 | 新增托盘文案、热键文案、窗口标题、模式名、取消/保存、空态提示 |
| `Native/Src/SnowAir.vcxproj` | 修改 | 登记 `Win\WinImageEdit.cpp` 与 `.h` |

### 3.2 窗口与布局

- `WinImageEdit` 继承 `Ling::WinBase`，构造中 `setSize(1159, 807)`、`setCenter()`、`setMinSize(1000, 700)`，窗口样式 `WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN`，保留系统原生标题按钮。
- `onCreated()`：`enableShadow()`；窗口固定深色，`DwmSetWindowAttribute(hwnd, 20, TRUE)` 将原生标题栏切为深色。
- 颜色常量固定为本功能设计值，不随设置中心浅/深主题变化：
  - 窗口底 `#262626`；画布 `#171717`；主文字 `#FAFAFA`；次级文字 `#A3A3A3`；
  - 胶囊轨道 `#232323`；选中块 `#FAFAFA` + 文字 `#171717`；
  - 取消按钮 `#171717`（文字 `#FAFAFA`），保存按钮 `#FAFAFA`（文字 `#171717`）；圆角 10。
- `body` 为横向容器，内边距：左 19、上 19、右 16、下 19。
- 左列：宽度自适应，`marginRight 18`；右列：固定宽 206。
- 左列纵向结构：
  - 画布宿主：`bg #171717`、圆角 20、`flexGrow 1`；
  - 拼图模式：画布宿主下方 20 间距处放素材条占位（高 78）；合并文档模式：素材条隐藏，画布宿主直接铺到底。
- 画布宿主内部：
  - 全尺寸 `Ling::Canvas` 作为后续自绘表面；
  - 空态提示 `Ling::Label` 居中显示（拼图/合并各一句）；
  - 底部工具栏：绝对定位 `left 80 / bottom 19 / 740×42`，背景 `#262626`、圆角 10；仅在拼图模式显示；
  - 顶部悬浮胶囊（M2 接入，M1 创建但隐藏）：绝对定位 `left 388 / top 22 / 124×36`。
- 右列：
  - 模式胶囊轨道 206×32（圆角 10），两个等宽 102×32 按钮；
  - 尺寸行：两个 `Ling::TextBox` 86×28（高、宽），中间 30 宽互换按钮；间距与面板对齐；
  - 模式属性区：M1 为占位容器，M2/M3 分别填充；
  - 底部按钮行：取消 70×32、间距 16、保存 70×32，右对齐。

### 3.3 模式切换

- 模式枚举 `Mode::Collage`（默认）与 `Mode::DocMerge`。
- 无内容时直接切换；有内容时 `SettingUi::ConfirmDialog::ask` 提示“切换模式会清空当前内容”，确认后清空并切换。
- M1 `hasContent()` 恒为 `false`，但保留判定入口，M2/M3 接入实际状态。

### 3.4 入口

- 热键：`Native/Src/Win/WinSettingHotkeys.cpp` 的「其他」组插入 `{ L"make_collage", Lang::get(L"shortcut.make_collage") }`，位置紧跟 `show_main`；默认键位为空（`defaultShortcutOf` 不返回键位）。
- `Setting.cpp`：`kHotkeys` 追加 msgId 113；`runHotkeyAction` 增加 `if (id == L"make_collage") { WinImageEdit::init(); return; }`。
- 托盘：`kCmdMakeCollage` 紧随 `kCmdQuickTranslate`；`kMenuRows` 在「快捷翻译」行之后插入 `{ kCmdMakeCollage, L"tray.makeCollage", L"make_collage" }`；`runTrayCommand` 调 `WinImageEdit::init()`。
- 单实例：`init()` 已存在则恢复/前置，不重复建窗；关闭时沿用 `WinSetting` 的延迟析构模式，窗口全关后释放 D2D 设备。

### 3.5 语言文案键

| 键 | zh-CN | en-US |
|---|---|---|
| `imageEdit.title` | 图片编辑 | Image Editor |
| `imageEdit.modeCollage` | 制作拼图 | Collage |
| `imageEdit.modeDoc` | 合并文档 | Merge Document |
| `imageEdit.cancel` | 取消 | Cancel |
| `imageEdit.save` | 保存 | Save |
| `imageEdit.emptyCollage` | 选择布局或把图片拖入画布开始制作 | Pick a layout or drop images onto the canvas |
| `imageEdit.emptyDoc` | 把图片拖入工作区，或点击添加图片 | Drop images here, or click Add Images |
| `shortcut.make_collage` | 制作拼图 | Make Collage |
| `tray.makeCollage` | 制作拼图 | Make Collage |

---

## 4. M2 拼图模式规格

### 4.1 新增文件与状态

- 新增 `Native/Src/Win/ImageEditCollage.h/.cpp`（`Ling::Node` 派生），承载拼图状态与画布绘制：
  - 画布像素尺寸（默认 1920×1080，可通过尺寸行修改）；
  - 格子树：矩形格子 + 分割产生的两格单元；
  - 选中格子索引、当前工具（无 / 分割 / 布局）；
  - 缩放比例（0.2–10.0）与视图偏移；
  - 背景（预设色/自定义色/背景图路径）；
  - 外边框、间距、圆角数值；
  - 撤销/重做栈（状态快照）。
- 右侧拼图属性区在 `WinImageEdit` 中按 SVG 坐标补齐：外边框、间距、圆角（数值框 60×28 + 滑条 202×4）、五色色板 + 图片背景入口、三个布局卡片 59×59、行/列输入 96×28。

### 4.2 交互落点

| 交互 | 实现 |
|---|---|
| 无工具滚轮缩放 | 以鼠标位置为中心，比例显示在画布左上角，超时隐藏；范围 20%–1000% |
| 分割 | 选中格子后落线；任意角度；按住 Shift 吸附到 8 向（0/45/90/135/180/225/270/315）；松手裁开为两格 |
| 分割二次调整 | 再次选中格子并激活分割，显示已有分割线；拖动端点调整，松手重新裁切 |
| 布局卡片 | 单击应用预设；描边 `#D9D9D9` 高亮 |
| 拖出格子 | 行/列为数量上限；拖动生成对应格子；按住 Shift 锁定拖拽起始宽高比 |
| 填图 | 单击格子后选图，或资源管理器拖入；默认等比铺满并裁切；Shift 允许拉伸；双击替换 |
| 尺寸显示 | 未选格子显示画布尺寸，选中格子显示格子尺寸；单击 PX 在像素/百分比间切换 |
| 顶部胶囊 | 选中后显示；左旋/右旋、水平/垂直居中；居中图标用 `Ling::Canvas` 自绘 |
| 撤销/重做 | 工具栏右侧按钮；每次结构性变更入栈 |

### 4.3 拖放

- `WinImageEdit` 创建后调用 `DragAcceptFiles(hwnd, TRUE)`；在 `WinBase` 窗口过程未处理 `WM_DROPFILES` 的前提下，通过子类化或新增消息钩子处理，把落点换算到画布坐标。
- 拖入多张时，按落点所在格子取第一张；空白画布按顺序自动建格。

---

## 5. M3 合并文档模式规格

- 新增 `Native/Src/Win/ImageEditMerge.h/.cpp`（`Ling::Node` 派生）：
  - 维护图片路径列表与顺序；
  - 卡片 140×160、`#FAFAFA`、圆角 20，每行 5 张，横向步进 176、纵向步进 214；
  - 卡片下方显示文件名；
  - 拖拽排序，插入位置以占位间隙提示。
- 右侧属性区：外边框（数值 + 滑条）、背景色板 + 图片背景、文件格式下拉（`.PDF` / `.PPTX`）。
- 尺寸行为：`0` 表示按原图尺寸；指定尺寸时所有页面统一，图片等比缩放到尺寸内并居中。
- 导入补充入口：工作区空态提供「添加图片」，复用多选文件对话框。

---

## 6. M4 导出规格

| 输出 | 实现 |
|---|---|
| 拼图位图 | 按画布像素尺寸渲染背景、格子图片、外边框、间距、圆角；复用 `Util::saveToFile` 与 `Setting::getSaveFormat()`（PNG/JPEG/WebP/BMP） |
| PDF | 新增 `Native/Src/Export/PdfWriter.h/.cpp`；每图一页；图片 JPEG 编码后以 DCTDecode 嵌入；页面尺寸按规则计算；外边框作为页面留白 |
| PPTX | 新增 `Native/Src/Export/PptxWriter.h/.cpp`；生成 ZIP 包（`[Content_Types].xml`、`_rels`、`ppt/presentation.xml`、`slideN.xml`、媒体文件）；每图一页 |
| 保存 | `Util::getSaveFilePath`，默认桌面；文件名可自定义 |

导出规则：
- 尺寸为 0：按原图尺寸输出，不放大；
- 指定尺寸：页面统一尺寸，图片等比缩放（取最宽/最高约束）后居中，剩余区域填背景；
- 合并外边框计入页面留白，图片在留白内居中。

---

## 7. 验证步骤

### M1
1. `MSBuild Native/Src/SnowAir.vcxproj /p:Configuration=Release /p:Platform=x64` 通过。
2. 托盘右键：快捷翻译下方出现「制作拼图」，点击打开窗口且默认拼图模式。
3. 设置中心 → 全局热键 → 其他：出现「制作拼图」且位于「开关设置」之后；录制键位后按热键可打开窗口。
4. 窗口为 1159×807、深色原生标题栏；模式胶囊切换后工具栏与素材条显隐正确。
5. 回归：F1 截图、设置中心、贴图、托盘其他项正常。

### M2
1. 应用布局卡片、拖出格子、填图、分割、二次调整、缩放、背景与三个数值属性均可操作。
2. 撤销/重做可回到上一状态。
3. Shift 分割吸附与 Shift 拖出等比约束生效。

### M3
1. 多选导入、拖拽排序、格式下拉、尺寸 0 与指定尺寸均生效。
2. 切换模式在有内容时弹确认并清空。

### M4
1. 拼图导出 PNG/JPEG 与预览一致。
2. PDF 每图一页，尺寸与留白正确，系统阅读器可打开。
3. PPTX 可在 PowerPoint/WPS 中打开，每图一页。

---

## 8. 需要用户确认

1. 4 处默认取值（第 2 节）是否接受。
2. M1 使用系统原生标题栏（窗口外框 1159×807），因此客户区宽度会比 SVG 画布坐标少约 16px；是否接受该差异，或改为无边框自绘标题栏以严格对齐 SVG。
3. 拼图默认画布尺寸取 1920×1080；合并文档默认尺寸取 0（按原图）。是否接受。
