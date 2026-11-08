# 图片编辑 · 界面返工任务清单

> 依据：`界面返工-spec.md`（以 `.figma/image` 两张截图为准）。
> 约束：不新增源文件、不改 `SnowAir.vcxproj`；控件复用 `Ling::Button/TextBox/Slider/Label/Node`、
> `SettingUi::Popup/HoverTip/ConfirmDialog`、`Src/Tool/ToolCap.cpp` 的槽位建钮模式；图标只取字库。

- [x] T0 对照清单与参考图核对（截图识别：15 工具 + 3 分隔线、右栏 7 色板、3 布局卡、顶部 4 图标）
- [x] T1 `WinImageEdit.h`：加 `toolBtns_`、`toolIds_`、`activeTool_`，声明 `buildToolbar / setActiveTool / buildTopCapsule / buildFieldRow / buildPropRow`
- [x] T2 `WinImageEdit.cpp` 工具栏：740×42@(99,628) r10 #262626；15 槽位 44×42、图标 22、内边距 15；3 条分隔线 1×14.5 #333；顺序 `split,layout,|,rect,arrow,line,marker,text,number,mosaic,blur,eraser,ocr,pin,|,undo,|,redo`；选中 #34C759、常态 #FAFAFA、悬停底 #333333；`HoverTip` 工具名
- [x] T3 顶部胶囊：124×36@(407,71) r10 #232323；四按钮 31×36，`Lock/Unlock/Crop/Expand`；删除 `CenterGlyph` 自绘
- [x] T4 组合输入框 `buildFieldRow()`：86×28 / 96×28，描边 #3A3A3A r5，左标签 12px #A3A3A3，右 TextBox 透明右对齐 #FAFAFA，PX 10px
- [x] T5 属性行 `buildPropRow()`：202×28（左标签 + 右数值 + PX），下滑条 202×4 白色拇指
- [x] T6 右栏绝对定位：模式 206×32；尺寸标题 53、输入 75；外边框 131/178.5；间距 201/248.5；圆角 271/318.5；背景标题 370 + 7 色板 392；添加布局 447 + 3 卡 470.5（1 格/上下 2 格/2×2）；行/列 544；按钮 702；合并：背景 230（7 个）、文件格式 308、下拉 330
- [x] T7 `ImageEditCollage`：默认 `bgColor_ = 0xFFFFFFFF`；新增 `setToolById`/`fitToView`；素材条 108×78 r19 间距 20.5、选中 #34C759 2px；`uiHitTest` 更新
- [x] T8 `ImageEditMerge`：卡片 140×160 r20 白底 #FFFFFF，首卡 (29,29)，步进 176/214；文件名 12px #A3A3A3；默认页面白底
- [x] T9 语言键：`imageEdit.tool.*`、`imageEdit.size|border|gap|radius|background|addLayout|rows|cols|format`
- [x] T10 构建 Release x64；按 `界面返工-spec.md` 第 4 节逐项核验并回报
