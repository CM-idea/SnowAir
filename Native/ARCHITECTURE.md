# SnowAir Native — 架构速查（ARCHITECTURE）

> 本文档用于快速建立对项目结构的认知，涵盖核心机制与关键文件位置。改动代码前先读本节，再读具体文件。

## 1. 技术栈

- C++20 + Ling GUI 框架（`Native/deps/Ling`）
- Ling 基于 Windows Composition（`windows.ui.composition`）与 Yoga 布局
- 渲染管线：Direct2D

## 2. Ling 框架关键行为

### WinBase（窗口基类）

- 源文件：`deps/Ling/include/WinBase.h`、`deps/Ling/src/WinBase.cpp`
- `body` 是窗口根节点，尺寸由窗口物理像素决定
- `createNativeWindow(DWORD exStyle = NULL, DWORD style = WS_POPUP | WS_MAXIMIZEBOX | WS_MINIMIZEBOX)`
  - 创建窗口后立即执行 `body = new Node(this); winTarget.Root(body->visual); onCreated(); layout();`
  - 因此 `body` 在 `onCreated` 中已可用，可直接构建 UI
- `setPosition(int x, int y)`：接收屏幕坐标，内部 `SetWindowPos` 到屏幕坐标
- `setSize(float w, float h)`：内部 `w *= dpi; h *= dpi`（物理像素）
- `sizeChange()`：更新 w/h 后调用 `layout()`
- `x/y/w/h` 均为物理像素，坐标转换用 `ClientToScreen(win->hwnd, &pt)`

### Node（UI 节点基类）

- 源文件：`deps/Ling/include/Node.h`、`deps/Ling/src/Node.cpp`
- 持有 Yoga 节点、子节点列表与 `win` 指针
- `layout()`：`x/y` 累加祖先坐标，得到相对窗口的可见逻辑像素
- `setChild()`：把子节点挂到当前节点，注意与 `makeChild` 的区别
- `isPosIn()`：沿 parent 链累加祖先 ScrollerBox 的 `scrollY` 后判断命中

### ScrollerBox（滚动容器）

- 源文件：`deps/Ling/include/ScrollerBox.h`、`deps/Ling/src/ScrollerBox.cpp`
- `scrollY` 为物理像素滚动偏移
- 滚动只平移 content 视觉偏移：`content->visual.Offset({ 0.f, -scrollY, 0.f })`，不改变子节点布局坐标
- `getMaxScrollY()` 返回 `content->h - h`
- 构造时设置 `YGNodeStyleSetMinHeight(node, 0.f)` 与 `YGNodeStyleSetFlexShrink(node, 1.f)`

### Yoga 布局注意事项

- flex 子项默认 `min-width/min-height: auto`（min-content），会锁死最小尺寸
- 解除方式：`YGNodeStyleSetMinHeight(node, 0.f)`、`YGNodeStyleSetMinWidth(node, 0.f)`、`YGNodeSetMinContentHeight(node, 0.f)`

## 3. WinSetting 主框架（源：Src\Win\WinSetting.cpp）

- 主窗口结构：`body`(Row) → `sideNav` + `rightArea` → `card` → `contentScroll`(ScrollerBox) → `content`
- 关键代码：

```cpp
auto contentScroll = card->makeChild<Ling::ScrollerBox>();
contentScroll->setFlexGrow(1.0);
contentScroll->setHeightPercent(100.f);
YGNodeStyleSetMinWidth(contentScroll->node, 0.f);
contentScroll->content->setFlexDirection(Ling::FlexDirection::Column);
contentScroll->content->setPadding(...);
contentScroll->content->setMinHeightPercent(100.f);  // 内容不足时撑满卡片可视高度
content = contentScroll->makeChild<Ling::Node>();
content->setWidthPercent(100.f);
content->setFlexGrow(1.f);
content->setFlexDirection(Ling::FlexDirection::Column);
```

- `makeFullFill` 辅助函数：解除 flex 项目默认 min 下限，用于撑满父容器：

```cpp
auto makeFullFill = [](Ling::Node* n) {
    n->setWidthPercent(100.f);
    n->setFlexGrow(1.0);
    n->setFlexShrink(1.0);
    YGNodeStyleSetMinHeight(n->node, 0.f);
    YGNodeStyleSetMinWidth(n->node, 0.f);
    n->setFlexDirection(Ling::FlexDirection::Column);
};
```

- 子页加载：`loadPage` switch，例如 `case 1: pageQuick = new WinSettingQuickTranslate(this); adopt(pageQuick); makeFullFill(pageQuick);`

## 4. SettingUi 浮层机制（源：Src\Win\SettingWidgets.h / .cpp）

下拉与浮层统一由独立 `WS_POPUP` 顶层窗口承载。挂到宿主 `body` 下的浮层会被窗口客户区裁剪，无法超出界面或完整展示选项。

### SettingUi::Popup

- 派生自 `Ling::WinBase`，构造 `Popup(WinBase* host, float pw, float totalH)`，`dpi` 取自宿主
- 创建窗口：`createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP)`
- `onCreated()`：`body` 下建 `listBox = body->makeChild<ScrollerBox>()`，以 Absolute 铺满窗口客户区
- `box()`：返回内容容器 `ScrollerBox*`
- `setAnchor(Button* a)`：指定锚定按钮
- `place()`：定位到锚定按钮正下方；沿 anchor 的 parent 链累加祖先 `ScrollerBox` 的 `scrollY`，再经 `ClientToScreen(host->hwnd, &pt)` 转为屏幕坐标后 `setPosition(x, y)`
- `open()`：依次 `setSize → place → show`，随后订阅宿主 `onMouseDown`（点外部关闭）与 `onMouseWheel`（滚动重定位），最后 `refresh()`
- `onDismiss`：`std::function<void()>` 点外关闭回调；全局 `gPopup` 指向 `destroyPopup`，页面成员弹层用于重置对应 `unique_ptr`
- `hostWindow()`：返回宿主，供 `closePopup(host)` 判断归属
- 生命周期：`WinBase` 析构不销毁 HWND，`~Popup()` 内必须 `removeHostHooks()`，并在 `hwnd` 有效时 `close()`，否则会残留幽灵窗口。持有者是全局 `gPopup` 或页面成员 `unique_ptr`
- 基类 `onDestroy` 是 `winrt::event`（非虚函数），不要在 `Popup` 中声明同名成员函数，否则会遮蔽基类 event

### 关键状态（SettingWidgets.cpp）

```cpp
std::unique_ptr<SettingUi::Popup> gPopup;            // 全局互斥浮层（bindPopup 用）
void destroyPopup() { if (gPopup) gPopup.reset(); }  // 析构内自动 close(HWND) 并移除宿主钩子
```

### bindPopup()

- 创建下拉浮层：`gPopup = std::make_unique<SettingUi::Popup>(w, pw, totalH)`，配置 itemH / totalH / 配色 / 选项按钮
- 点选项 → 执行 `onChange` / `setValue` 后 `destroyPopup()`
- `gPopup->setAnchor(btn); gPopup->onDismiss = []{ destroyPopup(); }; gPopup->open();`

### 其他接口

- `closePopup(WinBase* win)`：`if (!win || (gPopup && gPopup->hostWindow()==win)) destroyPopup();`
- `hasPopup()`：`return gPopup != nullptr;`
- `selectDdl()` 调用 `bindPopup`；`selectRow()` 设置 `dropdownWidth(160)`

## 5. 独立弹窗参考实现（源：Src\Tool）

### ToolNestedPanel（ToolNestedPanel.h / .cpp）

- 派生自 `Ling::WinBase`，独立顶层窗口：

```cpp
ToolNestedPanel::ToolNestedPanel(ToolSub* owner) : Ling::WinBase(), owner(owner)
{
    dpi = owner->dpi;
    createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}
```

- `place()`：用 `owner->x/y`（窗口物理坐标）与锚点节点坐标计算屏幕位置
- `finishOpen()`：

```cpp
void ToolNestedPanel::finishOpen(const RECT& workArea, Ling::Button* anchor, float logicW, float logicH)
{
    if (logicH <= 0.f) logicH = btnSize + marginTop + ToolbarTheme::shadowPad;
    setSize(logicW, logicH);
    anchorBtn = anchor;
    workArea_ = workArea;
    open_ = true;
    place(workArea, anchor);
    show();
    refresh();
}
```

### ToolPicker（ToolPicker.cpp）— 坐标转换

- 锚点节点坐标 → 屏幕坐标：

```cpp
POINT center{ (LONG)(anchor->x + anchor->w * 0.5f), (LONG)(anchor->y + anchor->h * 0.5f) };
ClientToScreen(toolbar->hwnd, &center);
RECT tb{ toolbar->x, toolbar->y, toolbar->x + (LONG)toolbar->w, toolbar->y + (LONG)toolbar->h };
const RECT wa = ToolbarChrome::workAreaNear(tb);
```

## 6. 快捷翻译对话框（源：Src\Win\WinSettingQuickTranslate.cpp）

翻译引擎：微软 / 谷歌 / 有道 / 百度 / 离线（Bergamot）。

- `WinSettingQuickTranslate` 构造 → `host`（flexGrow=1, flexShrink=1, `YGNodeStyleSetMinHeight(0.f)` + `YGNodeSetMinContentHeight(0.f)`）→ `buildTranslateTab(host)`
- `buildTranslateTab` 主面板布局：`panel`（flexGrow=1, flexShrink=1, Column）→ `topBar`(ctrlH=32) + `msgWrap`（flexGrow=1, flexShrink=1, 解除 min 下限）+ `inputArea`
- `inputArea`：

```cpp
auto inputArea = panel->makeChild<Ling::Node>();
inputArea->setFlexDirection(Ling::FlexDirection::Column);
inputArea->setWidthPercent(100.f);
inputArea->setPadding(0.f, 8.f, 0.f, 0.f);
```

- 输入框结构：`inputArea` → `inputBox`（圆角容器，padding 10,8,8,8）→ `input`(TextBox, height=48→86) + `toolRow`（源语言 / 互换 / 目标语言 / 翻译按钮，marginTop=6）
- `showOptionsBox()`：页内语言下拉，`std::make_unique<SettingUi::Popup>(win, 170.f, boxH)` 独立窗口，锚定触发按钮
- `hideOptionsBox()`：`optionsPopup.reset()`（析构内自动 `close(HWND)` 并移除宿主事件钩子）
- `host` / `panel` / `msgWrap` / `inputArea` 均解除默认 min 下限（`YGNodeStyleSetMinHeight(no, 0.f)` + `YGNodeSetMinContentHeight(no, 0.f)`），窗口缩小时面板可正常收缩，输入区始终贴底完整可见
- 翻译历史存于 `Setting` 工具键 `quickTranslate/history`

## 7. 使用范围（关键文件）

`bindPopup` / `closePopup` / `hasPopup` 的使用文件：

- `WinSettingQuickTranslate.cpp` / `.h`（`optionsPopup`）
- `WinSettingAppearance.cpp` / `.h`（`selectPopup` / `tbColorPopup`）
- `WinSettingFeatures.cpp`（析构中调用 `SettingUi::closePopup(win)`）
- `SettingWidgets.h` / `.cpp`（定义 `Popup` / `bindPopup` / `closePopup` / `hasPopup`）

## 8. 启动行为与截图触发

源码位置：`Src\main.cpp`、`Src\App.cpp`、`Src\Tray.cpp`、`Src\Setting.cpp::initShortcutKeys()`。

程序启动只挂托盘图标待命，不创建窗口，也不初始化图形设备。

| 触发方式 | 行为 | 位置 |
|---|---|---|
| 直接运行 exe（无参数） | 仅托盘待命，同时 `Update::checkLater()` | `App::App()` |
| `--auto-start`（开机自启）/ 升级后重启 / `--enter=tray` | 同上，仅托盘待命 | `App::App()` |
| `--enter=pin\|long\|video\|ocr\|qr` | 直接进入对应模式（`WinCap::init()` → `enterByArg()`） | `App::App()` / `WinCap::enterByArg()` |
| `--auto-quit=true` | 立即进入截图、截完即退（脚本 / 命令行用） | `App::App()` / `WinCap`、`WinPin` |
| 托盘图标**左键单击** | 按设置 `Setting::getTrayClickAction()`：0 截图 / 1 打开设置 / 2 无操作 | `Tray::Tray()` |
| 托盘图标**右键** | 弹出菜单：截图 / 截取全屏 / 屏幕录制 / 演示画布 / 快捷翻译 / 截图历史 / 打开目录 / 设置 / 退出（见第 9 节） | `Tray::onTrayRightClick()` |
| 全局热键（默认 `Ctrl+Alt+A`） | 进入截图 | `Setting::initShortcutKeys()` |
| **再次运行 exe**（已有实例在托盘） | 打开设置中心（`WinSetting::init()`） | `Setting::initShortcutKeys()` 的 `onSecondInstance` |

- `main.cpp` 不调用 `WinSetting::init()`：启动时不弹设置窗口，设置中心由托盘左键（设置为「打开设置」时）、托盘右键菜单或再次运行 exe 打开
- `WinCap::init()` 与 `WinSetting::init()` 都是幂等的，重复触发不会开出两份窗口

## 9. 托盘右键菜单（源：Src\Tray.cpp）

菜单项顺序固定：**截图 / 截取全屏 / 屏幕录制 / 演示画布 ┊ 快捷翻译 ┊ 截图历史 / 打开目录 ┊ 设置 / 退出**（`kMenuRows[]`，文案在语言文件 `tray.*`）

| 菜单项 | 行为 | 入口 |
|---|---|---|
| 截图 | 进截图态（悬停找元素 → 框选 → 截图工具栏） | `WinCap::init()` |
| 截取全屏 | 静默整屏抓图：存 `图片\SnowAir\<全屏模板>.png` → 剪贴板（勾选「以文件形式复制」则复制文件）→ 记录一条历史 | `App::captureFullScreenSilent()` |
| 屏幕录制 | 进入截图态（找元素 → 框选），框完直接切换为录屏工具栏 | `WinCap::beginRecordPick()`（内部 `pendingRecord`，在 `WinCap::onUp` 的 Select→Adjust 分支生效） |
| 演示画布 | 整屏作为画布：选区自动铺满，并按「功能设置 → 演示 → 默认工具」选好画笔（0 渐隐画笔 / 1 画笔 / 2 无） | `WinCap::beginDemoCanvas()`（`selectFullScreenRect()` + `startAnnotate(L"pen")`） |
| 快捷翻译 / 截图历史 / 设置 | 打开设置中心并切到对应页（1 / 2 / 0） | `WinSetting::init(pageIndex)` / `WinSetting::showPage()` |
| 打开目录 | 打开 `图片\SnowAir` | `Util::outputDir()` + `ShellExecute` |
| 退出 | 退出程序 | `Ling::App::quit(0)` |

实现要点：

- 观感完全跟随系统菜单：自绘（`MF_OWNERDRAW` + `WM_MEASUREITEM` / `WM_DRAWITEM`）只控制整体宽度 180（`kMenuW`，itemWidth = 180 − 系统菜单窗自带的 20）与文字左内缩（`kItemPadX` 10）。行高取 `GetSystemMetrics(SM_CYMENU)`，配色取 `GetSysColor(COLOR_MENU / MENUTEXT / HIGHLIGHT / HIGHLIGHTTEXT / 3DSHADOW)`，高亮为整行实心，分割线为 1px 系统色线
- 菜单宿主是自己的隐藏顶层窗口（类名 `SnowAirTrayMenuOwner`，`ensureMenuOwner()`）：Ling 的消息窗口是 message-only，跨进程不可枚举，不适合作为菜单宿主；`WM_MEASUREITEM` / `WM_DRAWITEM` 都由它接收
- 尺寸通过 `AppendMenu(..., (LPCWSTR)&MEASUREITEMSTRUCT)` 直接给出，`WM_MEASUREITEM` 分支仅作兜底。`g_measures` 必须先 `reserve`，指针需存活到菜单销毁。勾选列用 `MNS_NOCHECK` 去掉，底色用 `SetMenuInfo(MIM_BACKGROUND)`
- 右键在抬起时弹出菜单，按下即弹会被同一次抬键点掉
- 不使用 `AttachThreadInput` 抢前台：把本进程输入队列挂到光标下窗口（如 explorer）所在线程后，菜单开启期间任务栏无法点击。当前只做 `SetForegroundWindow`（失败不处理），代价是键盘（Esc / 方向键）不保证可操作菜单，鼠标点击与点菜单外关闭由系统菜单自身保证
- 静默整屏截图：`App::captureFullScreenSilent()` 使用 `Util::captureScreen()` + `Util::buildSavePath()`（支持 `{{YYYY}}` 等时间标记展开，根目录 `图片\SnowAir`）+ `Setting::addHistoryItem()`；`Util::saveToFile()` 目前只做 PNG 编码，因此固定 `.png`

## 10. 演示模式 / 全屏画布（源：Src\Win\WinCap.cpp + Src\Tool\ToolCap.cpp）

托盘「演示画布」进入全屏画布模式，三项关键要求：

| 要求 | 实现 |
|---|---|
| 画面实时 | `demoMode=true` + `hideScreenImg=true`：不绘制会话开始时的静帧；`WinCap::layout()` 在演示模式下跳过整个 cutMask 绘制（不压暗 / 不描边 / 不控点 / 不标签），桌面从 DComp 窗口的透明像素直接透出 |
| 无框选 | 选区仍设为整屏（`maskRect = 整屏`，标注引擎需要 `hasRect()`），但不绘制任何像素；不进入 `CapStage::Select`，`stage` 直接为 `Adjust` |
| 工具栏默认顶部居中 | `WinCap::layoutTool()` 中演示模式单独分支：`x = 窗口x + (w - 工具条宽)/2`、`y = 窗口y + 12dpi`；只对主工具条生效（`tool != toolSub`），属性栏仍挂在主栏下方 |

工具条槽位与图标见 `ToolCap::applyDemoLayout()`：

```
矩形 / 箭头 / 画笔 / 文字 / 序号 / 马赛克 / 橡皮擦 / 渐隐画笔 ┊ 撤销 ┊ 重置画布 / 穿透 / 取消
```

- 与截图工具栏的差别：没有椭圆，也没有贴图 / OCR / 翻译 / 长截图 / 保存 / 完成；多出渐隐画笔、重置画布、穿透
- 演示模式不弹出二级子工具气泡（马赛克与水印在布局中是独立槽位，`ToolCap::demoLayout` 直接返回）
- 默认工具由「功能设置 → 演示 → 默认工具」决定：0 渐隐画笔（默认）/ 1 画笔 / 2 无；`laser` 不是独立标注工具，实现为画笔 + `toolSub->isPenFade`，按钮选中态由 `ToolCap::applyDemoSelected()` 单独维护
- 「穿透」= `WinCap::toggleDemoThrough()` → `setMouseTransparent()`（`WS_EX_TRANSPARENT`，鼠标交给桌面，标注保留在屏幕上；工具条是独立顶层窗口，穿透时仍可点击）
- 「重置画布」= 清空 `history->shapes`；「取消」= 清空并关闭窗口

图标字体：`Src/Res/iconfont.ttf`，字码表与 `Src/Tool/IconCodes.h` 一致。

## 11. 构建输出编码

`cl.exe` 因 vcxproj 中的 `/utf-8` 会按 UTF-8 输出诊断信息，而中文 Windows 控制台默认 CP936(GBK)，中文警告会显示为乱码（不影响编译）。

控制台执行 `chcp 65001` 可修复；构建日志重定向到文件时按 UTF-8 读取，例如 `Get-Content log -Encoding UTF8`。