#include "pch.h"
#include "ImageEditAnnot.h"
#include "WinImageEdit.h"
#include "ImageEditTheme.h"
#include "SettingTheme.h"
#include "../App.h"
#include "../History.h"
#include "../Tool/ToolSub.h"
#include "../Tool/ToolbarChrome.h"
#include "../Tool/ToolbarTheme.h"
#include "../Shape/ShapeBase.h"
#include <algorithm>

namespace {
    // 图片编辑器的属性栏/颜色面板跟随软件主题（而不是截图工具栏自己的外观设置）：
    // 浅色气泡底用 #E5E5E5，悬停再深一档；深色沿用工具栏深色色板。
    ToolbarStore::Colors editorToolbarPalette()
    {
        if (SettingTheme::isDark()) return ToolbarStore::darkPreset();
        return ToolbarStore::Colors{ 0xE5E5E5FF, 0xD9D9D9FF, 0x18181BFF, 0x34C759FF, 0x34C759FF };
    }

    void applyEditorToolbarPalette()
    {
        ToolbarTheme::applyColors(editorToolbarPalette());
    }
}

ImageEditAnnotHost::ImageEditAnnotHost(WinImageEdit* editor) : editor_(editor)
{
    // 属性栏可能在「先点画布」时就懒创建，所以这里先把色板刷成软件主题配色
    applyEditorToolbarPalette();
    // 定时器（文字插入符、属性栏延后载入、预览脉冲、渐隐笔迹）都落在这个不可见窗口上
    tokTimer_ = onTimer.add([this](UINT id) { handleTimer(id); });
    // 1×1 的隐藏宿主窗：不显示、不抢焦点，只为 setTimer / 属性栏 owner 提供 HWND
    setSize(1.f / dpi, 1.f / dpi);
    createNativeWindow(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
}

ImageEditAnnotHost::~ImageEditAnnotHost()
{
    onTimer.remove(tokTimer_);
}

void ImageEditAnnotHost::handleTimer(UINT id)
{
    annotTimer(id);
    if (editor_) editor_->refresh();
}

HWND ImageEditAnnotHost::annotInputHwnd() const
{
    return (editor_ && editor_->hwnd) ? editor_->hwnd : hwnd;
}

POINT ImageEditAnnotHost::toImgPos(const POINT& pos) const
{
    return editor_ ? editor_->annotClientToImage(pos) : pos;
}

D2D1_POINT_2F ImageEditAnnotHost::annotToClientPt(float ax, float ay) const
{
    return editor_ ? editor_->annotImageToClient(ax, ay) : D2D1::Point2F(ax, ay);
}

D2D1_POINT_2F ImageEditAnnotHost::clientToAnnotPt(float cx, float cy) const
{
    const POINT p = editor_
        ? editor_->annotClientToImage(POINT{ (LONG)std::lround(cx), (LONG)std::lround(cy) })
        : POINT{ (LONG)std::lround(cx), (LONG)std::lround(cy) };
    return D2D1::Point2F((float)p.x, (float)p.y);
}

D2D1_RECT_F ImageEditAnnotHost::annotContentRect() const
{
    return editor_ ? editor_->annotArtRect() : D2D1::RectF(0.f, 0.f, -1.f, -1.f);
}

// ---------------- 工具 ----------------

void ImageEditAnnotHost::activateTool(const std::wstring& id)
{
    if (id.empty()) {
        deactivateTool();
        return;
    }
    // 先对齐 DPI：属性栏窗口在 ensureHistory 里才创建，构造时读 win->dpi
    if (editor_) dpi = (editor_->dpi > 0.f) ? editor_->dpi : 1.f;
    // 属性栏窗口创建时 initChrome 会调 ToolbarTheme::refresh()，这里先锁成软件主题色板，
    // 否则气泡底色又会被工具栏自己的外观设置（常常是深色）覆盖
    ToolbarTheme::setOverride(editorToolbarPalette());
    ensureHistory();
    ToolbarTheme::clearOverride();
    // 按钮/图标是在下面 showXxxTools 里创建的，运行时色板保持软件主题配色
    applyEditorToolbarPalette();
    // 图片编辑器：属性栏用带尖角的浮动气泡（箭头指向当前工具按钮），不再用无尖角的固定底板
    if (toolSub) toolSub->setFlatPopup(false);
    // 图片编辑器专用样式：气泡去掉外描边
    if (toolSub) toolSub->setBubbleBorderless(true);
    // 属性栏优先在主栏下方弹出（箭头朝上指向工具按钮）
    if (toolSub) toolSub->setBubbleAbove(false);
    if (toolSub && editor_ && editor_->hwnd) toolSub->bindOwner(editor_->hwnd);
    // 图片编辑器是普通窗口：属性栏不能置顶压到别的应用上
    if (toolSub) toolSub->setTopmost(false);
    // 打开工具时属性栏回到初始属性（与截图工具栏一致）
    if (toolSub) toolSub->endShapeStylePreview();
    if (toolSub) toolSub->resetStyleToDefaults();
    // 「尺寸」在图片编辑器里是独立工具 id：落笔由 History 按 id 直接创建尺寸，
    // 不再借 arrow + isLine/lineKind 的样式状态来推断形状类型。
    setForcedToolId(id);

    // 每次重建属性栏内容前再刷一次：图标色在创建按钮时读取
    applyEditorToolbarPalette();
    if (toolSub) {
        if (id == L"rect") toolSub->showRectTools();
        else if (id == L"arrow") {
            toolSub->isLine = false;
            toolSub->showArrowTools();
        }
        else if (id == L"dimension") {
            toolSub->showLineTools(ToolSub::LineDimension);
        }
        else if (id == L"pen") toolSub->showPenTools();
        else if (id == L"text") toolSub->showTextTools();
        else if (id == L"number") toolSub->showNumberTools();
        else if (id == L"mosaic") toolSub->showMosaicTools();
        else if (id == L"patina") toolSub->showPatinaTools();
        else if (id == L"watermark") toolSub->showWatermarkTools();
        else if (id == L"highlight") toolSub->showHighlightTools();
        else if (id == L"eraser") toolSub->showEraserTools();
        else toolSub->hideTools();
    }

    // 视口滤镜（包浆/水印/高亮）：切工具时清掉旧的，当前工具立即铺满画板
    if (history) history->syncViewportFilters(id);
    presentSelectedAnnotStyle();
    layoutTools();
    if (editor_) editor_->refresh();
}

void ImageEditAnnotHost::deactivateTool()
{
    clearForcedToolId();
    if (history) history->syncViewportFilters(L"");
    if (toolSub) toolSub->hideTools();
    if (editor_) editor_->refresh();
}

void ImageEditAnnotHost::suspendToolBar()
{
    if (toolSub) toolSub->suspendChrome();
}

void ImageEditAnnotHost::resumeToolBar()
{
    if (!annotating()) return;
    layoutTools();
}

// 右键逐层退出（与截图标注一致）：草稿 → 选中 → 样式开关 → 工具与属性栏
bool ImageEditAnnotHost::annotRightClick()
{
    if (annotMouseDown && newShape && history) {
        discardDraft();
        if (editor_) editor_->refresh();
        return true;
    }
    if (shapeHover) {
        clearAnnotSelection();
        if (editor_) editor_->refresh();
        return true;
    }
    if (toolSub && toolSub->hasActiveStyleOption() && toolSub->resetStyleOptions()) {
        if (editor_) editor_->refresh();
        return true;
    }
    if (!getCurToolId().empty()) {
        // 收起属性栏 + 工具栏选中态（由编辑器统一复位）
        if (editor_) editor_->exitAnnotTool();
        return true;
    }
    return false;
}

bool ImageEditAnnotHost::hasAnyShapes() const
{
    if (!history) return false;
    for (const auto& s : history->shapes) {
        if (s && !s->isUndo) return true;
    }
    return false;
}

bool ImageEditAnnotHost::canUndoAnnot() const
{
    return history && history->canUndo();
}

void ImageEditAnnotHost::undoAnnot()
{
    if (!history) return;
    history->undo();
    onHistoryChanged();
    if (editor_) editor_->refresh();
}

void ImageEditAnnotHost::clearAllShapesNow()
{
    clearAllShapes(false);
    if (editor_) editor_->refresh();
}

// ---------------- 输入转发 ----------------

bool ImageEditAnnotHost::forwardDown(POINT pos, bool right)
{
    return annotDown(pos, right ? TRUE : FALSE);
}

bool ImageEditAnnotHost::hitAnnotAtClient(POINT clientPos)
{
    const POINT ip = toImgPos(clientPos);
    return hitShapeAt((float)ip.x, (float)ip.y) != nullptr;
}

bool ImageEditAnnotHost::forwardDeleteKey(UINT key)
{
    dropStaleEditingText();
    if (editingText) {          // 文字编辑中：Backspace 退格、Delete 前删，作用在文字上
        annotKey(key);
        return true;
    }
    if (!hasAnyAnnotSelection()) return false;
    annotKey(key);
    return true;
}

void ImageEditAnnotHost::forwardMove(POINT pos)
{
    annotMove(pos);
    if (editor_) editor_->refresh();
}

void ImageEditAnnotHost::forwardUp(POINT pos, bool right)
{
    annotUp(pos, right ? TRUE : FALSE);
    if (editor_) editor_->refresh();
}

void ImageEditAnnotHost::forwardWheel(int dir)
{
    if (!toolSub || dir == 0) return;
    toolSub->adjustPrimarySize(dir);
    if (editor_) editor_->refresh();
}

void ImageEditAnnotHost::forwardChar(UINT ch)
{
    annotChar(ch);
    if (editor_) editor_->refresh();
}

void ImageEditAnnotHost::forwardIme()
{
    positionImeWindow();
}

void ImageEditAnnotHost::forwardKey(UINT key)
{
    AnnotHost::forwardKey(key);
    if (editor_) editor_->refresh();
}

// ---------------- 底图 ----------------

void ImageEditAnnotHost::setContentBitmap(std::vector<BYTE>& pixels, int w, int h)
{
    screenImg.Reset();
    if (pixels.empty() || w <= 0 || h <= 0 || !Ling::D2D::get()) {
        this->w = 0.f;
        this->h = 0.f;
        return;
    }
    App::createBitmapFromBGRA(w, h, pixels, screenImg.GetAddressOf());
    // 标注坐标 = 画板像素：窗口尺寸也跟着画板，视口滤镜/选区判定才能对得上
    if (screenImg) {
        this->w = (float)w;
        this->h = (float)h;
        scale = 1.f;
    }
}

void ImageEditAnnotHost::clearContentBitmap()
{
    screenImg.Reset();
    this->w = 0.f;
    this->h = 0.f;
}

void ImageEditAnnotHost::shiftCoordinates(float dx, float dy)
{
    if (!history) return;
    for (auto& s : history->shapes) {
        if (!s || s->isUndo || s->isViewportFilter()) continue;
        s->translateBy(dx, dy);
        s->syncLinks();
    }
    // 视口滤镜（包浆/水印）要重新铺满新画板
    if (history && !getCurToolId().empty()) history->syncViewportFilters(getCurToolId());
}

void ImageEditAnnotHost::paintOnScreen(ID2D1DeviceContext* ctx)
{
    if (!ctx || !history) return;
    if (editor_ && editor_->dpi > 0.f) dpi = editor_->dpi;
    paintAnnotPublic(ctx);
}

void ImageEditAnnotHost::paintPunchBackground(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r)
{
    if (!ctx) return;
    // 屏幕预览：画板允许透明（空画布 / 无背景色），screenImg 也可能是透明图；
    // 先铺一层画布底色，再把画板内容盖上去，文字区域才能稳定把线断开。
    // 离屏合成时 punchSource_ 就是最终底图，直接用它开洞，不能铺画布色（会破坏透明导出）。
    if (!punchSource_) {
        const uint32_t c = ImageEditTheme::canvasBg;
        const D2D1_COLOR_F col = D2D1::ColorF(
            ((c >> 24) & 0xFF) / 255.f,
            ((c >> 16) & 0xFF) / 255.f,
            ((c >> 8) & 0xFF) / 255.f,
            (c & 0xFF) / 255.f);
        if (!punchBgBrush_) {
            ctx->CreateSolidColorBrush(col, punchBgBrush_.GetAddressOf());
        }
        else {
            punchBgBrush_->SetColor(col);
        }
        if (punchBgBrush_) ctx->FillRectangle(r, punchBgBrush_.Get());
    }
    AnnotHost::paintPunchBackground(ctx, r);
}

void ImageEditAnnotHost::onToolChanged()
{
    // 改样式（含从颜色面板选色）后确保属性栏仍然显示：
    // 弹窗开合/激活切换可能把属性栏临时挂起，这里重新摆一次并补 show
    layoutTools();
    if (editor_) editor_->refresh();
}

// ---------------- 属性栏定位 ----------------

bool ImageEditAnnotHost::queryMainBarAnchor(MainBarAnchor& out) const
{
    if (!editor_) return false;
    float btnCenterX = 0.f;
    const RECT bar = editor_->toolbarScreenRect(&btnCenterX);
    if (bar.right <= bar.left || bar.bottom <= bar.top) return false;
    // 底部工具栏贴着窗口下沿：固定弹窗放在主栏上方（不盖住素材条）
    out.x = (float)bar.left;
    out.y = (float)bar.top;
    out.w = (float)(bar.right - bar.left);
    out.h = (float)(bar.bottom - bar.top);
    out.btnCenterX = btnCenterX;
    out.dpi = (editor_->dpi > 0.f) ? editor_->dpi : 1.f;
    return true;
}

void ImageEditAnnotHost::layoutTools()
{
    if (!toolSub || !editor_ || !editor_->hwnd) return;
    if (getCurToolId().empty() || !toolSub->hasContent()) {
        toolSub->hideTools();
        return;
    }
    RECT wr{};
    GetWindowRect(editor_->hwnd, &wr);
    toolSub->updatePosition(ToolbarChrome::workAreaNear(wr));
    // 属性栏压到画布窗口上方（它是图片编辑窗口的附属气泡）
    if (toolSub->hwnd) {
        SetWindowPos(toolSub->hwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // 气泡窗口不能抢激活：万一系统把激活给了它，立即还回编辑器（避免主窗变灰/失焦）
        if (editor_->hwnd && GetActiveWindow() == toolSub->hwnd)
            SetForegroundWindow(editor_->hwnd);
    }
}
