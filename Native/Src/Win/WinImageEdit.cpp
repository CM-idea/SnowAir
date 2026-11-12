#include "pch.h"
#include <dwmapi.h>
#include "../App.h"
#include "../Lang.h"
#include "../Util.h"
#include "../Setting.h"
#include "../Export/ImageExport.h"
#include "../Export/PdfWriter.h"
#include "../Export/PptxWriter.h"
#include "SettingWidgets.h"
#include "SettingTheme.h"
#include "ImageEditTheme.h"
#include "ImageEditCollage.h"
#include "ImageEditAnnot.h"
#include "ImageEditMerge.h"
#include "WinImageEdit.h"
#include "../Tool/IconCodes.h"
#include "../Tool/ToolSub.h"
#include <algorithm>
#include <cmath>
#include <commdlg.h>
#include <cstdlib>
#include <string>

// 截图颜色面板的自定义色块图标（定义在 ToolColorPanel.cpp）；这里只复用绘制
void PaintCustomColorSwatch(Ling::Canvas* canvas, float dpi);

namespace {
    // 自定义颜色色块：直接复用截图颜色面板那颗“圆环彩虹”图标（Canvas 每次布局时重绘）
    class ColorWheelCanvas : public Ling::Canvas
    {
    public:
        explicit ColorWheelCanvas(Ling::WinBase* w) : Canvas(w) {}
        void layout() override
        {
            Canvas::layout();
            PaintCustomColorSwatch(this, win && win->dpi > 0.f ? win->dpi : 1.f);
        }
    };

}

// 图片编辑窗口：M1 只搭骨架与入口，画布内容由 ImageEditCollage / ImageEditMerge 自绘。
// 与 WinSetting 同样的「静态实例 + 延迟析构」写法：关闭时把对象放到调度队列里 reset，
// 避免在事件派发过程中销毁正在回调的节点。

std::unique_ptr<WinImageEdit> winImageEdit;

namespace {
    unsigned long long gImageEditUid = 0;   // 每建一个图片编辑窗 +1

    // 本窗口配色取自 ImageEditTheme（跟随设置里的软件主题）
    // 背景预设色：无（0 表示透明）/ 白 / 黑 / 绿 / 蓝 / 黄
    constexpr uint32_t kBgNone{ 0x00000000u };
    const uint32_t kBgPresets[5]{ 0xFFFFFFFF, 0x0F0F0FFF, 0x00FF40FF, 0x1677FFFF, 0xFFCC00FF };

    // 保存对话框的默认文件名：与截图手动保存同一套模板（默认 SnowAir_2026-10-25_16-48-59），
    // 只取「手动保存文件名格式」的末段，不再用纯数字时间戳
    std::wstring saveDefaultName()
    {
        auto* st = Setting::get();
        std::wstring name = Util::previewFileName(
            st ? st->getManualSaveFormat() : L"", Setting::defaultManualSaveFormat(), L"");
        if (auto slash = name.find_last_of(L'/'); slash != std::wstring::npos)
            name = name.substr(slash + 1);
        return name;
    }

    // 容器（布局）尺寸 → 相对画板的百分比文本；最多一位小数，整数不带「.0」
    std::wstring percentText(int value, int base)
    {
        if (base <= 0) return L"";
        const int tenths = (int)std::lround(value * 1000.0 / base);   // 以 0.1% 为单位
        std::wstring s = std::to_wstring(tenths / 10);
        const int frac = tenths % 10;
        if (frac != 0) s += L"." + std::to_wstring(frac);
        return s;
    }

    // 延迟销毁浮层：Popup 的 open/onDismiss 都跑在事件派发链里，同步 reset 会析构正在回调的窗口
    void deferPopupReset(std::unique_ptr<SettingUi::Popup>& popup)
    {
        if (!popup) return;
        if (auto* app = Ling::App::get()) {
            auto pending = std::make_shared<std::unique_ptr<SettingUi::Popup>>();
            *pending = std::move(popup);
            app->dq.TryEnqueue([pending]() { pending->reset(); });
        } else {
            popup.reset();   // 无调度队列时兜底：同步析构（正常路径不会走到）
        }
    }

}


WinImageEdit* WinImageEdit::s_instance = nullptr;
WNDPROC WinImageEdit::s_oldProc = nullptr;

WinImageEdit::WinImageEdit() : Ling::WinBase()
{
    uid_ = ++gImageEditUid;
    onDestroy.add([uid = uid_]() {
        Ling::App::get()->dq.TryEnqueue([uid]() {
            // 关闭后可能已经开了新窗口，旧回调不能把新实例 reset 掉
            if (!winImageEdit || winImageEdit->uid_ != uid) return;
            winImageEdit.reset();
            // 与 WinSetting 同策略：窗口全关后把空闲的图形设备放掉，并把工作集闲页换出去
            auto* app = Ling::App::get();
            if (app && app->windows.empty()) {
                Ling::D2D::dispose();
                SetProcessWorkingSetSize(GetCurrentProcess(),
                    static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            }
            });
        });
    setTitle(Lang::get(L"imageEdit.title"));
    setSize(1159, 807);
    setCenter();
    setMinSize(1000, 700);
    // 保留系统原生标题栏（拖动 / 最小化 / 最大化 / 关闭都由系统提供）
    createNativeWindow(NULL, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN);
    s_instance = this;
}

WinImageEdit::~WinImageEdit()
{
    // 析构不在事件回调里：直接关闭并释放格式下拉
    if (formatPopup_) {
        formatPopup_->close();
        formatPopup_.reset();
    }
    if (s_instance == this) s_instance = nullptr;
}

void WinImageEdit::init()
{
    if (!winImageEdit || !winImageEdit->hwnd || !IsWindow(winImageEdit->hwnd))
        winImageEdit.reset(new WinImageEdit());

    HWND h = winImageEdit->hwnd;
    if (!h) return;
    // 恢复/显示并抢前台（SetForegroundWindow 被前台锁拒绝时借"临时置顶再取消置顶"兜底）
    ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (!SetForegroundWindow(h)) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(h);
    }
    SetFocus(h);
}

void WinImageEdit::dispose()
{
    // 先真正 DestroyWindow 再释放对象（退出时消息循环已结束，只 reset 会留下白板）
    if (winImageEdit && winImageEdit->hwnd) winImageEdit->close();
    winImageEdit.reset();
}

WinImageEdit* WinImageEdit::get()
{
    return winImageEdit.get();
}

// ---------------------------------------------------------------- 创建

void WinImageEdit::onCreated()
{
    enableShadow();

    // 窗口配色跟随「设置 → 外观 → 软件主题」，必须在构建界面之前取一次
    SettingTheme::apply(Setting::get() ? Setting::get()->getAppTheme() : 0);
    ImageEditTheme::apply();

    // 原生标题栏明暗跟随主题（Win10 1809+，属性 20 = DWMWA_USE_IMMERSIVE_DARK_MODE）
    if (hwnd) {
        BOOL dark = ImageEditTheme::dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    }

    body->setBg(ImageEditTheme::windowBg);
    body->setFlexDirection(Ling::FlexDirection::Row);
    body->setPadding(19.f, 19.f, 16.f, 19.f);   // left, top, right, bottom

    // 标注宿主：不可见窗口，承载工具属性栏与画布标注（输入由拼图画布转发）。
    // 必须在 buildUi 之前创建 —— 拼图画布在 rebuildForMode 里要拿它做标注宿主。
    annot_ = std::make_unique<ImageEditAnnotHost>(this);
    // 标注文字走自绘插入符，字符与输入法都要由本窗口转给标注宿主
    onChar.add([this](UINT ch) { if (annot_) annot_->forwardChar(ch); });
    // Delete / Backspace 都算删除：删除合并文档里框选/单击选中的图片卡片（标注模式下交给标注）
    onKeyDown.add([this](UINT key) {
        if (key != VK_DELETE && key != VK_BACK) return;
        if (annot_ && annot_->annotating()) return;
        if (mode_ != Mode::DocMerge || !merge_) return;
        if (merge_->deleteSelectedCards()) {
            updateEmptyState();
            updateSizeRow();
            refresh();
        }
        });
    onIME.add([this] { if (annot_) annot_->forwardIme(); });
    // 窗口尺寸/位置变了：属性栏跟着底部工具栏走
    onSizeChanged.add([this] { if (annot_) annot_->layoutTools(); });
    onMoved.add([this] { if (annot_) annot_->layoutTools(); });
    // 滚轮微调：鼠标停在尺寸/行列输入框或属性输入框上时，滚轮直接加减数字
    onMouseWheel.add([this](POINT pos, float space) { onWheelAdjust(pos, space); });

    buildUi();

    // 文件拖放：子类化窗口过程，转发到 onFilesDropped
    DragAcceptFiles(hwnd, TRUE);
    s_oldProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
        (LONG_PTR)&WinImageEdit::dropWndProc);
}

void WinImageEdit::buildUi()
{
    hoverTip_ = std::make_unique<SettingUi::HoverTip>(this);
    buildLeftColumn();
    buildRightColumn();
    rebuildForMode();
    applyModeUi();
    updateSizeRow();
    updateEmptyState();
}

void WinImageEdit::buildLeftColumn()
{
    leftCol_ = body->makeChild<Ling::Node>();
    leftCol_->setFlexGrow(1.f);
    leftCol_->setHeightPercent(100.f);
    leftCol_->setMarginTop(0.f);   // 客户区已不含标题栏（30px），画布顶端落在窗口 y=49
    leftCol_->setMarginRight(18.f);
    leftCol_->setFlexDirection(Ling::FlexDirection::Column);

    // 画布宿主：也是拼图/合并页面的绝对定位父级
    canvasHost_ = leftCol_->makeChild<Ling::Node>();
    canvasHost_->setBg(ImageEditTheme::canvasBg);
    canvasHost_->setBorderRadius(20.f);
    canvasHost_->setFlexGrow(1.f);
    canvasHost_->setWidthPercent(100.f);
    canvasHost_->setFlexDirection(Ling::FlexDirection::Column);
    canvasHost_->setAlignItems(Ling::Align::Center);
    canvasHost_->setJustifyContent(Ling::Justify::Center);

    // 空态一行：提示文字 + 合并模式的「添加图片」按钮（文案不再重复按钮名）
    emptyRow_ = canvasHost_->makeChild<Ling::Node>();
    emptyRow_->setFlexDirection(Ling::FlexDirection::Row);
    emptyRow_->setAlignItems(Ling::Align::Center);
    emptyRow_->setJustifyContent(Ling::Justify::Center);

    emptyLabel_ = emptyRow_->makeChild<Ling::Label>();
    emptyLabel_->setText(Lang::get(L"imageEdit.emptyCollage"));
    emptyLabel_->setFontSize(13.f);
    emptyLabel_->setColor(ImageEditTheme::fgMuted);

    // 合并模式空态的「添加图片」：跟随提示文字同一行显示（M3 使用）
    emptyAddBtn_ = emptyRow_->makeChild<Ling::Button>();
    emptyAddBtn_->setText(Lang::get(L"imageEdit.addImages"));
    emptyAddBtn_->setSize(72.f, 28.f);
    emptyAddBtn_->setMarginLeft(8.f);
    emptyAddBtn_->setFontSize(13.f);
    emptyAddBtn_->setBg(ImageEditTheme::trackBg);
    emptyAddBtn_->setBorderRadius(8.f);
    emptyAddBtn_->setColor(ImageEditTheme::fg);
    emptyAddBtn_->setHoverColor(ImageEditTheme::fg);
    emptyAddBtn_->setHoverBg(ImageEditTheme::hoverSubtle);
    emptyAddBtn_->onClick.add([this](Ling::Button*) {
        if (merge_) merge_->addImagesDialog();
        });
    emptyAddBtn_->hide();

    // 底部工具栏与顶部胶囊：结构与样式复用截图工具栏（ToolCap.cpp 的槽位数组 + Ling::Button）
    buildToolbar();
    buildTopCapsule();

    // 底部素材条：横向缩略图（M2 由 rebuildStrip 填充）
    strip_ = leftCol_->makeChild<Ling::Node>();
    strip_->setHeight(80.f);
    strip_->setMarginTop(20.f);
    strip_->setWidthPercent(100.f);
    strip_->setFlexDirection(Ling::FlexDirection::Row);
    strip_->setAlignItems(Ling::Align::Center);
    strip_->setPaddingLeft(0.f);
    strip_->setPaddingRight(0.f);
}

void WinImageEdit::buildToolbar()
{
    // 底栏不固定宽度：外层是一条贴底、左右拉满的透明轨道，用来把工具栏在画布内水平居中
    toolbarRail_ = canvasHost_->makeChild<Ling::Node>();
    toolbarRail_->setPositionType(Ling::Position::Absolute);
    toolbarRail_->setPosition(Ling::Edge::Left, 0.f);
    toolbarRail_->setPosition(Ling::Edge::Right, 0.f);
    toolbarRail_->setPosition(Ling::Edge::Bottom, 19.f);
    toolbarRail_->setHeight(42.f);
    toolbarRail_->setFlexDirection(Ling::FlexDirection::Row);
    toolbarRail_->setJustifyContent(Ling::Justify::Center);
    toolbarRail_->setAlignItems(Ling::Align::Center);

    toolbar_ = toolbarRail_->makeChild<Ling::Node>();
    toolbar_->setHeight(42.f);
    toolbar_->setBg(ImageEditTheme::windowBg);
    toolbar_->setBorderRadius(10.f);
    toolbar_->setFlexDirection(Ling::FlexDirection::Row);
    toolbar_->setJustifyContent(Ling::Justify::Start);
    toolbar_->setAlignItems(Ling::Align::Center);
    // 图标到栏边 24：按钮本体自带 4 内缩，故内边距取 20
    toolbar_->setPadding(20.f, 0.f, 20.f, 0.f);

    // 槽位顺序照截图：分割、布局 ｜ 矩形、箭头、尺寸、画笔、文字、序号、马赛克、
    // 包浆、水印、高亮、橡皮擦 ｜ 撤销 ｜ 重置（全部编辑工具都在一级栏，不再收进气泡）
    struct SlotDef { const wchar_t* id; const wchar_t* glyph; };
    static const SlotDef kSlots[]{
        { L"split",     Icon::Split },
        { L"layout",    Icon::Layout },
        { L"|",         nullptr },
        { L"rect",      Icon::Rect },
        { L"arrow",     Icon::Arrow },
        { L"dimension", Icon::Dimension01 },
        { L"pen",       Icon::Pen },
        { L"text",      Icon::Text },
        { L"number",    Icon::Number },
        { L"mosaic",    Icon::Mosaic },
        { L"patina",    Icon::Patina },
        { L"watermark", Icon::Watermark },
        { L"highlight", Icon::Highlight },
        { L"eraser",    Icon::Eraser },
        { L"|",         nullptr },
        { L"undo",      Icon::Undo },
        { L"|",         nullptr },
        { L"reset",     Icon::Redo },
    };

    for (const auto& slot : kSlots) {
        if (!slot.glyph) {
            // 分隔线：与截图工具栏同尺寸（1×18，左右各 8，竖向居中）
            auto* line = toolbar_->makeChild<Ling::Node>();
            line->setSize(1.f, 18.f);
            line->setMargin(8.f, 12.f, 8.f, 12.f);
            line->setBg(ImageEditTheme::hoverBg);
            continue;
        }

        // 与截图工具栏同一个规格：42 槽位内 34×34 停悬块（4 内缩）、圆角 8、图标 22
        auto* btn = toolbar_->makeChild<Ling::Button>();
        btn->setSize(34.f, 34.f);
        btn->setMargin(4.f);
        btn->setBg(0);
        btn->setHoverBg(ImageEditTheme::hoverBg);
        btn->setBorderRadius(8.f);
        btn->setText(slot.glyph);
        btn->setFontFamily(Icon::Family);
        btn->setFontSize(Icon::Size);
        btn->setColor(ImageEditTheme::fg);
        btn->setHoverColor(ImageEditTheme::fg);

        const int index = (int)toolBtns_.size();
        toolBtns_.push_back(btn);
        toolIds_.push_back(slot.id);
        if (std::wstring(slot.id) == L"split") splitToolIndex_ = index;

        if (std::wstring(slot.id) == L"undo") {
            btnUndo_ = btn;
        }
        else if (std::wstring(slot.id) == L"reset") {
            btnReset_ = btn;
        }

        btn->onClick.add([this, index](Ling::Button*) { setActiveTool(index); });

        if (hoverTip_ && std::wstring(slot.id) != L"undo" && std::wstring(slot.id) != L"reset") {
            // 一级工具停悬显示工具种类，不显示形态名：
            // 尺寸 → 标注，数字序号 → 序号
            const std::wstring id = slot.id;
            const wchar_t* kindKey = (id == L"dimension") ? L"tool.line"
                : (id == L"number") ? L"tool.number"
                : nullptr;
            const std::wstring key = kindKey ? std::wstring(kindKey)
                : L"imageEdit.tool." + id;
            hoverTip_->bind(btn, Lang::get(key.c_str()));
        }
    }

    // 宽度随内容自适应：工具槽 42（34 按钮 + 4×2 边距），分隔线槽 17（1 + 8×2），
    // 左右各 20 内边距（再加按钮 4 内缩 = 图标到栏边 24）
    float contentW{ 0.f };
    for (const auto& slot : kSlots)
        contentW += slot.glyph ? 42.f : 17.f;
    toolbar_->setSize(contentW + 40.f, 42.f);
}

void WinImageEdit::applyToolColors()
{
    const int si = splitToolIndex_;
    for (int i = 0; i < (int)toolBtns_.size(); ++i) {
        auto* b = toolBtns_[i];
        if (!b) continue;
        const bool gray = (!splitAvailable_ && i == si);
        // 已选中的工具悬停也保持主题绿（和截图工具栏一致），不因悬停变白
        b->setHoverColor(gray ? ImageEditTheme::fgDisabled : (i == activeTool_ ? ImageEditTheme::accent : ImageEditTheme::fg));
        b->setColor(gray ? ImageEditTheme::fgDisabled : (i == activeTool_ ? ImageEditTheme::accent : ImageEditTheme::fg));
    }
}

void WinImageEdit::updateToolAvailability()
{
    const bool avail = (mode_ == Mode::Collage && collage_) ? collage_->canSplitSelection() : true;
    if (avail == splitAvailable_) return;
    splitAvailable_ = avail;
    if (!avail && activeTool_ == splitToolIndex_) {
        // 选中变成多选/布局组：正在用的分割工具直接收起
        activeTool_ = -1;
        if (annot_ && annot_->annotating()) annot_->deactivateTool();
        if (collage_) collage_->setToolById(L"");
    }
    applyToolColors();
    refresh();
}

void WinImageEdit::bumpSizeBox(Ling::TextBox* box, int dir)
{
    if (!box) return;
    const std::wstring t = box->getText();
    if (t.empty()) return;   // 空 = 自适应，滚轮不强制改成 1px
    const int v = std::max(1, _wtoi(t.c_str()) + dir);
    const std::wstring nv = std::to_wstring(v);
    if (t != nv) box->setText(nv);   // 触发既有 onChange：按 px 或 % 重新生效
}

void WinImageEdit::bumpCountBox(Ling::TextBox* box, int dir)
{
    if (!box) return;
    const std::wstring t = box->getText();
    const int cur = _wtoi(t.c_str());
    if (t.empty() && dir < 0) return;   // 空在 0，向下滚不再减成负数
    const int v = std::clamp(cur + dir, 0, 6);
    const std::wstring nv = std::to_wstring(v);
    if (t != nv) box->setText(nv);      // 触发行/列重建（含单容器转多格组）
}

void WinImageEdit::onWheelAdjust(POINT pos, float space)
{
    if (space == 0.f) return;
    const int dir = space > 0.f ? 1 : -1;
    // 鼠标停在属性栏的序号输入框上（未在编辑）：滚轮直接加减序号
    if (annot_ && annot_->toolSub && annot_->toolSub->wheelNumberAt(dir)) return;
    // 尺寸：高 / 宽
    if (editH_ && editH_->isPosIn(pos)) { bumpSizeBox(editH_, dir); return; }
    if (editW_ && editW_->isPosIn(pos)) { bumpSizeBox(editW_, dir); return; }
    // 行 / 列（布局数量）：已分割的容器不允许再套布局，滚轮也不响应
    const bool countLocked = collage_ && collage_->selectionIsSplitGroup();
    if (rowsBox_ && rowsBox_->isPosIn(pos)) { if (!countLocked) bumpCountBox(rowsBox_, dir); return; }
    if (colsBox_ && colsBox_->isPosIn(pos)) { if (!countLocked) bumpCountBox(colsBox_, dir); return; }
    // 边框 / 间距 / 圆角：走滑条，值变了会同步输入框并立即应用（滑条本身也支持滚轮）
    for (auto& pb : propBoxes_) {
        auto* box = pb.first;
        auto* slider = pb.second;
        if (!box || !slider) continue;
        if (box->isPosIn(pos)) {
            slider->setValue(slider->getValue() + (float)dir);
            return;
        }
    }
}

void WinImageEdit::setActiveTool(int index)
{
    if (index < 0 || index >= (int)toolIds_.size()) return;
    const std::wstring& id = toolIds_[index];
    // 多选/布局组（多格）时分割工具置灰且不可激活
    if (id == L"split" && !splitAvailable_) return;

    if (id == L"undo") {
        if (mode_ == Mode::Collage) {
            // 标注优先：正在用标注工具，或空工具下选中了某个标注时，撤销的是标注；
            // 其余情况撤销拼图编辑。少了「选中标注」这一条，二次选中后撤销会被拼图栈吃掉。
            const bool annotFirst = annot_ && annot_->canUndoAnnot()
                && (annot_->annotating() || annot_->hasAnyAnnotSelection()
                    || !collage_ || !collage_->canUndo());
            if (annotFirst) annot_->undoAnnot();
            else if (collage_) collage_->undo();
            updateHistoryButtons();
            refresh();
        }
        return;
    }
    if (id == L"reset") {
        if (mode_ == Mode::Collage) {
            // 已有布局/标注：先确认，避免误触直接清空
            const bool hasAny = (collage_ && collage_->hasContent())
                || (annot_ && annot_->hasAnyShapes());
            if (hasAny) {
                SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.resetConfirm"),
                    [this](bool ok) {
                        if (!ok) return;
                        // 确认回调在弹窗事件链里：推迟到调度队列再清空
                        if (auto* app = Ling::App::get()) {
                            app->dq.TryEnqueue([this]() {
                                if (s_instance != this) return;
                                if (annot_) annot_->clearAllShapesNow();
                                if (collage_) collage_->clear();
                                updateHistoryButtons();
                                refresh();
                                });
                        }
                    });
                return;
            }
            // 重置 = 清空拼图内容与标注（空画布时直接执行）
            if (annot_) annot_->clearAllShapesNow();
            if (collage_) collage_->clear();
            updateHistoryButtons();
            refresh();
        }
        return;
    }

    // 标注工具：单击弹出与截图一致的属性栏，并在拼图画布上标注
    const bool annotTool = (id == L"rect" || id == L"arrow" || id == L"dimension"
        || id == L"pen" || id == L"text" || id == L"number" || id == L"mosaic"
        || id == L"patina" || id == L"watermark" || id == L"highlight" || id == L"eraser");

    // 再点同一个工具 = 取消选中、收起属性栏（分割/布局同样可再次单击取消）
    if (activeTool_ == index) {
        activeTool_ = -1;
        if (annot_) annot_->deactivateTool();
        if (mode_ == Mode::Collage && collage_) collage_->setToolById(L"");
        applyToolColors();
        refresh();
        return;
    }

    activeTool_ = index;
    applyToolColors();

    if (mode_ == Mode::Collage && collage_) collage_->setToolById(id);
    if (annotTool) {
        // 滤镜（马赛克/包浆/水印）取样需要最新画板底图
        syncAnnotContent();
        if (annot_) annot_->activateTool(id);
    }
    else if (annot_) {
        annot_->deactivateTool();
    }
    refresh();
}

void WinImageEdit::exitAnnotTool()
{
    if (activeTool_ < 0 || activeTool_ >= (int)toolIds_.size()) return;
    setActiveTool(activeTool_);   // 同一工具再点一次 = 取消选中并收起属性栏
}

POINT WinImageEdit::annotClientToImage(POINT p) const
{
    if (mode_ == Mode::Collage && collage_) return collage_->clientToArtPixel(p);
    return p;
}

D2D1_POINT_2F WinImageEdit::annotImageToClient(float ax, float ay) const
{
    if (mode_ == Mode::Collage && collage_) return collage_->artPixelToClient(ax, ay);
    return D2D1::Point2F(ax, ay);
}

D2D1_RECT_F WinImageEdit::annotArtRect() const
{
    if (mode_ == Mode::Collage && collage_) return collage_->artPixelRect();
    return D2D1::RectF(0.f, 0.f, -1.f, -1.f);
}

RECT WinImageEdit::toolbarScreenRect(float* btnCenterX) const
{
    RECT rc{ 0, 0, 0, 0 };
    if (!toolbar_ || !hwnd) return rc;
    POINT tl{ (LONG)toolbar_->x, (LONG)toolbar_->y };
    POINT br{ (LONG)(toolbar_->x + toolbar_->w), (LONG)(toolbar_->y + toolbar_->h) };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    rc = RECT{ tl.x, tl.y, br.x, br.y };
    if (btnCenterX) {
        float cx = toolbar_->w * 0.5f;
        if (activeTool_ >= 0 && activeTool_ < (int)toolBtns_.size() && toolBtns_[activeTool_]) {
            auto* b = toolBtns_[activeTool_];
            cx = b->x - toolbar_->x + b->w * 0.5f;
        }
        *btnCenterX = cx;
    }
    return rc;
}

// 画板内容 → 标注宿主底图：马赛克/包浆等滤镜按画板像素取样
void WinImageEdit::syncAnnotContent()
{
    if (!annot_) return;
    if (mode_ != Mode::Collage || !collage_) {
        annot_->clearContentBitmap();
        return;
    }
    // 拖拽/缩放等高频交互期间不做整画布导出：每帧导出既卡顿，又会反复重建图片位图导致闪烁。
    // 松手后的那次内容通知会把最新画面补同步过来。
    if (collage_->isInteracting()) return;
    std::vector<BYTE> pixels;
    int w = 0, h = 0;
    if (collage_->exportPixels(pixels, w, h) && w > 0 && h > 0)
        annot_->setContentBitmap(pixels, w, h);
    else
        annot_->clearContentBitmap();
}

void WinImageEdit::buildTopCapsule()
{
    topCapsule_ = canvasHost_->makeChild<Ling::Node>();
    topCapsule_->setPositionType(Ling::Position::Absolute);
    topCapsule_->setPosition(Ling::Edge::Left, 388.f);
    topCapsule_->setPosition(Ling::Edge::Top, 22.f);
    topCapsule_->setSize(124.f, 36.f);
    topCapsule_->setBg(ImageEditTheme::navBg);
    topCapsule_->setBorderRadius(10.f);
    topCapsule_->setFlexDirection(Ling::FlexDirection::Row);
    topCapsule_->hide();

    auto makeBtn = [this](const wchar_t* glyph) {
        auto* btn = topCapsule_->makeChild<Ling::Button>();
        btn->setSize(31.f, 36.f);
        btn->setBg(0);
        btn->setHoverBg(ImageEditTheme::hoverSubtle);
        btn->setBorderRadius(8.f);
        btn->setText(glyph);
        btn->setFontFamily(Icon::Family);
        btn->setFontSize(Icon::Size);
        btn->setColor(ImageEditTheme::fg);
        btn->setHoverColor(ImageEditTheme::fg);
        capsuleBtns_.push_back(btn);
        return btn;
    };

    // 截图胶囊：锁定 / 解锁 / 分割（剪刀） / 适应画布（四向展开）
    makeBtn(Icon::Lock)->onClick.add([this](Ling::Button*) {
        capsuleLocked_ = true;
        for (int i = 0; i < (int)capsuleBtns_.size(); ++i)
            capsuleBtns_[i]->setColor(i == 0 ? ImageEditTheme::accent : ImageEditTheme::fg);
        });
    makeBtn(Icon::Unlock)->onClick.add([this](Ling::Button*) {
        capsuleLocked_ = false;
        for (auto* b : capsuleBtns_) b->setColor(ImageEditTheme::fg);
        });
    makeBtn(Icon::Crop)->onClick.add([this](Ling::Button*) {
        if (mode_ == Mode::Collage && collage_) setActiveTool(0);   // 分割槽位
        });
    makeBtn(Icon::Expand)->onClick.add([this](Ling::Button*) {
        if (mode_ == Mode::Collage && collage_) collage_->fitToView();
        });
}

void WinImageEdit::buildRightColumn()
{
    rightCol_ = body->makeChild<Ling::Node>();
    rightCol_->setWidth(206.f);
    // 高度跟随窗口：取消/保存靠右下角锚定，窗口拉高拉矮都贴着右下角。
    // 不能用 height:100%（那是 body 内容高，再叠加 marginTop 会把底边顶出 5px，
    // 按钮底部间距就比左右少了 5）；交给 flex 交叉轴拉伸，高度 = 内容高 − marginTop，
    // 底边正好落在 body 的下内边距（19）上。
    rightCol_->setMarginTop(5.f);       // 窗口 y=54（客户区不含 30px 标题栏）
    rightCol_->setFlexShrink(0.f);

    // 模式胶囊：轨道 206×32
    auto capsule = rightCol_->makeChild<Ling::Node>();
    capsule->setPositionType(Ling::Position::Absolute);
    capsule->setPosition(Ling::Edge::Left, 0.f);
    capsule->setPosition(Ling::Edge::Top, 0.f);
    capsule->setSize(206.f, 32.f);
    capsule->setBg(ImageEditTheme::navBg);
    capsule->setBorderRadius(10.f);
    capsule->setFlexDirection(Ling::FlexDirection::Row);

    auto makeModeBtn = [capsule](const std::wstring& text) {
        // 分段按钮：常态完全透明，由 updateModeCapsule 决定选中段实心底
        auto* btn = capsule->makeChild<Ling::Button>();
        btn->setFlexGrow(1.f);
        btn->setHeight(32.f);
        btn->setText(text);
        btn->setFontSize(13.f);
        btn->setBorderRadius(10.f);
        btn->setBg(0);
        btn->setHoverBg(0);
        btn->setHoverColor(ImageEditTheme::fgMuted);
        return btn;
    };

    btnModeCollage_ = makeModeBtn(Lang::get(L"imageEdit.modeCollage"));
    btnModeCollage_->onClick.add([this](Ling::Button*) { setMode(Mode::Collage); });

    btnModeDoc_ = makeModeBtn(Lang::get(L"imageEdit.modeDoc"));
    btnModeDoc_->onClick.add([this](Ling::Button*) { setMode(Mode::DocMerge); });

    // 「尺寸」标题
    auto* sizeTitle = rightCol_->makeChild<Ling::Label>();
    sizeTitle->setPositionType(Ling::Position::Absolute);
    sizeTitle->setPosition(Ling::Edge::Left, 1.f);
    sizeTitle->setPosition(Ling::Edge::Top, 46.f);
    sizeTitle->setSize(202.f, 16.f);
    sizeTitle->setText(Lang::get(L"imageEdit.size"));
    sizeTitle->setFontSize(12.f);
    sizeTitle->setColor(ImageEditTheme::sideLabel);

    // 尺寸行：高 / 互换 / 宽（组合输入框）；占位符「-」表示自动尺寸
    buildFieldRow(rightCol_, Lang::get(L"imageEdit.height"), 1.f, 75.f, 86.f, editH_,
        nullptr, &unitH_, &unitHitH_);
    buildFieldRow(rightCol_, Lang::get(L"imageEdit.width"), 117.f, 75.f, 86.f, editW_,
        nullptr, &unitW_, &unitHitW_);
    editH_->setPlaceholder(L"-");
    editW_->setPlaceholder(L"-");

    // 单击「PX」切到 %：只对容器/布局尺寸有效，画板尺寸恒为 PX
    auto toggleUnit = [this](Ling::Button*) {
        if (!collage_ || liveSizeValid_ || !collage_->hasSelection()) return;
        sizePercent_ = !sizePercent_;
        if (editH_) editH_->blur();
        if (editW_) editW_->blur();
        updateSizeRow();
        };
    if (unitHitH_) unitHitH_->onClick.add(toggleUnit);
    if (unitHitW_) unitHitW_->onClick.add(toggleUnit);

    btnSwap_ = rightCol_->makeChild<Ling::Button>();
    btnSwap_->setPositionType(Ling::Position::Absolute);
    btnSwap_->setPosition(Ling::Edge::Left, 87.f);
    btnSwap_->setPosition(Ling::Edge::Top, 75.f);
    btnSwap_->setSize(30.f, 28.f);
    btnSwap_->setBg(0);
    btnSwap_->setHoverBg(ImageEditTheme::hoverSubtle);
    btnSwap_->setBorderRadius(6.f);
    btnSwap_->setText(Icon::Swap);
    btnSwap_->setFontFamily(Icon::Family);
    btnSwap_->setFontSize(16.f);
    btnSwap_->setColor(ImageEditTheme::fgMuted);
    btnSwap_->setHoverColor(ImageEditTheme::fg);

    auto onSizeEdited = [this](Ling::TextBox*, const std::wstring&) {
        if (syncingSize_) return;   // updateSizeRow / 互换回填期间不解析
        syncCanvasSizeFromEditors();
        };
    editH_->onTextChanged.add(onSizeEdited);
    editW_->onTextChanged.add(onSizeEdited);

    btnSwap_->onClick.add([this](Ling::Button*) {
        if (!editH_ || !editW_) return;
        const std::wstring h = editH_->getText();
        const std::wstring w = editW_->getText();
        syncingSize_ = true;
        editH_->setText(w);
        editW_->setText(h);
        syncingSize_ = false;
        syncCanvasSizeFromEditors();
        });

    // 模式属性区
    buildCollageProps();
    buildDocProps();

    // 底部按钮：始终贴右下角（取消 70×32 / 保存 70×32）
    auto* btnCancel = rightCol_->makeChild<Ling::Button>();
    btnCancel->setPositionType(Ling::Position::Absolute);
    btnCancel->setPosition(Ling::Edge::Right, 88.f);
    btnCancel->setPosition(Ling::Edge::Bottom, 0.f);
    btnCancel->setSize(70.f, 32.f);
    btnCancel->setBg(ImageEditTheme::trackBg);
    btnCancel->setHoverBg(ImageEditTheme::hoverSubtle);
    btnCancel->setBorderRadius(10.f);
    btnCancel->setText(Lang::get(L"imageEdit.cancel"));
    btnCancel->setFontSize(13.f);
    btnCancel->setColor(ImageEditTheme::fgMuted);
    btnCancel->setHoverColor(ImageEditTheme::sideLabel);
    btnCancel->onClick.add([this](Ling::Button*) { onCancelClicked(); });

    auto* btnSave = rightCol_->makeChild<Ling::Button>();
    btnSave->setPositionType(Ling::Position::Absolute);
    btnSave->setPosition(Ling::Edge::Right, 2.f);
    btnSave->setPosition(Ling::Edge::Bottom, 0.f);
    btnSave->setSize(70.f, 32.f);
    btnSave->setBg(ImageEditTheme::fg);
    btnSave->setHoverBg(ImageEditTheme::saveHover);
    btnSave->setBorderRadius(10.f);
    btnSave->setText(Lang::get(L"imageEdit.save"));
    btnSave->setFontSize(13.f);
    btnSave->setColor(ImageEditTheme::primaryText);
    btnSave->setHoverColor(ImageEditTheme::primaryText);
    btnSave->onClick.add([this](Ling::Button*) { onSaveClicked(); });
}

Ling::Node* WinImageEdit::buildFieldRow(Ling::Node* parent, const std::wstring& caption,
    float left, float top, float width, Ling::TextBox*& outBox, Ling::Label** outLabel,
    Ling::Label** outUnit, Ling::Button** outUnitHit)
{
    // 截图：无描边、底色 #232323、圆角 5；左「高/宽/行/列」白色，中数值，右 PX 暗白
    auto* box = parent->makeChild<Ling::Node>();
    box->setPositionType(Ling::Position::Absolute);
    box->setPosition(Ling::Edge::Left, left);
    box->setPosition(Ling::Edge::Top, top);
    box->setSize(width, 28.f);
    box->setBg(ImageEditTheme::inputBg);
    box->setBorder(0.f, ImageEditTheme::inputBg);
    box->setBorderRadius(5.f);

    auto* cap = box->makeChild<Ling::Label>();
    cap->setPositionType(Ling::Position::Absolute);
    cap->setPosition(Ling::Edge::Left, 6.f);
    cap->setPosition(Ling::Edge::Top, 6.f);
    cap->setSize(14.f, 16.f);
    cap->setText(caption);
    cap->setFontSize(12.f);
    cap->setColor(ImageEditTheme::fg);
    if (outLabel) *outLabel = cap;

    auto* unit = box->makeChild<Ling::Label>();
    unit->setPositionType(Ling::Position::Absolute);
    unit->setPosition(Ling::Edge::Right, 4.f);
    unit->setPosition(Ling::Edge::Top, 7.f);
    unit->setSize(14.f, 14.f);
    unit->setText(L"PX");
    unit->setFontSize(10.f);
    unit->setColor(ImageEditTheme::unitDim);
    if (outUnit) *outUnit = unit;

    // 单位区的点击热区（只有尺寸行需要：PX ↔ %），透明按钮叠在单位文字上。
    // 不设悬停底色：只靠手型光标提示可点，避免输入框里突然出现一块底色
    if (outUnitHit) {
        auto* hit = box->makeChild<Ling::Button>();
        hit->setPositionType(Ling::Position::Absolute);
        hit->setPosition(Ling::Edge::Right, 0.f);
        hit->setPosition(Ling::Edge::Top, 0.f);
        hit->setSize(30.f, 28.f);
        hit->setBg(0);
        hit->setHoverBg(0);
        hit->setBorderRadius(5.f);
        *outUnitHit = hit;
    }

    auto* edit = box->makeChild<Ling::TextBox>();
    edit->setPositionType(Ling::Position::Absolute);
    edit->setPosition(Ling::Edge::Left, 22.f);
    edit->setPosition(Ling::Edge::Top, 0.f);
    edit->setSize(width - 44.f, 28.f);
    edit->setBg(0);
    edit->setPadding(0.f);      // 去掉 TextBox 默认 6px 内边距，四位尺寸才放得下
    edit->setSingleLine(true);  // 回车 = 确定，不换行、不出现滚动条
    edit->setColor(ImageEditTheme::fg);
    edit->setFontSize(12.f);
    edit->setVerticalCenter(true);
    edit->setPlaceholder(L"-");
    edit->setPlaceholderColor(ImageEditTheme::placeholder);
    outBox = edit;
    return box;
}

void WinImageEdit::buildCollageProps()
{
    collageProps_ = rightCol_->makeChild<Ling::Node>();
    collageProps_->setPositionType(Ling::Position::Absolute);
    collageProps_->setPosition(Ling::Edge::Left, 0.f);
    collageProps_->setPosition(Ling::Edge::Top, 0.f);
    collageProps_->setSize(206.f, 734.f);

    // 外边框 / 间距 / 圆角：截图是「左标签 + 右数值 + PX」一行 + 细滑条
    addDivider(collageProps_, 116.f);
    buildPropRow(collageProps_, Lang::get(L"imageEdit.border"), 131.f, 0.f, 100.f, collageBorder_,
        [this](float v) { if (collage_) collage_->setBorder(v); });
    buildPropRow(collageProps_, Lang::get(L"imageEdit.gap"), 201.f, 0.f, 100.f, collageGap_,
        [this](float v) { if (collage_) collage_->setGap(v); });
    buildPropRow(collageProps_, Lang::get(L"imageEdit.radius"), 271.f, 0.f, 100.f, collageRadius_,
        [this](float v) { if (collage_) collage_->setRadius(v); });
    addDivider(collageProps_, 346.f);

    // 「背景」标题 + 色板（无色 / 白 / 黑 / 绿 / 蓝 / 黄 / 自定义）
    auto* bgTitle = collageProps_->makeChild<Ling::Label>();
    bgTitle->setPositionType(Ling::Position::Absolute);
    bgTitle->setPosition(Ling::Edge::Left, 1.f);
    bgTitle->setPosition(Ling::Edge::Top, 360.f);
    bgTitle->setSize(202.f, 16.f);
    bgTitle->setText(Lang::get(L"imageEdit.background"));
    bgTitle->setFontSize(12.f);
    bgTitle->setColor(ImageEditTheme::sideLabel);

    buildSwatchRow(collageProps_, 392.f, bgSwatchCollage_,
        [this](uint32_t color) {
            if (!collage_) return;
            collage_->setBackgroundColor(color);
            refresh();
        });
    addDivider(collageProps_, 426.f);

    // 「添加布局」标题 + 3 张 60×60 卡片
    auto* layoutTitle = collageProps_->makeChild<Ling::Label>();
    layoutTitle->setPositionType(Ling::Position::Absolute);
    layoutTitle->setPosition(Ling::Edge::Left, 1.f);
    layoutTitle->setPosition(Ling::Edge::Top, 440.f);
    layoutTitle->setSize(202.f, 16.f);
    layoutTitle->setText(Lang::get(L"imageEdit.addLayout"));
    layoutTitle->setFontSize(12.f);
    layoutTitle->setColor(ImageEditTheme::sideLabel);

    auto* layoutRow = collageProps_->makeChild<Ling::Node>();
    layoutRow->setPositionType(Ling::Position::Absolute);
    layoutRow->setPosition(Ling::Edge::Left, 1.f);
    layoutRow->setPosition(Ling::Edge::Top, 470.5f);
    layoutRow->setSize(202.f, 60.f);
    layoutRow->setFlexDirection(Ling::FlexDirection::Row);
    for (int i = 0; i < 3; ++i) layoutCards_.push_back(buildLayoutCard(layoutRow, i));

    // 行 / 列（随布局类型改成 上/下 或 左/右）
    buildFieldRow(collageProps_, Lang::get(L"imageEdit.rows"), 1.f, 544.f, 96.f, rowsBox_, &rowsLabel_);
    buildFieldRow(collageProps_, Lang::get(L"imageEdit.cols"), 107.f, 544.f, 96.f, colsBox_, &colsLabel_);

    auto onCountEdited = [this](Ling::TextBox*, const std::wstring&) {
        if (syncingProps_ || !collage_ || !rowsBox_ || !colsBox_) return;
        // 已分割的容器：布局数量输入框置灰禁用，不接受改数字
        if (collage_->selectionIsSplitGroup()) return;
        const int a = std::clamp(_wtoi(rowsBox_->getText().c_str()), 0, 6);
        const int b = std::clamp(_wtoi(colsBox_->getText().c_str()), 0, 6);
        collage_->setRowsCols(a, b);
        collage_->applyLayoutToSelection();   // 有选中容器：立即按新数量重排
        refresh();
        };
    rowsBox_->onTextChanged.add(onCountEdited);
    colsBox_->onTextChanged.add(onCountEdited);

    syncCollageProps();
}

Ling::Node* WinImageEdit::buildPropRow(Ling::Node* parent, const std::wstring& label, float top,
    float minV, float maxV, NumProp& out, std::function<void(float)> apply)
{
    auto* block = parent->makeChild<Ling::Node>();
    block->setPositionType(Ling::Position::Absolute);
    block->setPosition(Ling::Edge::Left, 1.f);
    block->setPosition(Ling::Edge::Top, top);
    block->setSize(202.f, 68.f);

    auto* row = block->makeChild<Ling::Node>();
    row->setSize(202.f, 28.f);
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setAlignItems(Ling::Align::Center);

    auto* lbl = row->makeChild<Ling::Label>();
    lbl->setText(label);
    lbl->setFlexGrow(1.f);
    lbl->setFontSize(12.f);
    lbl->setColor(ImageEditTheme::sideLabel);

    // 数值框：60×28、圆角 5，数值靠左、PX 贴右
    auto* valueBox = row->makeChild<Ling::Node>();
    valueBox->setSize(60.f, 28.f);
    valueBox->setBg(ImageEditTheme::inputBg);
    valueBox->setBorder(0.f, ImageEditTheme::inputBg);
    valueBox->setBorderRadius(5.f);

    out.box = valueBox->makeChild<Ling::TextBox>();
    out.box->setPositionType(Ling::Position::Absolute);
    out.box->setPosition(Ling::Edge::Left, 8.f);
    out.box->setPosition(Ling::Edge::Top, 0.f);
    out.box->setSize(34.f, 28.f);
    out.box->setBg(0);
    out.box->setPadding(0.f);
    out.box->setSingleLine(true);   // 回车 = 确定；不换行/不出滚动条
    out.box->setColor(ImageEditTheme::fg);
    out.box->setFontSize(12.f);
    out.box->setVerticalCenter(true);
    out.box->setPlaceholder(L"0");
    out.box->setPlaceholderColor(ImageEditTheme::placeholder);

    auto* unit = valueBox->makeChild<Ling::Label>();
    unit->setPositionType(Ling::Position::Absolute);
    unit->setPosition(Ling::Edge::Right, 4.f);
    unit->setPosition(Ling::Edge::Top, 7.f);
    unit->setSize(14.f, 14.f);
    unit->setText(L"PX");
    unit->setFontSize(10.f);
    unit->setColor(ImageEditTheme::unitDim);

    out.slider = block->makeChild<Ling::Slider>();
    out.slider->setPositionType(Ling::Position::Absolute);
    out.slider->setPosition(Ling::Edge::Left, 0.f);
    out.slider->setPosition(Ling::Edge::Top, 39.5f);
    out.slider->setSize(202.f, 20.f);
    out.slider->setRange(minV, maxV);
    out.slider->setStep(1.f);
    out.slider->setTrackColor(ImageEditTheme::cardLine);
    out.slider->setFillColor(ImageEditTheme::cardLine);   // 截图左侧已选段不是白色，与底轨同色
    out.slider->setThumbColor(ImageEditTheme::sliderThumb);
    out.slider->setHoverThumbColor(ImageEditTheme::sliderThumbHover);
    out.slider->setThumbRadius(9.f);       // 截图滑块直径 18
    out.slider->setTrackFullWidth(true);   // 截图底轨铺满 202，滑块两端内缩

    out.box->onTextChanged.add([this, &out, minV, maxV, apply](Ling::TextBox*, const std::wstring& text) {
        if (syncingProps_) return;
        out.value = (float)std::clamp(_wtoi(text.c_str()), (int)minV, (int)maxV);
        apply(out.value);
        syncingProps_ = true;
        if (out.slider) out.slider->setValue(out.value);
        syncingProps_ = false;
        });

    out.slider->onValueChanged.add([this, &out, minV, maxV, apply](Ling::Slider*, float v) {
        if (syncingProps_) return;
        out.value = (float)std::clamp((int)std::lround(v), (int)minV, (int)maxV);
        apply(out.value);
        syncingProps_ = true;
        if (out.box) out.box->setText(std::to_wstring((int)out.value));
        syncingProps_ = false;
        });

    propBoxes_.emplace_back(out.box, out.slider);
    return block;
}

Ling::Node* WinImageEdit::addDivider(Ling::Node* parent, float top)
{
    auto* line = parent->makeChild<Ling::Node>();
    line->setPositionType(Ling::Position::Absolute);
    line->setPosition(Ling::Edge::Left, 1.f);
    line->setPosition(Ling::Edge::Top, top);
    line->setSize(202.f, 1.f);
    line->setBg(ImageEditTheme::divider);
    return line;
}

void WinImageEdit::buildSwatchRow(Ling::Node* parent, float top, BgSwatchRefs& out,
    std::function<void(uint32_t)> applyColor)
{
    // 背景色板：无色 / 白 / 黑 / 绿 / 蓝 / 黄 / 自定义，步进 30。
    // 选中绿圈画在普通「外环」节点上（和截图颜色面板一致），不画在 Button 上 ——
    // Button 悬停时会缓存描边颜色，创建描边时读到的是悬停前的透明色，绿圈就不显示。
    // 「无色」= 透明背景，图标取字库里的 NoColor 字形，放在白色色块前面。
    auto* noneHit = parent->makeChild<Ling::Button>();
    noneHit->setPositionType(Ling::Position::Absolute);
    noneHit->setPosition(Ling::Edge::Left, 1.f);
    noneHit->setPosition(Ling::Edge::Top, top - 2.f);
    noneHit->setSize(22.f, 22.f);
    noneHit->setBg(0);
    noneHit->setHoverBg(0);
    // 不设圆角：Button 的圆角裁剪会削掉同尺寸外环的描边
    noneHit->setBorderRadius(0.f);
    noneHit->setBorder(0.f, 0);
    noneHit->setAlignItems(Ling::Align::Center);
    noneHit->setJustifyContent(Ling::Justify::Center);
    auto* noneRing = noneHit->makeChild<Ling::Node>();
    noneRing->setSize(22.f, 22.f);
    noneRing->setBorderRadius(3.f);   // 与截图颜色面板外环一致，描边更柔和
    noneRing->setAlignItems(Ling::Align::Center);
    noneRing->setJustifyContent(Ling::Justify::Center);
    out.none = noneRing;
    // 字库里的「无色」图标：24×24 设计网格 + 24 号字，墨迹约 18 与色块同大，盒子留余量不裁切
    auto* noneIcon = noneRing->makeChild<Ling::Label>();
    noneIcon->setSize(24.f, 24.f);
    noneIcon->setFlexShrink(0.f);   // 比外环大 2px：不参与收缩，避免被压回 22 裁掉笔迹
    noneIcon->setText(Icon::NoColor);
    noneIcon->setFontFamily(Icon::Family);
    noneIcon->setFontSize(24.f);
    noneIcon->setColor(ImageEditTheme::fgMuted);
    out.noneIcon = noneIcon;
    noneHit->onClick.add([this, applyColor](Ling::Button*) {
        applyColor(kBgNone);
        updateBgSwatches();
        });

    for (int i = 0; i < 5; ++i) {
        auto* hit = parent->makeChild<Ling::Button>();
        hit->setPositionType(Ling::Position::Absolute);
        hit->setPosition(Ling::Edge::Left, 31.f + i * 30.f);
        hit->setPosition(Ling::Edge::Top, top - 2.f);
        hit->setSize(22.f, 22.f);
        hit->setBg(0);
        hit->setHoverBg(0);
        hit->setHoverColor(0);
        hit->setBorderRadius(0.f);   // 同无色：不做裁剪，外环描边不被削角
        hit->setBorder(0.f, 0);
        hit->setAlignItems(Ling::Align::Center);
        hit->setJustifyContent(Ling::Justify::Center);

        auto* ring = hit->makeChild<Ling::Node>();
        ring->setSize(22.f, 22.f);   // 外环 22 / 色块 18：描边与色块之间正好留 1px（与截图色板一致）
        ring->setBorderRadius(3.f);
        ring->setAlignItems(Ling::Align::Center);
        ring->setJustifyContent(Ling::Justify::Center);
        auto* swatch = ring->makeChild<Ling::Label>();
        swatch->setSize(18.f, 18.f);
        swatch->setBg(kBgPresets[i]);
        swatch->setBorderRadius(2.f);   // 与截图颜色面板 swatchRadius 一致
        // 白色块加一圈很淡的描边，避免和面板底色混在一起（和截图一致）
        if (kBgPresets[i] == 0xFFFFFFFF) swatch->setBorder(1.f, 0xA8A8A8FF);
        hit->onClick.add([this, applyColor, color = kBgPresets[i]](Ling::Button*) {
            applyColor(color);
            updateBgSwatches();
            });
        out.rings.push_back(ring);
    }

    // 自定义颜色：截图颜色面板那颗「彩虹渐变」色块（Canvas 自绘）
    auto* customHit = parent->makeChild<Ling::Button>();
    customHit->setPositionType(Ling::Position::Absolute);
    customHit->setPosition(Ling::Edge::Left, 181.f);
    customHit->setPosition(Ling::Edge::Top, top - 2.f);
    customHit->setSize(22.f, 22.f);
    customHit->setBg(0);
    customHit->setHoverBg(0);
    customHit->setBorderRadius(0.f);   // 同无色：不做裁剪，外环描边不被削角
    customHit->setBorder(0.f, 0);
    customHit->setJustifyContent(Ling::Justify::Center);
    customHit->setAlignItems(Ling::Align::Center);
    auto* customRing = customHit->makeChild<Ling::Node>();
    customRing->setSize(22.f, 22.f);
    customRing->setBorderRadius(3.f);   // 与截图颜色面板外环一致
    customRing->setJustifyContent(Ling::Justify::Center);
    customRing->setAlignItems(Ling::Align::Center);
    out.custom = customRing;
    // 自定义色块与截图颜色面板一致：圆环彩虹图标
    auto* rainbow = customRing->makeChild<ColorWheelCanvas>();
    rainbow->setSize(18.f, 18.f);
    rainbow->setBorderRadius(2.f);
    customHit->onClick.add([this, applyColor](Ling::Button*) {
        if (!hwnd) return;
        // 初始颜色与截图属性栏的自定义颜色保持一致
        uint32_t cur = 0xFF272CFF;
        if (annot_ && annot_->toolSub) {
            const auto& pal = annot_->toolSub->colorPalette();
            const size_t idx = annot_->toolSub->customColorIdx();
            if (idx < pal.size()) cur = pal[idx];
        }
        static COLORREF customColors[16]{};
        CHOOSECOLORW cc{ sizeof(CHOOSECOLORW) };
        cc.hwndOwner = hwnd;
        cc.lpCustColors = customColors;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        cc.rgbResult = RGB((cur >> 24) & 0xFF, (cur >> 16) & 0xFF, (cur >> 8) & 0xFF);
        if (!ChooseColorW(&cc)) return;
        const uint32_t rgba = ((uint32_t)GetRValue(cc.rgbResult) << 24)
            | ((uint32_t)GetGValue(cc.rgbResult) << 16)
            | ((uint32_t)GetBValue(cc.rgbResult) << 8) | 0xFFu;
        applyColor(rgba);
        // 同步回截图属性栏的自定义颜色
        if (annot_ && annot_->toolSub) annot_->toolSub->selectColorByValue(rgba);
        updateBgSwatches();
        });
}

Ling::Button* WinImageEdit::buildLayoutCard(Ling::Node* parent, int preset)
{
    // 三张布局卡：图标本身就是整块布局图（字库里自带外框，墨迹占满字身），
    // 所以不再给卡片额外加底/描边，只按选中态换图标颜色：选中白色，未选中深灰。
    static const wchar_t* const kGlyphs[3]{ Icon::LayoutFree, Icon::LayoutTB, Icon::LayoutLR };
    static const wchar_t* const kTipKeys[3]{
        L"imageEdit.layoutFree", L"imageEdit.layoutTopBottom", L"imageEdit.layoutLeftRight"
    };
    const bool selected = (preset == 0);

    auto* card = parent->makeChild<Ling::Button>();
    card->setSize(60.f, 60.f);
    card->setMarginRight(preset == 2 ? 0.f : 11.f);
    card->setBg(0);
    card->setHoverBg(0);
    card->setText(kGlyphs[preset]);
    card->setFontFamily(Icon::Family);
    card->setFontSize(48.f);   // 字形墨迹等于字身，48 在 60 卡内既够大也不会被裁切
    card->setColor(selected ? ImageEditTheme::fg : ImageEditTheme::cardLine);
    card->setHoverColor(ImageEditTheme::fgMuted);
    if (hoverTip_) hoverTip_->bind(card, Lang::get(kTipKeys[preset]));

    card->onClick.add([this, preset](Ling::Button*) {
        if (!collage_) return;
        collage_->applyLayoutPreset(preset);
        layoutIndex_ = preset;
        syncCollageProps();   // 字段文案 / 默认数量跟着布局类型切
        refresh();
        });

    return card;
}

void WinImageEdit::syncCollageProps()
{
    if (!collage_ || !collageBorder_.box || !collageBorder_.slider) return;

    syncingProps_ = true;
    collageBorder_.value = collage_->border();
    collageGap_.value = collage_->gap();
    collageRadius_.value = collage_->radius();

    auto syncOne = [](NumProp& p) {
        if (!p.box || !p.slider) return;
        p.box->setText(std::to_wstring((int)std::lround(p.value)));
        p.slider->setValue(p.value);
    };
    syncOne(collageBorder_);
    syncOne(collageGap_);
    syncOne(collageRadius_);

    // 选中布局组：属性区回填该组的类型与行列（改数量/类型直接作用于该组）
    int groupType = -1, groupRows = 0, groupCols = 0;
    const bool hasGroup = collage_->selectedLayoutGroupInfo(groupType, groupRows, groupCols);
    if (hasGroup) {
        layoutIndex_ = groupType;
        collage_->setLayoutType(groupType);
        collage_->setRowsCols(groupRows, groupCols);
    }
    // 自由布局且没有选中布局组：行列保持用户当前输入（不在此处强行清空）
    auto setBoxText = [](Ling::TextBox* box, const std::wstring& txt) {
        if (box && box->getText() != txt) box->setText(txt);
        };
    setBoxText(rowsBox_, collage_->rows() > 0 ? std::to_wstring(collage_->rows()) : L"");
    setBoxText(colsBox_, collage_->cols() > 0 ? std::to_wstring(collage_->cols()) : L"");
    // 字段文案随布局类型：自由=行/列，上和下=上/下，左和右=左/右
    if (rowsLabel_ && colsLabel_) {
        const wchar_t* kA = L"imageEdit.rows";
        const wchar_t* kB = L"imageEdit.cols";
        if (layoutIndex_ == 1) { kA = L"imageEdit.up"; kB = L"imageEdit.down"; }
        else if (layoutIndex_ == 2) { kA = L"imageEdit.left"; kB = L"imageEdit.right"; }
        rowsLabel_->setText(Lang::get(kA));
        colsLabel_->setText(Lang::get(kB));
    }

    // 选中已分割的容器：布局数量不可用 —— 只读，用占位符「-」表示，不置灰
    const bool splitSel = collage_->selectionIsSplitGroup();
    lastSplitSel_ = splitSel;
    auto setCountState = [splitSel](Ling::TextBox* box) {
        if (!box) return;
        box->setReadOnly(splitSel);
        box->setColor(ImageEditTheme::fg);
        if (!splitSel) return;
        box->blur();
        if (!box->getText().empty()) box->setText(L"");   // 空文本 → 显示占位符「-」
    };
    setCountState(rowsBox_);
    setCountState(colsBox_);

    syncingProps_ = false;

    for (int i = 0; i < (int)layoutCards_.size(); ++i)
        layoutCards_[i]->setColor(i == layoutIndex_ ? ImageEditTheme::fg : ImageEditTheme::cardLine);
    updateBgSwatches();
}

// 背景色板选中态：当前背景色对应的色块加一圈绿色（无背景选「无色」）
void WinImageEdit::updateBgSwatches()
{
    const bool collageMode = (mode_ == Mode::Collage);
    BgSwatchRefs& refs = collageMode ? bgSwatchCollage_ : bgSwatchMerge_;
    uint32_t cur = 0xFFFFFFFF;
    if (collageMode) {
        if (!collage_) return;
        cur = collage_->backgroundColor();
    }
    else {
        if (!merge_) return;
        cur = merge_->backgroundColor();
    }
    const bool noBg = (cur == kBgNone);
    bool isPreset = false;
    for (size_t i = 0; i < refs.rings.size() && i < 5; ++i) {
        const bool on = !noBg && kBgPresets[i] == cur;
        if (on) isPreset = true;
        if (refs.rings[i]) refs.rings[i]->setBorder(on ? 1.f : 0.f, Ling::Color(on ? ImageEditTheme::accent : (uint32_t)0));
    }
    // 「无色」的选中态不用描边：图标本身变绿（选中绿）即可
    if (refs.none) refs.none->setBorder(0.f, 0);
    if (refs.noneIcon) refs.noneIcon->setColor(Ling::Color(noBg ? ImageEditTheme::accent : (uint32_t)ImageEditTheme::fgMuted));
    if (refs.custom)
        refs.custom->setBorder((!noBg && !isPreset) ? 1.f : 0.f, Ling::Color((!noBg && !isPreset) ? ImageEditTheme::accent : (uint32_t)0));
}

void WinImageEdit::rebuildStrip()
{
    if (!strip_ || !collage_) return;
    const auto thumbs = collage_->thumbnails();

    // 图片集合没变（拖动分割线等高频刷新）就不重建节点，只更新选中描边、
    // 并刷新索引缓存。否则底部图片格子会每帧销毁重建，表现为一直闪烁。
    bool same = (thumbs.size() == stripItems_.size());
    if (same) {
        for (size_t i = 0; i < thumbs.size(); ++i) {
            if (thumbs[i].path != stripPaths_[i]) { same = false; break; }
        }
    }
    if (same) {
        stripCells_.clear();
        for (size_t i = 0; i < thumbs.size(); ++i) {
            stripCells_.push_back(thumbs[i].cell);
            if (stripItems_[i]) stripItems_[i]->setBorder(2.f, thumbs[i].selected ? ImageEditTheme::accent : ImageEditTheme::cardLine);
        }
        return;
    }

    strip_->removeAllChildren();
    stripItems_.clear();
    stripCells_.clear();
    stripPaths_.clear();

    const float d = (dpi > 0.f) ? dpi : 1.f;
    for (const auto& thumb : thumbs) {
        auto* item = strip_->makeChild<Ling::Button>();
        item->setSize(80.f, 80.f);   // 1:1 方形格子
        item->setBg(ImageEditTheme::canvasBg);
        item->setHoverBg(ImageEditTheme::hoverSubtle);
        item->setBorderRadius(20.f);
        item->setBorder(2.f, thumb.selected ? ImageEditTheme::accent : ImageEditTheme::cardLine);
        item->setMarginRight(14.f);

        auto* img = item->makeChild<Ling::ImageBox>();
        img->setPositionType(Ling::Position::Absolute);
        img->setPosition(Ling::Edge::Left, 2.f);
        img->setPosition(Ling::Edge::Top, 2.f);
        img->setSize(76.f, 76.f);
        img->loadImg(thumb.path, 76.f * d, 76.f * d);

        const std::wstring path = thumb.path;
        // 悬停出现的「+」：单击直接替换这张图片
        auto* plus = item->makeChild<Ling::Button>();
        plus->setPositionType(Ling::Position::Absolute);
        plus->setPosition(Ling::Edge::Left, 26.f);
        plus->setPosition(Ling::Edge::Top, 26.f);
        plus->setSize(28.f, 28.f);
        plus->setBorderRadius(14.f);
        plus->setBg(0x000000CCu);
        plus->setHoverBg(ImageEditTheme::accent);
        plus->setText(L"+");
        plus->setFontSize(16.f);
        plus->setColor(0xFFFFFFFF);   // 浮在缩略图上的黑色圆底，两套主题都用白字
        plus->setHoverColor(0x171717FF);
        plus->hide();
        item->onEnter.add([plus](Ling::Button*) { plus->show(); });
        item->onLeave.add([plus](Ling::Button*) { plus->hide(); });

        item->onClick.add([this, path, plus](Ling::Button*) {
            if (!collage_) return;
            // 分割/重排后容器索引会变：按图片路径找回当前索引
            int cell = -1;
            for (const auto& t : collage_->thumbnails()) {
                if (t.path == path) { cell = t.cell; break; }
            }
            if (cell < 0) return;
            // 点在「+」上 = 直接替换；双击整块也替换；单击 = 选中
            POINT cp{};
            GetCursorPos(&cp);
            if (hwnd) ScreenToClient(hwnd, &cp);
            if (plus->isPosIn(cp)) {
                collage_->replaceCellImage(cell);
                return;
            }
            const DWORD now = GetTickCount();
            const bool dbl = lastStripClickCell_ == cell
                && (now - lastStripClickTick_) < GetDoubleClickTime();
            lastStripClickCell_ = cell;
            lastStripClickTick_ = now;
            if (dbl) {
                lastStripClickCell_ = -1;
                collage_->replaceCellImage(cell);
                return;
            }
            collage_->selectCell(cell);
            });

        stripItems_.push_back(item);
        stripCells_.push_back(thumb.cell);
        stripPaths_.push_back(path);
    }
}

void WinImageEdit::updateStripSelection()
{
    if (!collage_) return;
    auto thumbs = collage_->thumbnails();
    if (thumbs.size() != stripItems_.size()) {
        rebuildStrip();
        return;
    }
    for (size_t i = 0; i < thumbs.size(); ++i) {
        if (!stripItems_[i]) continue;
        stripItems_[i]->setBorder(2.f, thumbs[i].selected ? ImageEditTheme::accent : ImageEditTheme::cardLine);
    }
}

void WinImageEdit::updateCapsule()
{
    // 顶部胶囊只是早期示意，不再显示
    if (topCapsule_) topCapsule_->hide();
}

void WinImageEdit::updateHistoryButtons()
{
    if (mode_ != Mode::Collage || !collage_) return;
    const bool canUndo = collage_->canUndo() || (annot_ && annot_->canUndoAnnot());
    if (btnUndo_) btnUndo_->setColor(canUndo ? ImageEditTheme::fg : ImageEditTheme::fgDisabled);
    // 重置：画布为空且没有标注时置灰
    const bool hasAny = collage_->hasContent() || (annot_ && annot_->hasAnyShapes());
    if (btnReset_) btnReset_->setColor(hasAny ? ImageEditTheme::fg : ImageEditTheme::fgDisabled);
}

void WinImageEdit::buildDocProps()
{
    docProps_ = rightCol_->makeChild<Ling::Node>();
    docProps_->setPositionType(Ling::Position::Absolute);
    docProps_->setPosition(Ling::Edge::Left, 0.f);
    docProps_->setPosition(Ling::Edge::Top, 0.f);
    docProps_->setSize(206.f, 734.f);
    docProps_->hide();

    // 外边框（行内数值 + 滑条）
    addDivider(docProps_, 116.f);
    buildPropRow(docProps_, Lang::get(L"imageEdit.border"), 131.f, 0.f, 400.f, docBorder_,
        [this](float v) {
            if (merge_) merge_->setBorder(v);
            refresh();
            });
    addDivider(docProps_, 206.f);

    // 「背景」标题 + 色板
    auto* bgTitle = docProps_->makeChild<Ling::Label>();
    bgTitle->setPositionType(Ling::Position::Absolute);
    bgTitle->setPosition(Ling::Edge::Left, 1.f);
    bgTitle->setPosition(Ling::Edge::Top, 220.f);
    bgTitle->setSize(202.f, 16.f);
    bgTitle->setText(Lang::get(L"imageEdit.background"));
    bgTitle->setFontSize(12.f);
    bgTitle->setColor(ImageEditTheme::sideLabel);

    buildSwatchRow(docProps_, 252.f, bgSwatchMerge_,
        [this](uint32_t color) {
            if (!merge_) return;
            merge_->setBackgroundColor(color);
            refresh();
        });
    addDivider(docProps_, 286.f);

    // 「文件格式」标题 + 下拉
    auto* formatLabel = docProps_->makeChild<Ling::Label>();
    formatLabel->setPositionType(Ling::Position::Absolute);
    formatLabel->setPosition(Ling::Edge::Left, 1.f);
    formatLabel->setPosition(Ling::Edge::Top, 300.f);
    formatLabel->setSize(202.f, 16.f);
    formatLabel->setText(Lang::get(L"imageEdit.fileFormat"));
    formatLabel->setFontSize(12.f);
    formatLabel->setColor(ImageEditTheme::sideLabel);

    formatDdl_ = docProps_->makeChild<Ling::Button>();
    formatDdl_->setPositionType(Ling::Position::Absolute);
    formatDdl_->setPosition(Ling::Edge::Left, 1.f);
    formatDdl_->setPosition(Ling::Edge::Top, 330.f);
    formatDdl_->setSize(202.f, 28.f);
    formatDdl_->setBg(ImageEditTheme::inputBg);
    formatDdl_->setBorder(0.f, ImageEditTheme::inputBg);
    formatDdl_->setBorderRadius(5.f);
    formatDdl_->setHoverBg(ImageEditTheme::hoverSubtle);

    // 值用「行容器 + 自适应高度文字」竖向居中（直接给 Label 设高会按行高偏上）
    auto* valueRow = formatDdl_->makeChild<Ling::Node>();
    valueRow->setPositionType(Ling::Position::Absolute);
    valueRow->setPosition(Ling::Edge::Left, 9.f);
    valueRow->setPosition(Ling::Edge::Top, 0.f);
    valueRow->setSize(170.f, 28.f);
    valueRow->setFlexDirection(Ling::FlexDirection::Row);
    valueRow->setAlignItems(Ling::Align::Center);

    formatValue_ = valueRow->makeChild<Ling::Label>();
    formatValue_->setText(L".PDF");
    formatValue_->setFontSize(12.f);
    formatValue_->setColor(ImageEditTheme::fg);

    auto* arrow = formatDdl_->makeChild<Ling::Label>();
    arrow->setText(Icon::Dropdown);
    arrow->setFontFamily(Icon::Family);
    arrow->setFontSize(14.f);
    arrow->setColor(ImageEditTheme::fgMuted);
    arrow->setPositionType(Ling::Position::Absolute);
    arrow->setPosition(Ling::Edge::Right, 8.f);
    arrow->setPosition(Ling::Edge::Top, 0.f);
    arrow->setSize(20.f, 28.f);
    arrow->setAlignItems(Ling::Align::Center);
    arrow->setJustifyContent(Ling::Justify::Center);

    formatDdl_->onClick.add([this](Ling::Button*) { openFormatPopup(); });

    docFormat_ = 0;
    if (merge_) merge_->setFormat(0);
    syncDocProps();
}

void WinImageEdit::syncDocProps()
{
    if (!merge_) return;
    syncingProps_ = true;
    docBorder_.value = (float)std::lround(merge_->border());
    if (docBorder_.box) docBorder_.box->setText(std::to_wstring((int)docBorder_.value));
    if (docBorder_.slider) docBorder_.slider->setValue(docBorder_.value);
    syncingProps_ = false;
    docFormat_ = merge_->format();
    if (formatValue_) formatValue_->setText(docFormat_ == 0 ? L".PDF" : L".PPTX");
    updateBgSwatches();
}

void WinImageEdit::openFormatPopup()
{
    if (formatPopup_) {
        closeFormatPopup();
        return;
    }
    // 高度 = 上下内边距 4×2 + 两个 24 的选项，避免底部多出一截空白
    auto pop = std::make_unique<SettingUi::Popup>(this, 202.f, 56.f);
    SettingUi::Popup* raw = pop.get();
    formatPopup_ = std::move(pop);

    auto* box = raw->box();
    box->setBg(ImageEditTheme::inputBg);
    box->setBorderRadius(10.f);
    box->setPadding(4.f, 4.f, 4.f, 4.f);

    auto* panel = box->makeChild<Ling::Node>();
    panel->setWidthPercent(100.f);
    panel->setFlexDirection(Ling::FlexDirection::Column);

    auto addItem = [this, panel](int idx, const std::wstring& text) {
        auto* item = panel->makeChild<Ling::Button>();
        item->setHeight(24.f);
        item->setWidthPercent(100.f);
        item->setText(text);
        item->setFontSize(12.f);
        item->setColor(ImageEditTheme::fg);
        item->setHoverColor(ImageEditTheme::fg);
        item->setBg(0);
        item->setHoverBg(ImageEditTheme::hoverSubtle);
        item->setBorderRadius(6.f);
        item->setBorder(0.f, 0);
        // Button 默认居中，这里改成与触发框一致的左对齐
        item->setJustifyContent(Ling::Justify::Start);
        item->setPaddingLeft(9.f);
        item->onClick.add([this, idx](Ling::Button*) {
            docFormat_ = idx;
            if (merge_) merge_->setFormat(idx);
            if (formatValue_) formatValue_->setText(idx == 0 ? L".PDF" : L".PPTX");
            closeFormatPopup();
            });
    };
    addItem(0, L".PDF");
    addItem(1, L".PPTX");

    raw->setAnchor(formatDdl_);
    raw->onDismiss = [this]() { deferPopupReset(formatPopup_); };

    // 延迟 open：本回调运行在宿主 onClick 派发循环内，推迟到调度队列避免增删订阅崩溃
    auto* app = Ling::App::get();
    if (app) {
        app->dq.TryEnqueue([raw]() { raw->open(); });
    } else {
        raw->open();
    }
}

void WinImageEdit::closeFormatPopup()
{
    deferPopupReset(formatPopup_);
}

// ---------------------------------------------------------------- 模式

void WinImageEdit::setMode(Mode m)
{
    if (m == mode_) return;
    if (hasContent()) {
        SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.switchMode"),
            [this, m](bool ok) {
                if (ok) switchModeDeferred(m);
            });
        return;
    }
    switchModeDeferred(m);
}

void WinImageEdit::switchModeDeferred(Mode m)
{
    if (auto* app = Ling::App::get()) {
        app->dq.TryEnqueue([this, m]() {
            // 排队期间窗口可能已关闭（对象被延迟 reset）
            if (s_instance != this) return;
            if (m == mode_) return;
            mode_ = m;
            rebuildForMode();
            });
        return;
    }
    // 无调度队列时兜底
    if (m == mode_) return;
    mode_ = m;
    rebuildForMode();
}

void WinImageEdit::openImage(const std::wstring& path)
{
    if (path.empty()) return;
    if (mode_ != Mode::Collage) {
        mode_ = Mode::Collage;
        rebuildForMode();
    }
    if (!collage_) return;
    collage_->clear();
    RECT rc{};
    GetClientRect(hwnd, &rc);
    onFilesDropped({ path }, POINT{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 });
}

void WinImageEdit::rebuildForMode()
{
    if (!canvasHost_) return;

    // 换页面前先收起标注工具与属性栏（拼图节点即将被销毁）
    if (annot_) {
        annot_->deactivateTool();
        // 页面重建等于换文档：旧画布上的标注一并清掉（拼图内容本来就随页面销毁）
        annot_->clearAllShapesNow();
    }

    // 先摘掉旧页面（removeChild 会立即销毁节点，析构里会移除事件令牌）
    if (collage_) { auto* old = collage_; collage_ = nullptr; canvasHost_->removeChild(old); }
    if (merge_) { auto* old = merge_; merge_ = nullptr; canvasHost_->removeChild(old); }

    auto onSelection = [this]() {
        updateEmptyState();
        updateSizeRow();
        updateCapsule();
        updateStripSelection();
        updateHistoryButtons();
        updateToolAvailability();   // 多选/布局组时分割工具置灰
        // 选中布局组时属性区回填该组的类型/行列（只在组信息变化时同步，避免拖动时每帧重排）
        int gt = -1, gr = 0, gc = 0;
        const int key = (collage_ && collage_->selectedLayoutGroupInfo(gt, gr, gc))
            ? (gt * 100 + gr * 10 + gc) : -1;
        if (key != lastGroupKey_) {
            // 从「选中布局组」切换到没有组：行列回到自由布局默认 1×1
            if (lastGroupKey_ >= 0 && key < 0 && collage_ && collage_->layoutType() == 0)
                collage_->setRowsCols(1, 1);
            lastGroupKey_ = key;
            syncCollageProps();
        }
        // 选中/取消分割组时也要刷新属性区：行列输入框要在「-」与正常数值间切换
        const bool splitSel = collage_ && collage_->selectionIsSplitGroup();
        if (splitSel != lastSplitSel_) {
            lastSplitSel_ = splitSel;
            syncCollageProps();
        }
        refresh();
        };
    auto onContent = [this]() {
        updateEmptyState();
        updateSizeRow();
        updateCapsule();
        rebuildStrip();
        syncCollageProps();
        syncDocProps();
        updateHistoryButtons();
        updateToolAvailability();   // 布局组生成/删除后刷新分割工具可用性
        syncAnnotContent();
        refresh();
        };

    if (mode_ == Mode::Collage) {
        collage_ = canvasHost_->makeChild<ImageEditCollage>();
        collage_->setPositionType(Ling::Position::Absolute);
        collage_->setPosition(Ling::Edge::Left, 0.f);
        collage_->setPosition(Ling::Edge::Top, 0.f);
        collage_->setWidthPercent(100.f);
        collage_->setHeightPercent(100.f);
        collage_->onSelectionChanged = onSelection;
        collage_->onContentChanged = onContent;
        collage_->onZoomChanged = [this]() { refresh(); };
        collage_->onLiveSize = [this](int lw, int lh, bool live) {
            liveSizeValid_ = live;
            if (live) { liveSizeW_ = lw; liveSizeH_ = lh; }
            updateSizeRow();
            };
        collage_->onRequestExitTool = [this]() {
            if (activeTool_ >= 0) setActiveTool(activeTool_);   // 右键最后一层：收起工具/属性栏
            };
        // 标注宿主：输入转发 + 标注坐标系（空画布时画板为 0）
        collage_->setAnnotHost(annot_.get());
        // 恢复当前选中的布局类型与默认数量（无选中时只作为拖拽初始属性）
        collage_->applyLayoutPreset(layoutIndex_);
        // 点击落在浮动工具栏/胶囊上时，画布应忽略同一次广播
        collage_->uiHitTest = [this](POINT p) {
            const bool onToolbar = toolbar_ && toolbar_->visual.IsVisible() && toolbar_->isPosIn(p);
            const bool onCapsule = topCapsule_ && topCapsule_->visual.IsVisible() && topCapsule_->isPosIn(p);
            return onToolbar || onCapsule;
            };
    }
    else {
        merge_ = canvasHost_->makeChild<ImageEditMerge>();
        merge_->setPositionType(Ling::Position::Absolute);
        merge_->setPosition(Ling::Edge::Left, 0.f);
        merge_->setPosition(Ling::Edge::Top, 0.f);
        merge_->setWidthPercent(100.f);
        merge_->setHeightPercent(100.f);
        merge_->onChanged = onContent;
    }

    // 画布页面是后挂上去的（在视觉树最上层），把工具栏/胶囊/空态提示重新盖回它上面
    if (emptyRow_) canvasHost_->bringToFront(emptyRow_);
    if (toolbarRail_) canvasHost_->bringToFront(toolbarRail_);
    if (topCapsule_) canvasHost_->bringToFront(topCapsule_);

    // 新页面没有选中工具：图标恢复常态色（分割可用性按当前选中重新判断）
    activeTool_ = -1;
    splitAvailable_ = (mode_ == Mode::Collage && collage_) ? collage_->canSplitSelection() : true;
    applyToolColors();

    // 新页面的状态回填到素材条 / 属性区 / 历史按钮
    rebuildStrip();
    syncCollageProps();
    syncDocProps();
    updateHistoryButtons();
    syncAnnotContent();

    applyModeUi();
    updateEmptyState();
    updateCapsule();
    refresh();
}

void WinImageEdit::applyModeUi()
{
    const bool collageMode = (mode_ == Mode::Collage);

    if (strip_) {
        if (collageMode) strip_->show();
        else strip_->hide();
    }
    if (toolbar_) {
        if (collageMode) toolbar_->show();
        else toolbar_->hide();
    }
    if (collageProps_) {
        if (collageMode) collageProps_->show();
        else collageProps_->hide();
    }
    if (docProps_) {
        if (collageMode) docProps_->hide();
        else docProps_->show();
    }

    // 合并属性回填；离开合并模式时收回未关闭的格式下拉
    if (collageMode) {
        if (formatPopup_) closeFormatPopup();
    }
    else {
        syncDocProps();
    }

    updateModeCapsule();
    updateSizeRow();
    updateEmptyState();
    updateBgSwatches();   // 两种模式的色板各记自己的选中态
}

void WinImageEdit::updateModeCapsule()
{
    // 与设置栏的「内容导航」同一结构：一条轨道 + 若干分段，只有选中段是一块实心底，
    // 未选中段只显示文字、不给独立按钮的悬停底色。
    auto apply = [](Ling::Button* btn, bool selected) {
        if (!btn) return;
        if (selected) {
            btn->setBg(ImageEditTheme::fg);
            btn->setColor(ImageEditTheme::primaryText);
            btn->setHoverBg(ImageEditTheme::fg);
            btn->setHoverColor(ImageEditTheme::primaryText);
        }
        else {
            btn->setBg(0);   // 透明 → 露出轨道色
            btn->setColor(ImageEditTheme::fgMuted);
            btn->setHoverBg(0);
            btn->setHoverColor(ImageEditTheme::fgMuted);
        }
    };
    apply(btnModeCollage_, mode_ == Mode::Collage);
    apply(btnModeDoc_, mode_ == Mode::DocMerge);

    // 文本随语言；M1 不做运行时热切换语言，这里只保证建窗时文案正确
    if (btnModeCollage_) btnModeCollage_->setText(Lang::get(L"imageEdit.modeCollage"));
    if (btnModeDoc_) btnModeDoc_->setText(Lang::get(L"imageEdit.modeDoc"));
}

void WinImageEdit::updateSizeUnit(bool percent)
{
    if (unitShownPercent_ == percent) return;
    unitShownPercent_ = percent;
    const std::wstring text = percent ? L"%" : L"PX";
    const uint32_t color = percent ? ImageEditTheme::accent : ImageEditTheme::unitDim;
    if (unitH_) { unitH_->setText(text); unitH_->setColor(color); }
    if (unitW_) { unitW_->setText(text); unitW_->setColor(color); }
}

void WinImageEdit::updateSizeRow()
{
    if (!editH_ || !editW_) return;
    // PX 切成 % 只对「选中的容器」有效：其余状态隐藏点击热区，
    // 鼠标保持普通箭头（只有选中布局时才出现手型并可点）
    const bool unitClickable = mode_ == Mode::Collage && collage_
        && !liveSizeValid_ && collage_->hasSelection();
    for (auto* hit : { unitHitH_, unitHitW_ }) {
        if (!hit) continue;
        if (unitClickable) hit->show();
        else hit->hide();
    }
    const bool focused = editH_->isFocused() || editW_->isFocused();

    if (mode_ == Mode::Collage) {
        if (!collage_) return;   // 页面尚未建好：保持原样
        // 1) 正在拖拽布局：显示拖出的区域尺寸（画板还没生成，固定 PX）
        if (liveSizeValid_) {
            updateSizeUnit(false);
            editH_->setColor(ImageEditTheme::fg);
            editW_->setColor(ImageEditTheme::fg);
            if (focused) return;
            syncingSize_ = true;
            editH_->setText(std::to_wstring(liveSizeH_));
            editW_->setText(std::to_wstring(liveSizeW_));
            syncingSize_ = false;
            return;
        }
        // 2) 选中容器：显示容器尺寸（可单击 PX 切成 %，相对画板）
        // 3) 没选中时显示画板尺寸（固定则绿色，自适应显示「-」；画板恒为 PX）
        int sw = 0, sh = 0;
        const bool hasCell = collage_->selectedCellSize(sw, sh);
        const bool percent = hasCell && sizePercent_;
        updateSizeUnit(percent);
        if (hasCell) {
            editH_->setColor(ImageEditTheme::fg);
            editW_->setColor(ImageEditTheme::fg);
            if (focused) return;
            syncingSize_ = true;
            if (percent) {
                const int bw = collage_->canvasWidth();
                const int bh = collage_->canvasHeight();
                editH_->setText(percentText(sh, bh));
                editW_->setText(percentText(sw, bw));
            }
            else {
                editH_->setText(std::to_wstring(sh));
                editW_->setText(std::to_wstring(sw));
            }
            syncingSize_ = false;
            return;
        }
        const int mw = collage_->manualWidth();
        const int mh = collage_->manualHeight();
        // 绿色只表示「用户主动输入固定尺寸」；拖入内容自动建板仍是白色
        editH_->setColor(collage_->manualSizeEnteredH() ? ImageEditTheme::accent : ImageEditTheme::fg);
        editW_->setColor(collage_->manualSizeEnteredW() ? ImageEditTheme::accent : ImageEditTheme::fg);
        // 用户正在输入尺寸：不要用画板状态把输入框的文本改回去
        if (focused) return;
        syncingSize_ = true;
        editH_->setText(mh > 0 ? std::to_wstring(mh) : L"");
        editW_->setText(mw > 0 ? std::to_wstring(mw) : L"");
        syncingSize_ = false;
        return;
    }

    if (!merge_) return;
    updateSizeUnit(false);
    const int w = merge_->pageWidth();
    const int h = merge_->pageHeight();
    if (focused) return;
    syncingSize_ = true;
    editH_->setText(h > 0 ? std::to_wstring(h) : L"");
    editW_->setText(w > 0 ? std::to_wstring(w) : L"");
    syncingSize_ = false;
}

void WinImageEdit::updateEmptyState()
{
    if (!emptyLabel_) return;

    const std::wstring text = Lang::get(mode_ == Mode::Collage
        ? L"imageEdit.emptyCollage" : L"imageEdit.emptyDoc");
    if (emptyLabel_->getText() != text) emptyLabel_->setText(text);

    const bool empty = !hasContent();
    if (empty) emptyLabel_->show();
    else emptyLabel_->hide();

    if (emptyAddBtn_) {
        if (empty && mode_ == Mode::DocMerge) emptyAddBtn_->show();
        else emptyAddBtn_->hide();
    }
}

bool WinImageEdit::hasContent() const
{
    if (mode_ == Mode::Collage) return collage_ ? collage_->hasContent() : false;
    return merge_ ? merge_->hasContent() : false;
}

// ---------------------------------------------------------------- 尺寸

void WinImageEdit::syncCanvasSizeFromEditors()
{
    if (!editH_ || !editW_) return;
    // 选中容器且单位为 % 时，输入值按画板宽高的百分比解释
    const bool percent = (mode_ == Mode::Collage) && sizePercent_
        && collage_ && collage_->hasSelection();
    const double hRaw = percent
        ? _wtof(editH_->getText().c_str()) : (double)_wtoi(editH_->getText().c_str());
    const double wRaw = percent
        ? _wtof(editW_->getText().c_str()) : (double)_wtoi(editW_->getText().c_str());

    if (mode_ == Mode::Collage) {
        if (!collage_) return;
        // 选中容器时输入尺寸 = 改容器尺寸；没选中时才改画板（空/0 = 该轴自适应）
        if (collage_->hasSelection()) {
            int cw = (int)std::lround(wRaw);
            int ch = (int)std::lround(hRaw);
            if (percent) {
                const int bw = collage_->canvasWidth();
                const int bh = collage_->canvasHeight();
                cw = (bw > 0 && wRaw > 0) ? std::max(1, (int)std::lround(wRaw * bw / 100.0)) : cw;
                ch = (bh > 0 && hRaw > 0) ? std::max(1, (int)std::lround(hRaw * bh / 100.0)) : ch;
            }
            collage_->setSelectedCellSize(cw, ch);
            refresh();
            return;
        }
        collage_->setCanvasSize((int)std::lround(wRaw), (int)std::lround(hRaw));
        refresh();
    }
    else {
        if (!merge_) return;
        // 合并：0 合法（按原图尺寸）
        merge_->setPageSize(std::clamp((int)std::lround(wRaw), 0, 8192),
            std::clamp((int)std::lround(hRaw), 0, 8192));
        refresh();
    }
}

// ---------------------------------------------------------------- 按钮

void WinImageEdit::onSaveClicked()
{
    if (!hasContent()) {
        SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.saveEmpty"), nullptr, L"", L"", true);
        return;
    }

    if (mode_ == Mode::Collage) {
        if (!collage_) return;
        std::vector<BYTE> pixels;
        int w = 0, h = 0;
        if (!collage_->exportPixels(pixels, w, h) || w <= 0 || h <= 0) {
            SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.saveFail"), nullptr, L"", L"", true);
            return;
        }
        // 画布上的标注合成到画板像素上再保存
        if (annot_ && annot_->hasAnyShapes()) {
            std::vector<BYTE> merged;
            if (annot_->composeAnnotOnImage(pixels.data(), w, h, merged)) pixels.swap(merged);
        }
        const std::wstring ext = Util::extensionFromSaveFormat(Setting::get()->getSaveFormat());
        const std::wstring path = Util::getSaveFilePath(hwnd, ext, saveDefaultName(), Util::desktopDir().wstring());
        if (path.empty()) return;   // 用户取消
        if (!Util::saveToFile(path, w, h, pixels.data()))
            SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.saveFail"), nullptr, L"", L"", true);
        return;
    }

    // 合并文档
    if (!merge_) return;
    const int count = merge_->itemCount();
    if (count <= 0) {
        SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.saveEmpty"), nullptr, L"", L"", true);
        return;
    }
    const int fmt = merge_->format();          // 0 = PDF，1 = PPTX
    const std::wstring ext = (fmt == 0) ? L"pdf" : L"pptx";
    const std::wstring path = Util::getSaveFilePath(hwnd, ext, saveDefaultName(), Util::desktopDir().wstring());
    if (path.empty()) return;

    PdfWriter pdf;
    PptxWriter pptx;
    for (int i = 0; i < count; ++i) {
        std::vector<BYTE> pixels;
        int w = 0, h = 0;
        if (!merge_->renderPage(i, pixels, w, h) || w <= 0 || h <= 0) {
            SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.saveFail"), nullptr, L"", L"", true);
            return;
        }
        std::vector<BYTE> jpeg;
        if (!ImageExport::encodeJpeg(pixels, w, h, 90, jpeg)) {
            SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.saveFail"), nullptr, L"", L"", true);
            return;
        }
        if (fmt == 0) pdf.addPage(std::move(jpeg), w, h);
        else pptx.addPage(std::move(jpeg), w, h);
    }
    const bool ok = (fmt == 0) ? pdf.save(path) : pptx.save(path);
    if (!ok)
        SettingUi::ConfirmDialog::ask(this, Lang::get(L"imageEdit.saveFail"), nullptr, L"", L"", true);
}

void WinImageEdit::onCancelClicked()
{
    close();
}

// ---------------------------------------------------------------- 拖放

void WinImageEdit::onFilesDropped(const std::vector<std::wstring>& paths, POINT clientPos)
{
    if (paths.empty()) return;
    if (mode_ == Mode::Collage) {
        if (collage_) collage_->handleDrop(paths, clientPos);
    }
    else {
        if (merge_) merge_->handleDrop(paths, clientPos);
    }
    updateEmptyState();
    updateSizeRow();
    refresh();
}

LRESULT CALLBACK WinImageEdit::dropWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (s_instance && msg == WM_ACTIVATEAPP) {
        // 只有整个进程失去前台（切到别的软件）才收起属性栏；
        // 点击本进程内的属性栏/颜色面板/画布都保持常显，和截图工具栏一致
        if (s_instance->annot_) {
            if (wParam == FALSE) s_instance->annot_->suspendToolBar();
            else s_instance->annot_->resumeToolBar();
        }
    }
    if (s_instance && msg == WM_DROPFILES) {
        HDROP hd = reinterpret_cast<HDROP>(wParam);
        POINT pt{ 0, 0 };
        DragQueryPoint(hd, &pt);
        // DROPFILES 在 fNC=FALSE 时存的就是本窗口客户区坐标，直接用于画布命中

        std::vector<std::wstring> paths;
        std::vector<wchar_t> buf(32768);
        const UINT count = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(hd, i, buf.data(), (UINT)buf.size());
            if (len == 0) continue;
            paths.emplace_back(buf.data(), len);
        }
        DragFinish(hd);

        s_instance->onFilesDropped(paths, pt);
        return 0;
    }

    if (s_oldProc) return CallWindowProcW(s_oldProc, hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
