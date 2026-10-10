#include "pch.h"
#include <include/Ling.h>
#include "WinCap.h"
#include "WinPin.h"
#include "CutMask.h"
#include "../App.h"
#include "../Util.h"
#include "../Lang.h"
#include "../Update.h"
#include "CapLong.h"
#include "CapVideo.h"
#include "../Tool/ToolCap.h"
#include "../Tool/ToolSub.h"
#include "../Tool/ToolQrcode.h"
#include "../Tool/ToolOcr.h"
#include "../Ocr/OcrService.h"
#include "../Translate/TranslateService.h"
#include "../History.h"
#include "../Shape/ShapeText.h"
#include "../Tool/ToolbarTheme.h"
#include "../Tool/ToolbarChrome.h"
#include "CaptureEsc.h"
#include "CaptureShield.h"
#include "../Tip.h"
#include "../Setting.h"
#include "../App.h"
#include "../Lang.h"
#include "../Translate/TranslateTypes.h"
#include <sstream>
#include <algorithm>
#include <thread>
using namespace Microsoft::WRL;

namespace
{
	// 放大镜：120 画布 / 9 倍 / 绿十字 / 坐标+色值
	constexpr float kCanvas{ 120.f };
	constexpr float kScale{ 9.f };
	constexpr float kCursorGap{ 12.f };   // 外壳与光标的间距（不紧贴十字准星）
	constexpr float kPad{ 4.f };
	constexpr float kMetaGap{ 6.f };
	constexpr float kRowH{ 18.f };
	constexpr float kHintH{ 16.f };
	constexpr float kShellW{ kCanvas + kPad * 2.f };
	constexpr float kShellH{ kPad + kCanvas + kMetaGap + kRowH * 2.f + 4.f + kHintH + kPad };

	// OCR 临时缓存：识别是同步的（几百毫秒起），同一块选区反复点的时候没必要重跑。
	// 只在同一次截图会话里有效（重开截图 / 换了选区都作废）。
	OcrResult g_ocrCache;
	D2D1_RECT_F g_ocrCacheRect{};
	bool g_ocrCacheValid{ false };

	bool sameOcrRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
	{
		return std::abs(a.left - b.left) < 0.5f && std::abs(a.top - b.top) < 0.5f
			&& std::abs(a.right - b.right) < 0.5f && std::abs(a.bottom - b.bottom) < 0.5f;
	}
}

std::unique_ptr<WinCap> winCap;

WinCap::WinCap() : AnnotHost()
{
	setTitle(L"SnowAir 轻雪");
    auto [x1, y1, w1, h1] = App::get()->getScreenArea();
	this->x = x1;this->y = y1;this->w = (float)w1;this->h = (float)h1;
    onMouseDown.add([this](POINT pos, bool isRight) { this->onDown(pos, isRight); });
    onMouseMove.add([this](POINT pos) { this->onMove(pos); });
    onMouseUp.add([this](POINT pos, bool isRight) { this->onUp(pos, isRight); });
    onKeyDown.add([this](UINT key) { this->onKey(key); });
    onTimer.add([this](UINT id) {
		annotTimer(id);
		if (capLong) capLong->onTimerCB(id);
		if (cutMask) cutMask->onHoverDwellTimer(id);
		if (cutMask) cutMask->onHoverAnimTick(id);
	});
    onMouseWheel.add([this](POINT pos, float space) { this->onWheel(pos, space); });
    onDestroy.add([this]() { this->onClosed(); });
    // DPI 变了（用户改了缩放比例）：系统会按新旧缩放比把窗口整体缩放一圈，但本窗口是铺满整个
    // 虚拟桌面的，缩放之后就盖不住桌面了，而且底图、选区（cutMask->maskRect）用的都是物理像素，
    // 窗口一变形它们全部错位，挂在选区上的工具条自然也跟着偏。改缩放不会改分辨率，
    // 桌面还是那么多像素，所以等系统把建议矩形应用完（紧随而来的 WM_SIZE）再把窗口掰回桌面大小。
    // 位置不能在 onDpiChanged 里改 —— 那个事件在建议矩形生效之前触发，改了马上被覆盖
    onDpiChanged.add([this]() { dpiChanged = true; });
    onSizeChanged.add([this]() {
        if (!dpiChanged) return;
        dpiChanged = false;
        auto [x1, y1, w1, h1] = App::get()->getScreenArea();
        this->x = x1; this->y = y1; this->w = (float)w1; this->h = (float)h1;
        SetWindowPos(hwnd, nullptr, x1, y1, (int)w1, (int)h1, SWP_NOZORDER | SWP_NOACTIVATE);
        relayoutTool();
    });
}

WinCap::~WinCap()
{
}

// 进入截图会话。
// 【为什么要拆两段】建窗 + 整屏抓屏（GDI BitBlt+GetDIBits）在整屏分辨率下要几十~几百毫秒；
// 如果整段都发生在热键回调里，系统会认为"应用正忙"，鼠标上会短暂闪一下"忙"光标。
// 这里把抓屏丢给工作线程、建窗推给调度队列的下一轮：热键回调立即返回、应用回到空闲态，
// 抓屏完成后再回 UI 线程建窗（抓屏发生在覆盖窗 show() 之前，底图不会带上本窗口）。
void WinCap::init(StartMode mode)
{
    // 双击托盘图标会连着来两下，已经开着就不再建第二个
    if (winCap) return;
    // 先铺一层"看不见但能收鼠标"的盾牌：紧接着的建窗 + 抓屏有几十~几百毫秒空窗，
    // 不拦的话这一段拖动会落到下面的窗口（浏览器里就是"网页文字被拖选"），
    // 而且覆盖窗永远收不到这次按下 —— 表现成"按了快捷键不能马上框选"。
    // 覆盖窗 show() 之后由 onCreated 里的 drain() 把这一下补交给它。
    CaptureShield::arm();

    auto* app = Ling::App::get();
    if (!app) { initNow(mode, nullptr); return; }   // 无调度队列：退回同步（正常不会走到）
    const auto [sx, sy, sw, sh] = App::get()->getScreenArea();
    auto grab = std::make_shared<std::vector<BYTE>>();
    std::thread([grab, sx, sy, sw, sh, app, mode]() {
        // 演示画布不用底图（实时桌面），省掉这次抓屏
        if (mode != StartMode::Demo) *grab = Util::captureScreen(sx, sy, sw, sh);
        app->dq.TryEnqueue([grab, mode]() { WinCap::initNow(mode, grab); });
    }).detach();
}

void WinCap::initNow(StartMode mode, std::shared_ptr<std::vector<BYTE>> grab)
{
    if (winCap) return;   // 抓屏这段空窗里已经开过了
    auto ptr = new WinCap();
    winCap.reset(ptr);
    ptr->cutMask = std::make_unique<CutMask>(ptr);
    ptr->pendingGrab_ = std::move(grab);
    if (mode == StartMode::Demo) {
        // 演示画布：不画静帧（实时桌面）、不压暗/不描边/不画控点
        ptr->demoMode = true;
        ptr->hideScreenImg = true;
        ptr->stage = CapStage::Adjust;
        // 标注引擎要求 hasRect() 才给画；选区铺满整屏，但一个像素都不画出来
        ptr->cutMask->maskRect = D2D1::RectF(0.f, 0.f, (float)ptr->w, (float)ptr->h);
        ptr->cutMask->showDim = false;
        ptr->cutMask->showBorder = false;
        ptr->cutMask->showHandles = false;
        ptr->cutMask->hideLabel = true;
    }
    ptr->createNativeWindow(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP);
    if (mode == StartMode::Demo) {
        ptr->refresh();
        ptr->makeDemoToolCap();
        // 「功能设置 → 演示 → 默认工具」：0 渐隐画笔（默认）/ 1 画笔 / 2 无
        const int tool = Setting::get()->getDemoDefaultTool();
        if (tool != 2) ptr->startPenSlot(tool == 0);
    }
    else if (mode == StartMode::Record) {
        ptr->pendingRecord = true;
    }
}

WinCap* WinCap::get()
{
    return winCap.get();
}

// 托盘「屏幕录制」：照常进截图态（找元素 / 框选），框完由 onUp 直接切到录屏
void WinCap::beginRecordPick()
{
    init(StartMode::Record);
}

// 托盘「演示画布 / 演示模式」：
//   · 画面是**实时的**：不画会话开始时抓的静帧，桌面直接透出来（只留标注）
//   · **没有框选**：不压暗、不描边、不画控点、不画尺寸标签，整屏就是画布
//   · 工具栏按 Fullscreen 布局，默认摆在屏幕顶部居中（拖动/贴边逻辑后续再补）
void WinCap::beginDemoCanvas()
{
    init(StartMode::Demo);
}

void WinCap::makeDemoToolCap()
{
    if (!toolCap) {
        clearMainToolUserPlaced();
        toolCap = std::make_unique<ToolCap>(this);
        toolCap->applyDemoLayout();            // 必须在 createNativeWindow 之前换槽位表
        toolCap->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
        toolCap->bindOwner(hwnd);
    }
    layoutTool(toolCap.get());
}

// 「渐隐画笔」/「画笔」：工具栏上并列的一级槽位，底层工具都是 pen，区别只在 isPenFade。
// curId 用槽位 id（laser / pen），标注引擎经 resolveToolId 拿 pen。
// isPenFade 的设定放在 ToolCap::selectAnnotTool → ToolSub::setPenSlotMode，
// 这样截图栏 / 贴图栏 / 演示栏三条路径同一套逻辑（必须在建属性栏之前定好）。
void WinCap::startPenSlot(bool fade)
{
    ensureHistory();                           // 先有 toolSub，isPenFade 才能在建属性栏前定好
    // selectAnnotTool 会按槽位做排他高亮（旧的取消、新的选中）并把 curId 设为该槽位
    startAnnotate(fade ? L"laser" : L"pen");
    // 与旧路径一致：工具样式变了要通知一次（选中的滤镜/文字等按新属性重刷）
    if (toolSub) toolSub->notifyStyleChanged();
}

void WinCap::toggleDemoThrough()
{
    if (!demoMode) return;
    setMouseTransparent(!isMouseTransparent);
    if (toolCap) toolCap->setDemoThroughState(isMouseTransparent);
    refresh();
}

void WinCap::resetDemoCanvas()
{
    // 走 clearAllShapes：先断开 shapeHover/editingText 再释放，否则下一个工具栏点击
    // （onClick 里要读 hasSelectedAnnot）就会在已释放对象上做虚调用 → 闪退
    clearAllShapes();
    newShape = nullptr;
    refresh();
}

void WinCap::exitDemoCanvas()
{
    // 穿透状态下退出：先把命中样式恢复回来，再按安全顺序清标注并关窗
    setMouseTransparent(false);
    clearAllShapes();
    close();
}

void WinCap::selectFullScreenRect()
{
    if (!cutMask) return;
    cutMask->maskRect = D2D1::RectF(0.f, 0.f, (float)w, (float)h);
    cutMask->refreshLabel();
    stage = CapStage::Adjust;
    refresh();
    makeToolCap();
}

void WinCap::dispose()
{
    // 同 WinSetting::dispose：~WinBase 不销毁 HWND，退出前必须先关窗，否则残留空白窗
    if (winCap && winCap->hwnd) winCap->close();
    winCap.reset();
}

void WinCap::onCreated()
{
    // 新一次截图会话：底图换了，OCR 临时缓存作废
    g_ocrCacheValid = false;
    // 底图：优先用工作线程抓好的 BGRA（init 里已抓完，UI 线程只做 CreateBitmap）；
    // 失败/未就绪则兜底同步抓一次。演示画布不画静帧，直接跳过。
    if (pendingGrab_ && !pendingGrab_->empty()) {
        App::createBitmapFromBGRA((int)w, (int)h, *pendingGrab_, &screenImg);
    }
    else if (!hideScreenImg) {
        App::get()->takeScreenShot((int)x, (int)y, (int)w, (int)h, &screenImg);
    }
    pendingGrab_.reset();
    // 「显示光标」快照：必须在窗口显示前抓，否则抓到的是覆盖窗自己的光标
    captureCursorSnapshot();
	auto d2d = Ling::D2D::get();
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免调整选区时整帧闪烁
    canvas = body->makeChild<Ling::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    tip = std::make_unique<Tip>(this);
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.75f), brushMuted.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.45f), brushHint.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1E1E1F), brushBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0, 0.f, 0.f, 0.24f), brushShadow.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x34C759), crossBrush.GetAddressOf());
    POINT pos;
    GetCursorPos(&pos);
    ScreenToClient(hwnd, &pos);
    setPixPos(pos);
    show();
    CaptureEsc::claim(hwnd);
    // 窗口已经能收鼠标了：把空窗期被盾牌吃掉的那一下按下补进来（还按着就接着拖）
    CaptureShield::drain(hwnd);
}

void WinCap::layout()
{
    Ling::WinBase::layout();
    if (!canvas) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, (float)w, (float)h);
    if (!hideScreenImg) {
        ctx->DrawBitmap(screenImg.Get(), destRect);
    }
    // 标注在底图上、暗色遮罩下：选区外被压暗，洞内正常显示（与「全屏纸+导出裁切」一致）
    // 长图选区大图由 CapLong 后画，标注需叠在其上
    if (annotLive || (stage == CapStage::Video && hasAnnotShapes())) {
        if (stage != CapStage::Long) paintShapes(ctx);
    }
    if (annotLive && (sizePreviewHold || GetTickCount64() < sizePreviewPulseUntil))
		paintSizePreview(ctx);
    // 查找元素悬停：仅描边；已框选/标注中：截图选区控点一直显示
    // 演示模式不画遮罩的任何部件（不压暗、不描边、不控点、不标签）—— 整屏就是画布
    if (cutMask && !demoMode) {
        const bool findHover = (stage == CapStage::Select && !isPress);
        cutMask->showDim = !findHover;
        // 拖选区时不画控点，少一圈 FillRectangle（对齐跟手）
        const bool editingSel = isPress && (stage == CapStage::Adjust || maskDragging
            || videoCanAdjust() || (stage == CapStage::Select));
		const bool longEdit = stage == CapStage::Long && capLong && capLong->isEditMode();
		// 长图编辑：对齐贴图——保留边框，隐藏四周调选区控点
        cutMask->showHandles = !editingSel && !longEdit
			&& (stage == CapStage::Adjust || stage == CapStage::Long || videoCanAdjust());
		cutMask->showBorder = true;
        cutMask->drawQrcodeFill = (toolQrcode != nullptr);
        cutMask->paint(ctx);
    }
    if (capLong) capLong->paint(ctx);
    if (stage == CapStage::Long && (annotLive || hasAnnotShapes()) && capLong) {
        D2D1_MATRIX_3X2_F oldXform{};
        ctx->GetTransform(&oldXform);
		if (capLong->beginLongPreviewAnnotPaint(ctx, oldXform)) {
			paintShapes(ctx, false);
			capLong->endLongAnnotPaint(ctx, oldXform);
		}
        if (capLong->beginLongAnnotPaint(ctx, oldXform)) {
            paintShapes(ctx);
            capLong->endLongAnnotPaint(ctx, oldXform);
        }
    }
    paintPix(ctx);
    paintCursor(ctx);
    canvas->finishPaint();
}

void WinCap::applyMaskHitCursor(MaskHit hit)
{
	switch (hit) {
	case MaskHit::TopLeft:
	case MaskHit::BottomRight:
		SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
		break;
	case MaskHit::TopRight:
	case MaskHit::BottomLeft:
		SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
		break;
	case MaskHit::Top:
	case MaskHit::Bottom:
		SetCursor(LoadCursor(nullptr, IDC_SIZENS));
		break;
	case MaskHit::Left:
	case MaskHit::Right:
		SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
		break;
	case MaskHit::Inside:
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		break;
	default:
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		break;
	}
}

bool WinCap::hasAnnotShapes() const
{
	if (!history) return false;
	for (auto& s : history->shapes) {
		if (s && !s->isUndo) return true;
	}
	return false;
}

bool WinCap::beginMaskResize(POINT pos)
{
	if (!cutMask || !cutMask->hasRect()) return false;
	// 演示画布没有"选区"这个概念（铺满整屏、不描边不压暗）：不许拖/调边，
	// 否则一拖就把看不见的选区挪走、还会 setToolChromeHidden 把工具条藏掉
	if (demoMode) return false;
	const auto hit = cutMask->hitTest(pos);
	if (hit == MaskHit::None) return false;
	// 边/角始终可调选区；区内「拖动/平移」仅在无标注时允许（有笔迹后锁平移）
	if (hit == MaskHit::Inside) {
		if (hasAnnotShapes() || !getCurToolId().empty()) return false;
	}
	maskDragging = true;
	isPress = true;
	SetCapture(hwnd);
	cutMask->startAdjust(pos);
	setPixPos(pos);
	setToolChromeHidden(true);
	refresh();
	return true;
}

void WinCap::setToolChromeHidden(bool hidden)
{
	// 按当前阶段只显隐「自己的」工具栏，截图栏与录屏栏互不串台
	if (stage == CapStage::Video) {
		if (capVideo) capVideo->setToolVisible(!hidden);
		return;
	}
	if (stage == CapStage::Long) {
		if (capLong) capLong->setToolVisible(!hidden);
		return;
	}
	if (hidden) {
		if (toolCap) {
			toolCap->hideHoverTip();
			toolCap->hide();
		}
		if (toolSub) toolSub->suspendChrome();
		return;
	}
	if (toolCap) toolCap->show();
	layoutTools();
	if (toolSub && toolSub->hasContent()) {
		RECT winRect{ x, y, x + (int)w, y + (int)h };
		toolSub->resumeChrome(ToolbarChrome::workAreaNear(winRect));
	}
}

BOOL WinCap::setCursor()
{
	POINT pos{};
	GetCursorPos(&pos);
	ScreenToClient(hwnd, &pos);

	if (annotLive) {
		if (stage == CapStage::Long && capLong
			&& (capLong->isPreviewPanning() || capLong->isSelViewportPanning()
				|| capLong->hitPreview(pos)
				|| (capLong->isEditMode() && !shapeHover
					&& capLong->hitSelViewport(pos)
					&& (getCurToolId().empty() || !hitShapeAt(
						(float)toImgPos(pos).x, (float)toImgPos(pos).y))))) {
			// 未选中标注：选区内示意可拖浏览（有工具且点在图形上时除外）
			if (capLong->isEditMode() && getCurToolId().empty()
				&& capLong->hitSelViewport(pos)) {
				capLong->setCursor();
				return TRUE;
			}
			if (capLong->isPreviewPanning() || capLong->isSelViewportPanning()
				|| capLong->hitPreview(pos)) {
				capLong->setCursor();
				return TRUE;
			}
		}
		if (maskDragging && cutMask) {
			const auto hit = cutMask->adjustingHit();
			applyMaskHitCursor(hit != MaskHit::None ? hit : cutMask->hitTest(pos));
			return TRUE;
		}
		const auto img = toImgPos(pos);
		const float ix = (float)img.x, iy = (float)img.y;

		// 1) 已选中标注：控点缩放 / 平移（优先于选区边）
		if (shapeHover && !shapeHover->isUndo && shapeHover != newShape
			&& !shapeHover->isViewportFilter()) {
			if (annotMouseDown) {
				shapeHover->setCursor();
				return TRUE;
			}
			shapeHover->mouseMove(ix, iy);
			if (shapeHover->hoverDraggerIndex >= 0) {
				shapeHover->setCursor();
				return TRUE;
			}
		}

		// 2) 截图选区边/角：悬停即 ↔ ↕（不必等到拖动）
		// 长图编辑态选区已定死，不提供调边光标；演示画布没有可调选区
		const bool longEdit = stage == CapStage::Long && capLong && capLong->isEditMode();
		if (!demoMode && cutMask && cutMask->hasRect() && !longEdit) {
			if (cutMask->hitInfoControl(pos) != InfoHit::None) {
				SetCursor(LoadCursor(nullptr, IDC_HAND));
				return TRUE;
			}
			const auto hit = cutMask->hitTest(pos);
			if (hit != MaskHit::None && hit != MaskHit::Inside) {
				applyMaskHitCursor(hit);
				return TRUE;
			}
		}
		// 长图编辑且未选中标注：空白处可拖浏览
		if (longEdit && !shapeHover && getCurToolId().empty()
			&& capLong->hitSelViewport(pos)) {
			capLong->setCursor();
			return TRUE;
		}

		// 3) 未选中标注悬停手型 / 工具光标 / 无标注时区内平移
		applyAnnotCursor();
		return TRUE;
	}
	// 演示画布：没开标注会话时画布不可交互，光标固定普通箭头
	if (demoMode) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return TRUE;
	}
	if (stage == CapStage::Select) {
		SetCursor(LoadCursor(nullptr, IDC_CROSS));
		return TRUE;
	}
	if (stage == CapStage::Long && capLong) {
		capLong->setCursor();
		return TRUE;
	}
	if (stage == CapStage::Adjust && cutMask) {
		if (!isPress && cutMask->hitInfoControl(pos) != InfoHit::None) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		if (isPress) {
			const auto hit = cutMask->adjustingHit();
			applyMaskHitCursor(hit != MaskHit::None ? hit : cutMask->hitTest(pos));
		}
		else {
			applyMaskHitCursor(cutMask->hitTest(pos));
		}
		return TRUE;
	}
	if (videoCanAdjust() && cutMask) {
		if (!isPress && cutMask->hitInfoControl(pos) != InfoHit::None) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		if (isPress) {
			const auto hit = cutMask->adjustingHit();
			applyMaskHitCursor(hit != MaskHit::None ? hit : cutMask->hitTest(pos));
		}
		else {
			applyMaskHitCursor(cutMask->hitTest(pos));
		}
		return TRUE;
	}
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
	return TRUE;
}

void WinCap::applyEmptyToolCursor()
{
	// 演示画布没选中工具 = 什么都不做：不该给出"可平移选区"的移动光标
	//（全屏画布"无工具时不拖选区、不重开框选"）
	if (demoMode) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return;
	}
	POINT pos{};
	GetCursorPos(&pos);
	ScreenToClient(hwnd, &pos);
	if (stage == CapStage::Long && capLong && capLong->isEditMode()
		&& capLong->hitSelViewport(pos)) {
		capLong->setCursor();
		return;
	}
	if (cutMask && cutMask->hasRect()) {
		const auto hit = cutMask->hitTest(pos);
		// 区内：无标注 → 可平移(SizeAll)；**有标注 → 普通箭头**
		//（用户要的：有标注 + 没选工具 = 正常鼠标样式；十字只属于"选了工具"）
		if (hit == MaskHit::Inside) {
			SetCursor(LoadCursor(nullptr, hasAnnotShapes() ? IDC_ARROW : IDC_SIZEALL));
			return;
		}
		if (hit != MaskHit::None) {
			applyMaskHitCursor(hit);
			return;
		}
	}
	// 框还没框 / 点在选区外：保持十字（拖框选、重开选区）
	SetCursor(LoadCursor(nullptr, IDC_CROSS));
}

void WinCap::setPixPos(POINT pos)
{
	// 外壳贴在鼠标**右下角**，与光标留 12 逻辑像素间距
	//（不紧贴十字准星），越界时夹回窗口内。pixFocus 仍是取样点（= 光标处）。
	const float gap = kCursorGap * dpi;
	const float shellW = kShellW * dpi;
	const float shellH = kShellH * dpi;
	pixPos.x = (LONG)std::clamp((float)pos.x + gap, 0.f, std::max(0.f, w - shellW));
	pixPos.y = (LONG)std::clamp((float)pos.y + gap, 0.f, std::max(0.f, h - shellH));
	pixFocus = pos;
}

void WinCap::samplePixColor()
{
	if (!screenImg) return;
	const int px = (int)std::clamp(pixFocus.x, 0L, (LONG)std::max(0.f, w - 1.f));
	const int py = (int)std::clamp(pixFocus.y, 0L, (LONG)std::max(0.f, h - 1.f));
	if (px == pixSampleX_ && py == pixSampleY_) return;
	auto d2d = Ling::D2D::get();
	if (!pixSampleBmp_) {
		D2D1_BITMAP_PROPERTIES1 prop{};
		// 必须与底图同格式：原先写死 PREMULTIPLIED，与底图(IGNORE)不一致会令
		// CopyFromBitmap 失败 → 色值恒为黑（"无法识别颜色"的根因）。
		prop.pixelFormat = screenImg->GetPixelFormat();
		prop.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
		screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
		d2d->deviceContext->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, &prop, pixSampleBmp_.GetAddressOf());
	}
	if (!pixSampleBmp_) return;
	D2D1_POINT_2U dest{ 0, 0 };
	D2D1_RECT_U src{ (UINT)px, (UINT)py, (UINT)px + 1, (UINT)py + 1 };
	if (FAILED(pixSampleBmp_->CopyFromBitmap(&dest, screenImg.Get(), &src))) return;
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(pixSampleBmp_->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return;
	const BYTE* p = mapped.bits;
	pixColor_ = RGB(p[2], p[1], p[0]);
	pixSampleBmp_->Unmap();
	// 只有真正读到值才记缓存：失败时下帧重试，不会被 -1/旧坐标误跳过
	pixSampleX_ = px;
	pixSampleY_ = py;
}

void WinCap::paintPix(ID2D1DeviceContext* ctx)
{
	// 放大镜显示时机：框选真正拖动 / 调四边四角 / **查找元素悬停**（Select 阶段未按下）
	if (!screenImg || !cutMask) return;
	const bool framing = (stage == CapStage::Select && isPress && selectDragging_);
	const bool resizing = cutMask->isResizingEdge()
		&& (maskDragging || ((stage == CapStage::Adjust || videoCanAdjust()) && isPress));
	// 查找元素阶段（未按下时悬停画布）：跟手显示放大镜
	const bool elementHover = stage == CapStage::Select && !isPress && !demoMode
		&& Setting::get()->getFindWindowElements();
	if (!framing && !resizing && !elementHover) return;

	const float pad = kPad * dpi;
	const float canvas = kCanvas * dpi;
	const float shellW = kShellW * dpi;
	const float shellH = kShellH * dpi;
	const float x0 = (float)pixPos.x, y0 = (float)pixPos.y;
	const float radius = 6.f * dpi;

	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0 + dpi, x0 + shellW, y0 + shellH + dpi), radius, radius), brushShadow.Get());
	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x0 + shellW, y0 + shellH), radius, radius), brushBg.Get());

	const D2D1_RECT_F canvasRect{ x0 + pad, y0 + pad, x0 + pad + canvas, y0 + pad + canvas };
	// 轴对齐裁剪代替每帧建圆角 Geometry，拖边更跟手
	ctx->PushAxisAlignedClip(canvasRect, D2D1_ANTIALIAS_MODE_ALIASED);
	ctx->FillRectangle(canvasRect, brushBg.Get());

	const float picker = canvas / kScale;
	const float half = picker * 0.5f;
	D2D1_RECT_F src{
		(float)pixFocus.x - half, (float)pixFocus.y - half,
		(float)pixFocus.x + half, (float)pixFocus.y + half
	};
	ctx->DrawBitmap(screenImg.Get(), canvasRect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src);

	const float cx = canvasRect.left + canvas * 0.5f;
	const float cy = canvasRect.top + canvas * 0.5f;
	const float stroke = 1.5f * dpi;
	ctx->DrawLine(D2D1::Point2F(cx, canvasRect.top), D2D1::Point2F(cx, canvasRect.bottom), crossBrush.Get(), stroke);
	ctx->DrawLine(D2D1::Point2F(canvasRect.left, cy), D2D1::Point2F(canvasRect.right, cy), crossBrush.Get(), stroke);
	ctx->PopAxisAlignedClip();

	// 色值从底图取样（避免 GetPixel 同步读屏拖垮帧率）
	samplePixColor();
	const auto hex = std::format(L"#{:02X}{:02X}{:02X}", GetRValue(pixColor_), GetGValue(pixColor_), GetBValue(pixColor_));
	const auto posText = std::format(L"{}, {}", pixFocus.x, pixFocus.y);

	const float metaLeft = x0 + pad + 2.f * dpi;
	const float metaW = canvas - 4.f * dpi;
	const float rowH = kRowH * dpi;
	const float fontSize = 12.f * dpi;
	const float hintSize = 11.f * dpi;
	float metaY = y0 + pad + canvas + kMetaGap * dpi;
	auto d2d = Ling::D2D::get();

	auto drawRow = [&](float y, const wchar_t* label, const std::wstring& value, ID2D1SolidColorBrush* valueBrush) {
		ComPtr<IDWriteTextLayout> left, right;
		d2d->dwriteFactory->CreateTextLayout(label, (UINT32)wcslen(label), d2d->baseTextFormat.Get(), metaW, rowH, left.GetAddressOf());
		d2d->dwriteFactory->CreateTextLayout(value.data(), (UINT32)value.size(), d2d->baseTextFormat.Get(), metaW, rowH, right.GetAddressOf());
		if (left) {
			left->SetFontSize(fontSize, { 0, UINT32_MAX });
			left->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			ctx->DrawTextLayout({ metaLeft, y }, left.Get(), brushMuted.Get());
		}
		if (right) {
			right->SetFontSize(fontSize, { 0, UINT32_MAX });
			right->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
			right->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			ctx->DrawTextLayout({ metaLeft, y }, right.Get(), valueBrush);
		}
	};
	drawRow(metaY, L"坐标", posText, brushText.Get());
	metaY += rowH + 2.f * dpi;
	drawRow(metaY, L"色值", hex, brushText.Get());
	metaY += rowH + 2.f * dpi;

	// 复制成功后提示行临时变绿显示「色值复制成功！」，1.5s 后自动恢复
	const bool copied = GetTickCount64() < pixCopiedUntil_;
	const wchar_t* hintStr = copied ? L"色值复制成功！" : L"按 Ctrl+C 复制色值";
	ComPtr<IDWriteTextLayout> hint;
	d2d->dwriteFactory->CreateTextLayout(hintStr, (UINT32)wcslen(hintStr), d2d->baseTextFormat.Get(), metaW, kHintH * dpi, hint.GetAddressOf());
	if (hint) {
		hint->SetFontSize(hintSize, { 0, UINT32_MAX });
		hint->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		ctx->DrawTextLayout({ metaLeft, metaY }, hint.Get(), copied ? crossBrush.Get() : brushHint.Get());
	}
}

void WinCap::forwardKey(UINT key)
{
	onKey(key);
}

void WinCap::onKey(UINT key)
{
    if (key == VK_ESCAPE) {
        // 演示画布 + 穿透中：第一次 Esc 先退出穿透，再按一次才关画布
        if (demoMode && isMouseTransparent) {
            toggleDemoThrough();
            return;
        }
        // Esc 始终退出截图；分层退出只走右键
        close();
        return;
    }
    // 录屏：Tab 切换鼠标穿透（穿透后焦点常在别的窗，靠 CaptureEsc::claimTab 热键兜底）
    if (key == VK_TAB && stage == CapStage::Video) {
        // 热键与本窗/工具栏 WM_KEYDOWN 可能连着来两次，短时去重
        static ULONGLONG lastTabMs{ 0 };
        const auto now = GetTickCount64();
        if (now - lastTabMs < 100) return;
        lastTabMs = now;
        setMouseTransparent(!isMouseTransparent);
        if (isMouseTransparent && capVideo) capVideo->cancelAnnotSelect();
        return;
    }
    if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
        const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
        const int dx = (key == VK_LEFT) ? -step : (key == VK_RIGHT) ? step : 0;
        const int dy = (key == VK_UP) ? -step : (key == VK_DOWN) ? step : 0;
        if (nudgeSelectionByArrow(dx, dy)) return;
        // 查找元素阶段：方向键挪光标跟手高亮
        POINT pos;
        GetCursorPos(&pos);
        pos.x += dx;
        pos.y += dy;
        SetCursorPos(pos.x, pos.y);
        return;
    }
    if (annotLive) {
        annotKey(key);
        // 正在编辑标注文字（输入框有焦点）：键盘归输入框 —— Enter 是"换行"（线上文字靠用户
        // 自己回车排成竖列），Ctrl+C/V 是文本框自己的复制粘贴，都不能被下面这些截图快捷键抢走。
        // 这里 return 只是不再处理快捷键，事件派发照旧会走到输入框那个订阅者
        if (editingText) return;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        // 演示画布：Enter 不做"完成复制"、Ctrl+C/S 复制保存后也不关画布
        //（全屏画布：Enter 不结束会话；画布只靠 ✕ / Esc 退出）
        if (demoMode) {
            if (ctrl && key == 'C') copyToClipboard(true);
            else if (ctrl && key == 'S') saveToFile(true);
            return;
        }
        if (ctrl && key == 'C') { copyToClipboard(); return; }
        if (ctrl && key == 'S') { saveToFile(); return; }
        if (key == VK_RETURN) { copyToClipboard(); return; }
        return;
    }
    // 放大镜可见时 Ctrl+C 复制色值（框选拖动 / 调边角 / 查找元素悬停——后者未按下）
    if (key == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)
        && ((stage == CapStage::Select && isPress && selectDragging_)
            || (stage == CapStage::Select && !isPress && !demoMode
                && Setting::get()->getFindWindowElements())
            || (cutMask && cutMask->isResizingEdge()
                && (maskDragging || ((stage == CapStage::Adjust || videoCanAdjust()) && isPress))))) {
        samplePixColor();
        Ling::Util::setTextToClipboard(std::format(L"#{:02X}{:02X}{:02X}",
            GetRValue(pixColor_), GetGValue(pixColor_), GetBValue(pixColor_)));
        pixCopiedUntil_ = GetTickCount64() + 1500;   // 提示行临时变绿
        refresh();
    }
    // 长图与录屏阶段：Ctrl+S 存文件，Ctrl+C 存剪切板
    else if ((key == 'S' || key == 'C') && (GetKeyState(VK_CONTROL) & 0x8000)) {
        const bool toClipboard{ key == 'C' };
        if (stage == CapStage::Long && capLong && capLong->hasImage()) {
            if (toClipboard) longCopyToClipboard();
            else if (!longSaveToFile()) return;
            close();
        }
        else if (stage == CapStage::Video && capVideo) {
            capVideo->onSaveKey(toClipboard);
        }
    }
    // Enter：长图 / 录屏阶段存剪切板
    else if (key == VK_RETURN) {
        copyCurrentStage();
    }
}

bool WinCap::nudgeSelectionByArrow(int dx, int dy)
{
	if (dx == 0 && dy == 0) return false;
	if (!cutMask || !cutMask->hasRect()) return false;
	// 演示画布没有可挪的选区（全屏画布方向键不微调选区）
	if (demoMode) return false;
	if (stage != CapStage::Adjust && !annotLive && !videoCanAdjust()) return false;
	if (maskDragging || isPress) return false;
	// 已有标注时方向键不挪选区
	if (annotLive && hasAnnotShapes()) return false;
	cutMask->translateBy((float)dx, (float)dy);
	if (annotLive) syncViewportFilters();
	if (annotLive) layoutTools();
	else if (videoCanAdjust() && capVideo) capVideo->layoutTool();
	else if (toolCap) layoutTool(toolCap.get());
	syncQrcodePanel();
	syncOcrPanel();
	refresh();
	return true;
}

bool WinCap::nudgeSelectionByOutsideWheel(POINT pos, int dir)
{
	if (!cutMask || !cutMask->hasRect()) return false;
	if (stage != CapStage::Adjust && !annotLive && !videoCanAdjust()) return false;
	const auto& sel = cutMask->maskRect;
	const float px = (float)pos.x, py = (float)pos.y;
	const bool inColumn = px >= sel.left && px <= sel.right;
	const bool inRow = py >= sel.top && py <= sel.bottom;
	const bool isAbove = py < sel.top;
	const bool isBelow = py > sel.bottom;
	const bool isLeft = px < sel.left;
	const bool isRight = px > sel.right;
	const bool isVerticalSide = (isAbove || isBelow) && inColumn;
	const bool isHorizontalSide = (isLeft || isRight) && inRow;
	if (!isVerticalSide && !isHorizontalSide) return false;

	constexpr float kNudge = 2.f;
	float dx = 0.f, dy = 0.f;
	if (isVerticalSide) {
		const float sideSign = isAbove ? 1.f : -1.f;
		dy = sideSign * (float)dir * kNudge;
	}
	else {
		const float sideSign = isLeft ? 1.f : -1.f;
		dx = sideSign * (float)dir * kNudge;
	}
	if (!cutMask->translateBy(dx, dy)) return true;
	// 跟方向键一样：先跟栏再 refresh，否则只见工具栏动、选区画面不跟
	if (annotLive) syncViewportFilters();
	if (annotLive) layoutTools();
	else if (videoCanAdjust() && capVideo) capVideo->layoutTool();
	else if (toolCap) layoutTool(toolCap.get());
	syncQrcodePanel();
	syncOcrPanel();
	refresh();
	return true;
}

bool WinCap::sizePreviewOrigin(float& cx, float& cy, float& halfSpan) const
{
	if (!cutMask || !cutMask->hasRect()) return false;
	const auto& r = cutMask->maskRect;
	cx = (r.left + r.right) * 0.5f;
	cy = (r.top + r.bottom) * 0.5f;
	halfSpan = std::min(80.f, std::max(40.f, (r.right - r.left) * 0.14f));
	return true;
}

POINT WinCap::toImgPos(const POINT& pos) const
{
	if (stage == CapStage::Long && capLong && (capLong->isEditMode() || hasAnnotShapes()))
		return capLong->clientToLongImg(pos);
	return AnnotHost::toImgPos(pos);
}

D2D1_POINT_2F WinCap::annotToClientPt(float ax, float ay) const
{
	if (stage == CapStage::Long && capLong && (capLong->isEditMode() || hasAnnotShapes()))
		return capLong->longImgToClient(ax, ay);
	return AnnotHost::annotToClientPt(ax, ay);
}

D2D1_POINT_2F WinCap::clientToAnnotPt(float cx, float cy) const
{
	if (stage == CapStage::Long && capLong && (capLong->isEditMode() || hasAnnotShapes())) {
		const auto p = capLong->clientToLongImg(POINT{ (LONG)std::lround(cx), (LONG)std::lround(cy) });
		return D2D1::Point2F((float)p.x, (float)p.y);
	}
	return AnnotHost::clientToAnnotPt(cx, cy);
}

void WinCap::onWheel(POINT pos, float space)
{
	if (space == 0.f) return;
	const int dir = space > 0.f ? 1 : -1;

	// 长截图：预览区优先；编辑标注时其余走标注滚轮
	if (stage == CapStage::Long) {
		if (capLong && capLong->onWheel(pos, space)) return;
		if (annotLive && capLong && capLong->isEditMode()) {
			const auto img = toImgPos(pos);
			if (shapeHover && !shapeHover->isViewportFilter()) {
				shapeHover->mouseWheel((float)img.x, (float)img.y,
					dir > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
				pulseSizePreview();
				return;
			}
			if (cutMask && cutMask->hasRect()) {
				const auto& r = cutMask->maskRect;
				if (pos.x >= (int)r.left && pos.x <= (int)r.right
					&& pos.y >= (int)r.top && pos.y <= (int)r.bottom
					&& toolSub && !getCurToolId().empty()
					&& getCurToolId() != L"eraser") {
					toolSub->adjustPrimarySize(dir);
				}
			}
		}
		return;
	}

	if (annotLive) {
		const auto img = toImgPos(pos);
		// 单选标注：改该标注粗细
		if (shapeHover && !shapeHover->isViewportFilter()) {
			shapeHover->mouseWheel((float)img.x, (float)img.y,
				dir > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
			// 改粗/圆角会改像素：上层盖着的模糊/马赛克要重新取样
			refreshFiltersAbove(shapeHover);
			pulseSizePreview();
			return;
		}
		// 视窗外上/下/左/右：微调选区
		if (nudgeSelectionByOutsideWheel(pos, dir)) return;
		// 视窗内 + 有工具：改默认粗细 + 临时预览
		if (cutMask && cutMask->hasRect()) {
			const auto& r = cutMask->maskRect;
			if (pos.x >= (int)r.left && pos.x <= (int)r.right
				&& pos.y >= (int)r.top && pos.y <= (int)r.bottom
				&& toolSub && !getCurToolId().empty()
				&& getCurToolId() != L"eraser") {
				toolSub->adjustPrimarySize(dir);
				return;
			}
		}
		return;
	}

	// 框选 / 录屏开录前：区外滚轮微调位置
	if (stage == CapStage::Adjust || videoCanAdjust())
		nudgeSelectionByOutsideWheel(pos, dir);
}

void WinCap::copyCurrentStage()
{
    if (stage == CapStage::Adjust) {
        copyToClipboard();
    }
    else if (stage == CapStage::Long && capLong && capLong->hasImage()) {
        // 还没点"开始"滚动时一张图都没有（hasImage），此时不该收工
        longCopyToClipboard();
        close();
    }
    else if (stage == CapStage::Video && capVideo) {
        // 停录、存盘、关窗都在 ToolVideo 那边一条龙做完
        capVideo->onSaveKey(true);
    }
}

ComPtr<ID2D1Bitmap1> WinCap::getCutImg()
{
    ComPtr<ID2D1Bitmap1> cutImg;
    auto& maskRect = cutMask->maskRect;
    const UINT32 cw = (UINT32)(maskRect.right - maskRect.left);
    const UINT32 ch = (UINT32)(maskRect.bottom - maskRect.top);
    if (cw == 0 || ch == 0) return cutImg;
    D2D1_BITMAP_PROPERTIES1 prop{};
    prop.pixelFormat = screenImg->GetPixelFormat();
    prop.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
    Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(cw, ch), nullptr, 0, &prop, cutImg.GetAddressOf());
    auto start = D2D1::Point2U(0, 0);
    auto rect = D2D1::RectU((UINT32)maskRect.left, (UINT32)maskRect.top, (UINT32)maskRect.right, (UINT32)maskRect.bottom);
    cutImg->CopyFromBitmap(&start, screenImg.Get(), &rect);
    return cutImg;
}

LRESULT WinCap::onHitTest(const POINT pos)
{
    // 仅「正在录制」时整窗穿透；开录前还要拖选区
    if (isMouseTransparent) return HTTRANSPARENT;
    return HTCLIENT;
}

void WinCap::onDown(POINT pos, bool isRight)
{
    // 按下即撤查找元素的查询保护：查询期间遮罩可能挂着"临时点穿 + 临时捕获"，
    // 不先撤掉这一下按下会漏给下层窗口，也会把下面要 SetCapture 的拖框捕获弄丢
    if (cutMask) cutMask->cancelHoverQuery();
    if (isRight) {
        if (handleLayerCancel(false))
            requestExitAfterRightUp();
        return;
    }
    // 点遮罩易把全屏窗抬到前；侧栏是 owned 窗，再抬一次确保可点
    if (stage == CapStage::Long && capLong) {
		capLong->raiseTool();
		if (capLong->hitPreview(pos)) {
			capLong->onDown(pos, false);
			return;
		}
		if (annotLive && capLong->isEditMode()) {
			if (cutMask && cutMask->hasRect()) {
				const auto& r = cutMask->maskRect;
				if (pos.x >= (int)r.left && pos.x <= (int)r.right
					&& pos.y >= (int)r.top && pos.y <= (int)r.bottom) {
					annotDown(pos, isRight);
				}
			}
			return;
		}
		capLong->onDown(pos, false);
		return;
	}
    if (cutMask && !demoMode && cutMask->onInfoDown(pos)) {
        // 仅拖圆角滑条需要 Capture；点开关/色块立刻放开会抢工具栏鼠标
        if (cutMask->infoInteracting())
            SetCapture(hwnd);
        if (tip) { tip->hide(); infoTipHit_ = InfoHit::None; }
        return;
    }
    // 选区框好之后、且当前没有任何标注时，窗口里任意位置双击 = 点工具条上的「完成」（复制并退出）。
    // 双击判定自己做：Ling 的窗口类没带 CS_DBLCLKS，WM_LBUTTONDBLCLK 根本不会来，
    // 所以用系统双击间隔（控制面板里调的那个）+ 双击判定框来认。
    // ★ 必须放在 annotLive 分支之前：选区框好后 annotLive 恒为 true，
    //   原先那次判定写在 annotLive 之后 → 永远走不到，"双击完成"形同不存在。
    {
        const auto now = GetTickCount64();
        const bool dbl = (now - lastDownTime <= GetDoubleClickTime())
            && std::abs(pos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
            && std::abs(pos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
        lastDownTime = now;
        lastDownPos = pos;
        if (dbl && !demoMode && cutMask && cutMask->hasRect()
            && !hasAnnotShapes() && stage == CapStage::Adjust) {
            copyToClipboard();
            return;
        }
    }
    if (annotLive) {
        if (beginMaskResize(pos)) return;
        annotDown(pos, isRight);
        return;
    }
    // 演示画布：没开标注会话（默认工具=无）时画布不响应点击，别退化成"拖选区"
    if (demoMode) return;
    if (stage == CapStage::Select) {
        isPress = true;
        selectDragging_ = false;
        cutMask->startMakeRect(pos);
        refresh();
    }
    else if (stage == CapStage::Adjust) {
        isPress = true;
        cutMask->startAdjust(pos);
        setPixPos(pos);
        if (cutMask->adjustingHit() != MaskHit::None) {
            SetCapture(hwnd); // 拖边时结果窗盖在选区上，无 Capture 则抬起被抢走、isPress 卡死
            setToolChromeHidden(true);
        }
        refresh();
    }
    else if (videoCanAdjust()) {
        isPress = true;
        cutMask->startAdjust(pos);
        setPixPos(pos);
        if (cutMask->adjustingHit() != MaskHit::None) {
            SetCapture(hwnd);
            setToolChromeHidden(true);
        }
        refresh();
    }
}

void WinCap::updateInfoBarTip(POINT pos)
{
	if (!tip || !cutMask || cutMask->hideLabel) {
		if (tip && infoTipHit_ != InfoHit::None) {
			tip->hide();
			infoTipHit_ = InfoHit::None;
		}
		return;
	}
	const auto hit = cutMask->hitInfoControl(pos);
	const wchar_t* text = nullptr;
	switch (hit) {
	case InfoHit::Lock: text = L"锁定比例"; break;
	case InfoHit::Radius: text = L"圆角"; break;
	case InfoHit::Shadow: text = L"投影"; break;
	default: break;
	}
	if (!text) {
		if (infoTipHit_ != InfoHit::None) {
			tip->hide();
			infoTipHit_ = InfoHit::None;
		}
		return;
	}
	float sx = 0.f, sy = 0.f;
	if (!cutMask->infoTipAnchor(hit, sx, sy)) return;
	// 同一个控件就别每次鼠标移动都重挂一遍：Tip 的锚点在信息栏上是固定的，
	// 旧代码在信息栏上滑动时会把 showAt（重排 + 定位 + 显示）跑满每一个 WM_MOUSEMOVE
	if (hit == infoTipHit_) return;
	// Tip 用 Node* 做归属比较；信息栏无 Node，用稳定哨兵指针
	static char tipIds[3]{};
	Ling::Node* owner = nullptr;
	if (hit == InfoHit::Lock) owner = reinterpret_cast<Ling::Node*>(&tipIds[0]);
	else if (hit == InfoHit::Radius) owner = reinterpret_cast<Ling::Node*>(&tipIds[1]);
	else owner = reinterpret_cast<Ling::Node*>(&tipIds[2]);
	infoTipHit_ = hit;
	tip->showAt(owner, sx, sy, text);
}

void WinCap::onMove(POINT pos)
{
    if (cutMask && cutMask->infoInteracting()) {
        cutMask->onInfoMove(pos);
        return;
    }
    // 演示画布不画信息栏（也不该能命中它），别再弹那圈的悬停提示
    if (!demoMode) updateInfoBarTip(pos);
    // 长截图：预览交互优先于标注（编辑态也要能拖/悬停预览）
    if (stage == CapStage::Long && capLong) {
        capLong->onMove(pos);
        if (capLong->isPreviewPanning() || capLong->isSelViewportPanning()
			|| capLong->hitPreview(pos)) {
            setCursor();
            return;
        }
        if (annotLive && capLong->isEditMode()) {
            annotMove(pos);
            setCursor();
            return;
        }
        setCursor();
        return;
    }
    if (annotLive) {
        if (maskDragging && isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            cutMask->adjust(cur);
            if (cutMask->isResizingEdge()) setPixPos(cur);
            syncQrcodePanel();
            refresh();
            return;
        }
        annotMove(pos);
        setCursor();
        return;
    }
    if (stage == CapStage::Select) {
        if (isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            cutMask->makeRect(cur);
            // 超过单击抖动阈值才算拖动 → 才出放大镜
            if (!selectDragging_) {
                const int moved = std::abs(cur.x - lastDownPos.x) + std::abs(cur.y - lastDownPos.y);
                if (moved > 5) selectDragging_ = true;
            }
            if (selectDragging_) setPixPos(cur);
            refresh();
        }
        else {
            cutMask->highlight(pos);
            // 查找元素阶段：悬停即显示放大镜并跟随光标
            if (Setting::get()->getFindWindowElements() && !demoMode) {
                setPixPos(pos);
                refresh();
            }
        }
        setCursor();
    }
    else if (stage == CapStage::Adjust) {
        if (isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            cutMask->adjust(cur);
            if (cutMask->isResizingEdge()) setPixPos(cur);
            syncQrcodePanel();
            refresh();
        }
        setCursor();
    }
    else if (videoCanAdjust()) {
        if (isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            cutMask->adjust(cur);
            if (cutMask->isResizingEdge()) setPixPos(cur);
            syncQrcodePanel();
            refresh();
        }
        setCursor();
    }
}

void WinCap::onUp(POINT pos, bool isRight)
{
    if (isRight) {
        if (deferExitAfterRightUp) {
            deferExitAfterRightUp = false;
            ReleaseCapture();
            close();
        }
        return;
    }
    if (cutMask && cutMask->infoInteracting()) {
        cutMask->onInfoUp();
        ReleaseCapture();
        return;
    }
    if (stage == CapStage::Long && capLong) {
        if (capLong->isPreviewPanning()) {
            capLong->onUp(pos);
            return;
        }
        if (annotLive && capLong->isEditMode()) {
            annotUp(pos, isRight);
            return;
        }
        if (capLong->isSelViewportPanning()) {
            capLong->onUp(pos);
            ReleaseCapture();
            return;
        }
        capLong->onUp(pos);
        return;
    }
    if (annotLive) {
        if (maskDragging) {
            maskDragging = false;
            isPress = false;
            ReleaseCapture();
            if (cutMask) cutMask->endAdjust();
            syncViewportFilters();
            setToolChromeHidden(false);
            refresh();
            return;
        }
        annotUp(pos, isRight);
        return;
    }
    // 演示画布：没有标注会话时点击已在 onDown 被吃掉，这里不要再走"改选区收尾"
    if (demoMode) return;
    if (stage == CapStage::Select) {
        // 没有按下就来的抬起：托盘菜单项是在 mouse **down** 上触发命令的（Ling::Button::onDown），
        // 菜单随之销毁，用户那一下抬手就落到刚 show() 出来的本窗口上。而"查找元素"悬停
        // 已经把 maskRect 铺好了（CutMask::setHoverTarget / snapHoverTarget），旧代码只看
        // hasRect() 就会把它当成"框选完成"：截图会话直接带着悬停元素的框进 Adjust，
        // 托盘「屏幕录制」更是一路冲进录制 —— 表现就是"没有查找元素和框选，点了就没了"。
        // 所以这里必须要求"这一次确实按下过"。
        if (!isPress) return;
        isPress = false;
        selectDragging_ = false;
        if (!cutMask->hasRect()) return;
        // 托盘「屏幕录制」：点一下、抖一下不算框选，别拿 1 像素选区直接开录
        if (pendingRecord) {
            const auto& mr = cutMask->maskRect;
            if (mr.right - mr.left < 4.f || mr.bottom - mr.top < 4.f) return;
        }
        if (enterByArg()) return;
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            startPin();
            return;
        }
        stage = CapStage::Adjust;
        refresh();
        // 托盘「屏幕录制」进来的一次会话：框完直接进录屏，不再弹截图工具栏
        if (pendingRecord) {
            pendingRecord = false;
            startVideo();
            return;
        }
        makeToolCap();
    }
    else if (stage == CapStage::Adjust) {
        isPress = false;
        ReleaseCapture();
        if (cutMask) cutMask->endAdjust();
        setToolChromeHidden(false);
        syncQrcodePanel();
        refresh();
    }
    else if (videoCanAdjust()) {
        isPress = false;
        ReleaseCapture();
        if (cutMask) cutMask->endAdjust();
        setToolChromeHidden(false);
        syncQrcodePanel();
        refresh();
    }
}

// close() 里 DestroyWindow 之后同步触发 onDestroy，而这条路径通常是从某个工具条的按钮
// 回调里一路进来的（工具条是 WinCap / CapLong / CapVideo 的成员）。在这里直接
// winCap.reset() 就是 use-after-free，所以窗口句柄立即销毁，C++ 对象的释放推迟到下一轮消息循环。
void WinCap::onClosed()
{
    if (isClosed) return;
    isClosed = true;
    deferExitAfterRightUp = false;
    CaptureEsc::release(hwnd);
    annotLive = false;
    if (capVideo) capVideo->dispose();
    if (capLong) capLong->dispose();
    if (toolQrcode) { toolQrcode->close(); toolQrcode.reset(); }
    if (toolCap) toolCap->close();
    if (toolSub) toolSub->close();
    Ling::App::get()->dq.TryEnqueue([]() {
        winCap.reset();
        if (!WinPin::hasWindow()) {
            if (Ling::App::get()->args[L"--auto-quit"] == L"true") {
                Ling::App::get()->quit(0);
            }
            else {
                Update::checkLater();
            }
        }
    });
}

void WinCap::stopIfRecording()
{
    if (!winCap || !winCap->capVideo) return;
    // 正在录制：先停止编码线程，避免退出时线程与设备卡死
    winCap->capVideo->stop();
}

void WinCap::makeToolCap()
{
    if (toolCap) {
        layoutTool(toolCap.get());
        toolCap->show();
        return;
    }
    clearMainToolUserPlaced();
    toolCap = std::make_unique<ToolCap>(this);
    toolCap->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
    toolCap->bindOwner(hwnd);
    layoutTool(toolCap.get());
}

bool WinCap::enterByArg()
{
    auto& args = Ling::App::get()->args;
    auto it = args.find(L"--enter");
    if (it == args.end()) return false;
    auto& val = it->second;
    stage = CapStage::Adjust;
    refresh();
    if (val == L"long") startLong();
    else if (val == L"video") startVideo();
    else if (val == L"ocr") startOcr();
    else if (val == L"qr" || val == L"qrcode") startQrcode();
    else if (val == L"pin") startPin();
    else return false;
    return true;
}

void WinCap::relayoutTool()
{
    if (capLong) capLong->layoutTool();
    else if (capVideo) capVideo->layoutTool();
    else if (toolCap) layoutTool(toolCap.get());
}

void WinCap::layoutTool(Ling::WinBase* tool)
{
    if (!tool) return;
    if (isMainToolUserPlaced()) return;
    const int toolW = (int)(tool->w + 0.5f);
    const int toolH = (int)(tool->h + 0.5f);
    // 演示模式：工具栏固定在**屏幕顶部居中**（全屏画布工具栏的默认吸附位）
    if (demoMode) {
        const int gap = (int)(12.f * tool->dpi + 0.5f);
        const int toolX = x + (int)((w - toolW) * 0.5f);
        const int toolY = y + gap;
        if (tool->x != toolX || tool->y != toolY) tool->setPosition(toolX, toolY);
        return;
    }
    // shadowPad 曾为投影透明边；现为 0，工具窗即可视栏面
    const int pad = (int)(ToolbarTheme::shadowPad * tool->dpi + 0.5f);
    const int visualH = (std::max)(1, toolH - pad * 2);
    const int visualW = (std::max)(1, toolW - pad * 2);

    const int maskLeftScr = x + (int)cutMask->maskRect.left;
    const int maskTopScr = y + (int)cutMask->maskRect.top;
    const int maskRightScr = x + (int)cutMask->maskRect.right;
    const int maskBottomScr = y + (int)cutMask->maskRect.bottom;

    RECT maskScrRect{ maskLeftScr, maskTopScr, maskRightScr, maskBottomScr };
    HMONITOR hMon = MonitorFromRect(&maskScrRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(hMon, &mi);

    const int gap = (int)(cutMask->strokeWidth + 2.f * tool->dpi + 0.5f);
    const bool fitBelow = (maskBottomScr + gap + visualH) <= mi.rcWork.bottom;
    const bool fitAbove = (maskTopScr - gap - visualH) >= mi.rcWork.top;

    // 可视栏左缘对齐选区左缘（与左上角信息栏同侧）
    int toolX = maskLeftScr - pad;
    int toolY = 0;
    if (fitBelow) {
        toolY = maskBottomScr + gap - pad;
    }
    else if (fitAbove) {
        toolY = maskTopScr - gap - toolH + pad;
    }
    else {
        const int overlapPad = (int)(3.f * tool->dpi + 0.5f);
        toolX = maskLeftScr - pad + overlapPad;
        toolY = maskBottomScr - toolH + pad - overlapPad;
    }

    if (toolX < mi.rcWork.left) toolX = mi.rcWork.left;
    if (toolX + toolW > mi.rcWork.right) toolX = mi.rcWork.right - toolW;
    if (tool->x == toolX && tool->y == toolY) return;
    tool->setPosition(toolX, toolY);
}

void WinCap::enterLiveStage()
{
    // 底图是拖框那一刻的静态截图，从这里开始不能再画它 ——
    // 否则录屏和滚动截图从屏幕上拿到的都是这张死图。只留遮罩，选区内是透明的洞。
    hideScreenImg = true;
    // 尺寸标签放不下时会折回选区内部（全屏必然如此），那就会被录进去 / 滚进长图。
    // 选区这时已经定死了，标签也没什么可看的，直接不画 —— 这样选区内就真的什么都不画了，
    // 不必再拿 WDA_EXCLUDEFROMCAPTURE 去摘整块屏幕大小的宿主窗口
    cutMask->hideLabel = true;
    if (toolCap) toolCap->hide();
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    refresh();
}

bool WinCap::videoCanAdjust() const
{
	return stage == CapStage::Video && capVideo && !capVideo->isRecording() && !isMouseTransparent;
}

bool WinCap::annotRightClick()
{
	if (handleLayerCancel(false))
		requestExitAfterRightUp();
	return true;
}

bool WinCap::annotSkipHitSelect() const
{
	// 录屏/长图编辑且已选标注工具（含橡皮）：不点选已有标注
	if (stage == CapStage::Video && !getCurToolId().empty()) return true;
	if (stage == CapStage::Long && capLong && capLong->isEditMode() && !getCurToolId().empty()) return true;
	return false;
}

bool WinCap::annotEmptyToolDrag(POINT pos)
{
	if (stage != CapStage::Long || !capLong || !capLong->isEditMode()) return false;
	if (!capLong->isSelViewportPanning()) {
		if (!capLong->beginSelViewportPan(annotPressPos)) return false;
	}
	capLong->onMove(pos);
	return true;
}

void WinCap::annotAfterEmptyToolUp()
{
	if (capLong && capLong->isSelViewportPanning()) {
		POINT pos{};
		GetCursorPos(&pos);
		ScreenToClient(hwnd, &pos);
		capLong->onUp(pos);
	}
}

void WinCap::requestExitAfterRightUp()
{
	deferExitAfterRightUp = true;
	if (hwnd) SetCapture(hwnd);
}

bool WinCap::handleLayerCancel(bool fromToolbar)
{
	// 对齐 Tauri2 OverlayWindow.handleLayerCancel（右键分层；Esc 另走直接退出）
	// 返回 true = 应退出截图（由 onUp 再 close，避免桌面吃到右键抬起）
	if (stage == CapStage::Long) {
		if (capLong && capLong->isEditMode()) {
			if (editingText) {
				editingText->finishEdit();
				return false;
			}
			if (annotMouseDown && newShape && history) {
				history->removeShape(newShape);
				newShape = nullptr;
				shapeHover = nullptr;
				annotMouseDown = false;
				annotDragIndex = -1;
				ReleaseCapture();
				refresh();
				return false;
			}
			if (shapeHover) {
				clearAnnotSelection();
				return false;
			}
			if (!getCurToolId().empty()) {
				if (capLong) capLong->cancelAnnotSelect();
				else clearLongAnnotTool();
				return false;
			}
			// 退出编辑栏，回到长截图栏
			toggleLongEdit();
			return false;
		}
		return true;
	}
	// 录屏：先退标注层，再退选区拖动，最后退出
	if (stage == CapStage::Video) {
		if (editingText) {
			editingText->finishEdit();
			return false;
		}
		if (annotMouseDown && newShape && history) {
			history->removeShape(newShape);
			newShape = nullptr;
			shapeHover = nullptr;
			annotMouseDown = false;
			annotDragIndex = -1;
			ReleaseCapture();
			refresh();
			return false;
		}
		if (shapeHover) {
			clearAnnotSelection();
			return false;
		}
		if (!getCurToolId().empty()) {
			if (capVideo) capVideo->cancelAnnotSelect();
			else clearVideoAnnotTool();
			return false;
		}
		if (isPress && cutMask) {
			cutMask->cancelAdjust();
			isPress = false;
			ReleaseCapture();
			setToolChromeHidden(false);
			return false;
		}
		return true;
	}
	// 1) 文字编辑 → 完成并保留已输入
	if (editingText) {
		editingText->finishEdit();
		return false;
	}
	// 2) 落笔草稿未抬起 → 丢掉
	if (annotMouseDown && newShape && history) {
		history->removeShape(newShape);
		newShape = nullptr;
		shapeHover = nullptr;
		annotMouseDown = false;
		annotDragIndex = -1;
		ReleaseCapture();
		refresh();
		return false;
	}
	// 3) 选中标注 → 取消选中（先于取消工具）
	if (shapeHover) {
		clearAnnotSelection();
		return false;
	}
	// 4) 有标注工具 → 取消工具
	if (toolCap && !toolCap->curId.empty()) {
		toolCap->cancelSelect();
		refresh();
		return false;
	}
	// 工具栏上右键：最多退到未选工具，不重框选、不退出
	if (fromToolbar) return false;
	// 5) 改选区拖拽中 → 恢复原选区
	if (isPress && (stage == CapStage::Adjust || videoCanAdjust()) && cutMask) {
		cutMask->cancelAdjust();
		isPress = false;
		ReleaseCapture();
		setToolChromeHidden(false);
		return false;
	}
	// 演示画布：右键最多退到"取消选中/取消工具"（上面 1–5 层），
	// 不能再往下走 enterReselect —— 那会把全屏画布拆回"查找元素"框选态
	if (demoMode) return false;
	// 6) 已框选（Adjust/标注）→ 回到查找元素
	if (stage == CapStage::Adjust || annotLive) {
		enterReselect();
		return false;
	}
	// 7) 查找元素态 → 退出截图
	return true;
}

void WinCap::enterReselect()
{
	// 回到查找元素：清标注与选区
	annotLive = false;
	clearMainToolUserPlaced();
	clearAllShapes();
	if (toolCap) {
		toolCap->cancelSelect();
		toolCap->hide();
	}
	if (toolSub) toolSub->hideTools();
	if (cutMask) cutMask->clearRect();
	stage = CapStage::Select;
	isPress = false;
	selectDragging_ = false;
	refresh();
}

void WinCap::startAnnotate(const std::wstring& toolId)
{
	if (!cutMask->hasRect()) return;
	ensureHistory();
	if (toolSub) toolSub->bindOwner(hwnd);
	annotLive = true;
	// 标注阶段仍显示选区尺寸信息栏；长图/录屏才在 enterLiveStage 里隐藏
	cutMask->hideLabel = false;
	// 全屏同表面：不挖洞、不建选区 Pin；只换笔；保留已选标注
	if (toolCap) toolCap->selectAnnotTool(toolId);
	presentSelectedAnnotStyle();
	layoutTools();
	refresh();
}

void WinCap::startPin(const std::wstring& /*toolId*/)
{
	if (!cutMask->hasRect()) return;
	std::vector<BYTE> pixels;
	int cw = 0, ch = 0;
	if (!exportSelection(pixels, cw, ch)) return;
	const int px = x + (int)cutMask->maskRect.left;
	const int py = y + (int)cutMask->maskRect.top;
	annotLive = false;
	WinPin::initFromData(px, py, cw, ch, pixels);
	close();
}

void WinCap::layoutTools()
{
	if (stage == CapStage::Video && capVideo) {
		capVideo->layoutTool();
	}
	else if (stage == CapStage::Long && capLong) {
		capLong->layoutTool();
	}
	else if (toolCap) {
		layoutTool(toolCap.get());
	}
	if (!toolSub || !annotLive || !toolSub->hasContent() || getCurToolId().empty()) {
		if (toolSub && getCurToolId().empty()) toolSub->hideTools();
		return;
	}
	RECT winRect{ x, y, x + (int)w, y + (int)h };
	const RECT wa = ToolbarChrome::workAreaNear(winRect);
	// 演示模式同样跟着触发工具锚定（居中于按钮 + 箭头指向它）：先按主栏居中再改窗口 x
	// 会把 updatePosition 算好的 arrowX 甩掉，表现为「属性栏既不对准工具、箭头也乱指」
	toolSub->updatePosition(wa);
	HWND bar = nullptr;
	if (stage == CapStage::Video && capVideo)
		bar = capVideo->toolHwnd();
	else if (stage == CapStage::Long && capLong)
		bar = capLong->toolHwnd();
	else if (toolCap) bar = toolCap->hwnd;
	if (bar && toolSub->hwnd) {
		SetWindowPos(toolSub->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		SetWindowPos(bar, toolSub->hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void WinCap::onHistoryChanged()
{
	AnnotHost::onHistoryChanged();
	if (capVideo) capVideo->notifyHistoryChanged();
	if (capLong) capLong->notifyHistoryChanged();
}

bool WinCap::queryMainBarAnchor(MainBarAnchor& out) const
{
	if (stage == CapStage::Video && capVideo) {
		return capVideo->queryBarAnchor(out.x, out.y, out.w, out.h, out.btnCenterX, out.dpi);
	}
	if (stage == CapStage::Long && capLong) {
		return capLong->queryBarAnchor(out.x, out.y, out.w, out.h, out.btnCenterX, out.dpi);
	}
	return AnnotHost::queryMainBarAnchor(out);
}

void WinCap::startVideoAnnotate(const std::wstring& toolId)
{
	if (stage != CapStage::Video || toolId.empty()) return;
	setMouseTransparent(false);
	ensureHistory();
	if (toolSub) toolSub->bindOwner(hwnd);
	annotLive = true;
	// 保留已选标注：点所属工具应出属性栏，不应取消选中
	setForcedToolId(toolId);
	if (!toolSub) return;
	if (toolId == L"rect") toolSub->showRectTools();
	else if (toolId == L"arrow") toolSub->showArrowTools();
	else if (toolId == L"pen") toolSub->showPenTools();
	else if (toolId == L"text") toolSub->showTextTools();
	else if (toolId == L"number") toolSub->showNumberTools();
	else if (toolId == L"mosaic") toolSub->showMosaicTools();
	else if (toolId == L"eraser") toolSub->showEraserTools();
	else toolSub->hideTools();
	presentSelectedAnnotStyle();
	layoutTools();
	refresh();
}

void WinCap::clearVideoAnnotTool()
{
	clearForcedToolId();
	if (toolSub) toolSub->hideTools();
	// 取消工具后仍保留选中，方便继续改属性（点空白再清）
	refresh();
}

void WinCap::toggleLongEdit()
{
	if (stage != CapStage::Long || !capLong) return;
	const bool next = !capLong->isEditMode();
	if (next) {
		ensureHistory();
		if (toolSub) toolSub->bindOwner(hwnd);
	}
	else {
		clearLongAnnotTool();
		annotLive = false;
	}
	capLong->setEditMode(next);
	if (next) {
		// 进入编辑态先不选工具，点画笔等再 startLongAnnotate
		annotLive = true;
	}
	layoutTools();
	refresh();
}

void WinCap::startLongAnnotate(const std::wstring& toolId)
{
	if (stage != CapStage::Long || !capLong || toolId.empty()) return;
	if (!capLong->isEditMode()) capLong->setEditMode(true);
	ensureHistory();
	if (toolSub) toolSub->bindOwner(hwnd);
	annotLive = true;
	// 保留已选标注：点所属工具应出属性栏，不应取消选中
	setForcedToolId(toolId);
	if (!toolSub) return;
	if (toolId == L"rect") toolSub->showRectTools();
	else if (toolId == L"arrow") toolSub->showArrowTools();
	else if (toolId == L"pen") toolSub->showPenTools();
	else if (toolId == L"text") toolSub->showTextTools();
	else if (toolId == L"number") toolSub->showNumberTools();
	else if (toolId == L"mosaic") toolSub->showMosaicTools();
	else if (toolId == L"eraser") toolSub->showEraserTools();
	else toolSub->hideTools();
	presentSelectedAnnotStyle();
	layoutTools();
	refresh();
}

void WinCap::clearLongAnnotTool()
{
	clearForcedToolId();
	if (toolSub) toolSub->hideTools();
	// 取消工具后仍保留选中，方便继续改属性（点空白再清）
	refresh();
}

void WinCap::setRecordPaused(bool on)
{
	if (capVideo) capVideo->setPaused(on);
}

void WinCap::beginVideoCapture()
{
	hideScreenImg = true;
	if (cutMask) cutMask->hideLabel = true;
	// 默认不穿透，便于边录边标；穿透按钮后续再开
	refresh();
}

void WinCap::startLong()
{
    if (stage != CapStage::Adjust || !cutMask->hasRect()) return;
    stage = CapStage::Long;
    enterLiveStage();
    ensureNoMousePierce(); // 长截图全程不允许鼠标穿透
    capLong = std::make_unique<CapLong>(this);
    capLong->makeTool();
}

void WinCap::startVideo()
{
    if (stage != CapStage::Adjust || !cutMask || !cutMask->hasRect()) return;

    if (annotLive) {
        annotLive = false;
        clearAnnotSelection();
        if (toolSub) toolSub->hideTools();
        if (toolCap) toolCap->cancelSelect();
    }

    stage = CapStage::Video;
    // 与 startLong 同路径：收起截图栏 + live 遮罩；开录前不穿透，选区仍可调
    enterLiveStage();
    if (cutMask) cutMask->hideLabel = false;
    clearMainToolUserPlaced();
    capVideo = std::make_unique<CapVideo>(this);
    capVideo->makeTool();
}

void WinCap::startMp4(bool useSpeaker, bool useMic)
{
    beginVideoCapture();
    if (capVideo) capVideo->startMp4(useSpeaker, useMic);
}

void WinCap::startGif()
{
    beginVideoCapture();
    if (capVideo) capVideo->startGif();
}

std::wstring WinCap::stopRecord()
{
	// 工具栏已先 hide；属性栏是独立顶层窗，须在 join 编码线程前立刻收掉，否则会空挂一阵
	if (toolSub) toolSub->hideTools();
	clearForcedToolId();
	annotLive = false;
	return capVideo ? capVideo->stop() : L"";
}

void WinCap::layoutLongTool()
{
    if (capLong) capLong->layoutTool();
}

void WinCap::longToggle()
{
    if (capLong) capLong->toggleCapture();
}

void WinCap::longPreviewPanChanged()
{
	if (capLong) capLong->onPreviewPanChanged();
}

bool WinCap::longSaveToFile()
{
    return capLong ? capLong->saveToFile() : false;
}

void WinCap::longCopyToClipboard()
{
    if (capLong) capLong->copyToClipboard();
}

void WinCap::hollowWin()
{
    HRGN rgn1 = CreateRectRgn(0, 0, (int)w, (int)h);
    auto& r = cutMask->maskRect;
    HRGN rgn2 = CreateRectRgn((int)r.left, (int)r.top, (int)r.right, (int)r.bottom);
    CombineRgn(rgn1, rgn1, rgn2, RGN_DIFF);
    DeleteObject(rgn2);
    if (SetWindowRgn(hwnd, rgn1, TRUE) == 0) {
        DeleteObject(rgn1);
    }
}

void WinCap::restoreWin()
{
    SetWindowRgn(hwnd, NULL, TRUE);
}

void WinCap::ensureNoMousePierce()
{
	if (!hwnd) return;
	restoreWin();
	if (isMouseTransparent) setMouseTransparent(false);
	else {
		LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
		if (ex & WS_EX_TRANSPARENT) {
			ex &= ~WS_EX_TRANSPARENT;
			SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
			SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		}
	}
}

void WinCap::setMouseTransparent(bool transparent)
{
    isMouseTransparent = transparent;
    if (!hwnd) return;
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (transparent) {
        exStyle |= WS_EX_LAYERED;
        exStyle |= WS_EX_TRANSPARENT;
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        isPress = false;
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        CaptureEsc::claimTab(hwnd);
    }
    else {
        exStyle &= ~WS_EX_TRANSPARENT;
        exStyle &= ~WS_EX_LAYERED;
        CaptureEsc::releaseTab(hwnd);
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (capVideo) capVideo->syncPierceBtn(transparent);
}

// 文字识别：进程内 OcrService + 选区浮层
void WinCap::startOcr()
{
	// 再点一次同一个按钮 = 取消：收起识别面板（结果留在临时缓存里，下次点同一块选区直接就出来，
	// 不用再等一次识别）。以前这里会再跑一遍识别 —— 同步的几百毫秒，看着就像点了没反应/失败。
	if (toolOcr) {
		clearOcrPanel();
		return;
	}
	startOcrInternal(false);
}

void WinCap::startTranslate()
{
	startOcrInternal(true);
}

void WinCap::startOcrInternal(bool translateMode)
{
	if (!cutMask || !cutMask->hasRect()) return;
	const D2D1_RECT_F sel = cutMask->maskRect;

	std::vector<BYTE> pixels;
	int cw{ 0 }, ch{ 0 };
	if (!exportSelection(pixels, cw, ch)) return;

	OcrResult result;
	if (g_ocrCacheValid && sameOcrRect(g_ocrCacheRect, sel)) {
		result = g_ocrCache;   // 同一块选区：识别结果直接用临时缓存（省掉识别那几百毫秒）
	}
	else {
		result = OcrService::instance().recognize(cw, ch, pixels.data());
		g_ocrCache = result;
		g_ocrCacheRect = sel;
		g_ocrCacheValid = true;
	}

	toolQrcode.reset();
	toolOcr = std::make_unique<ToolOcr>(this, result, translateMode, std::move(pixels), cw, ch);
	toolOcr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	SetWindowLongPtr(toolOcr->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hwnd));
	syncOcrPanel();
	App::excludeFromCapture(toolOcr->hwnd);

	if (result.ok && !result.text.empty()) {
		Ling::Util::setTextToClipboard(result.text);
	}

	if (translateMode && result.ok) {
		std::vector<std::wstring> segments;
		segments.reserve(result.regions.size());
		for (auto& reg : result.regions) segments.push_back(reg.text);
		if (segments.empty() && !result.text.empty()) segments.push_back(result.text);

		TranslateRequest req;
		req.provider = Setting::get()->getTranslateProvider();
		req.sourceLang = Setting::get()->getTranslateSourceLang();
		req.targetLang = Setting::get()->getTranslateTargetLang();

		HWND host = hwnd;
		TranslateService::instance().translateSegments(segments, req,
			[host](const TranslateResult& tr) {
				Ling::App::get()->dq.TryEnqueue([host, tr]() {
					auto* cap = WinCap::get();
					if (!cap || cap->hwnd != host) return;
					cap->finishOcrTranslate(tr);
				});
			});
	}
	refresh();
}

void WinCap::finishOcrTranslate(const TranslateResult& tr)
{
	if (!toolOcr) return;
	if (!tr.ok) {
		toolOcr->showBanner(
			std::wstring(L"翻译失败：") + (tr.error.empty() ? L"未知错误" : tr.error), true);
		return;
	}
	std::vector<std::wstring> lines;
	std::wistringstream ss(tr.text);
	std::wstring line;
	while (std::getline(ss, line)) lines.push_back(line);
	if (lines.empty()) lines.push_back(tr.text);
	toolOcr->applyTranslate(lines);
	Ling::Util::setTextToClipboard(tr.text);
	toolOcr->clearBanner();
}

void WinCap::syncOcrPanel()
{
	if (!toolOcr || !cutMask || !cutMask->hasRect() || !toolOcr->hwnd) return;
	toolOcr->syncToMask(cutMask->maskRect, x, y, dpi);
}

void WinCap::clearOcrPanel()
{
	toolOcr.reset();
	refresh();
}

void WinCap::syncQrcodePanel()
{
	if (!toolQrcode || !cutMask || !cutMask->hasRect() || !toolQrcode->hwnd) return;
	toolQrcode->syncToMask(cutMask->maskRect, x, y, dpi);
}

void WinCap::startQrcode()
{
	std::vector<BYTE> pixels;
	int cw{ 0 }, ch{ 0 };
	if (!exportSelection(pixels, cw, ch)) return;
	auto text = Util::decodeQrCode(cw, ch, pixels.data());
	// 对齐参考：结果盖在选区上，不弹系统框；主工具栏保持可见
	if (!cutMask || !cutMask->hasRect()) {
		close();
		return;
	}
	toolQrcode = std::make_unique<ToolQrcode>(this, std::move(text));
	const int left = (int)std::lround(cutMask->maskRect.left);
	const int top = (int)std::lround(cutMask->maskRect.top);
	const int right = (int)std::lround(cutMask->maskRect.right);
	const int bottom = (int)std::lround(cutMask->maskRect.bottom);
	const int pw = (std::max)(1, right - left);
	const int ph = (std::max)(1, bottom - top);
	toolQrcode->setSize((float)pw / dpi, (float)ph / dpi);
	toolQrcode->setPosition(x + left, y + top);
	toolQrcode->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	SetWindowLongPtr(toolQrcode->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hwnd));
	syncQrcodePanel();
	syncOcrPanel();
	refresh();
}

void WinCap::saveToFile(bool keepOpen)
{
    std::vector<BYTE> pixels;
    int cw{ 0 }, ch{ 0 };
    if (!exportSelection(pixels, cw, ch)) return;
    auto setToolTopmost = [this](bool topmost) {
        if (!toolCap || !toolCap->hwnd) return;
        SetWindowPos(toolCap->hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    setToolTopmost(false);
    auto path = Util::getSaveFilePath(hwnd);
    if (path.empty()) {
        setToolTopmost(true);
        return;
    }
    if (Util::saveToFile(path, cw, ch, pixels.data())) {
        // 记一条截图历史（只在会话收工时；keepOpen 的中间态复制不算）
        if (!keepOpen) Setting::get()->addHistoryItem(pixels, cw, ch, L"capture");
        if (!keepOpen) close();
    }
    else {
        setToolTopmost(true);
    }
}

void WinCap::copyToClipboard(bool keepOpen)
{
    // 文字识别面板开着时，「复制 / 完成」复制的是识别出来的文字，而不是截图图片
    if (toolOcr && toolOcr->result().ok && !toolOcr->result().text.empty()) {
        Ling::Util::setTextToClipboard(toolOcr->result().text);
        if (!keepOpen) close();
        return;
    }
    std::vector<BYTE> pixels;
    int cw{ 0 }, ch{ 0 };
    if (!exportSelection(pixels, cw, ch)) return;
    Util::saveToClipboard(cw, ch, pixels.data());
    // 记一条截图历史（只在会话收工时；演示画布 keepOpen=true 不算）
    if (!keepOpen) Setting::get()->addHistoryItem(pixels, cw, ch, L"capture");
    if (!keepOpen) close();
}

bool WinCap::exportSelection(std::vector<BYTE>& pixels, int& cw, int& ch)
{
    if (!cutMask->hasRect() || !screenImg) return false;
    D2D1_SIZE_U size{};
    if (annotLive && history && !history->shapes.empty()) {
        if (!getAnnotPixels(pixels, size, &cutMask->maskRect)) return false;
        // 合成结果可能带预乘 alpha，导出当不透明图
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }
    else {
        auto& maskRect = cutMask->maskRect;
        const UINT32 cutW = (UINT32)(maskRect.right - maskRect.left);
        const UINT32 cutH = (UINT32)(maskRect.bottom - maskRect.top);
        if (cutW == 0 || cutH == 0) return false;
        D2D1_BITMAP_PROPERTIES1 prop{
            .pixelFormat{ screenImg->GetPixelFormat() },
            .dpiX{ 96.0f }, .dpiY{ 96.0f },
            .bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
        };
        ComPtr<ID2D1Bitmap1> cpuBmp;
        auto hr = Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(cutW, cutH), nullptr, 0, &prop, cpuBmp.GetAddressOf());
        if (FAILED(hr)) return false;
        auto start = D2D1::Point2U(0, 0);
        auto rect = D2D1::RectU((UINT32)maskRect.left, (UINT32)maskRect.top, (UINT32)maskRect.left + cutW, (UINT32)maskRect.top + cutH);
        if (FAILED(cpuBmp->CopyFromBitmap(&start, screenImg.Get(), &rect))) return false;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        const UINT32 rowBytes = cutW * 4;
        pixels.resize((size_t)rowBytes * cutH);
        for (UINT32 row = 0; row < cutH; row++) {
            auto dst = pixels.data() + (size_t)row * rowBytes;
            CopyMemory(dst, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
            for (UINT32 i = 3; i < rowBytes; i += 4) dst[i] = 255;
        }
        cpuBmp->Unmap();
        size = D2D1::SizeU(cutW, cutH);
    }
    cw = (int)size.width;
    ch = (int)size.height;
    if (showCursor && cursorValid && cursorW > 0 && cursorH > 0 && cw > 0 && ch > 0)
        compositeCursor(pixels, cw, ch, (int)cutMask->maskRect.left, (int)cutMask->maskRect.top);
    return cutMask->applyExportEffects(pixels, cw, ch);
}

// 「显示光标」：会话开始抓一次真实系统光标。老式光标只有 AND 掩码没 alpha，
// DrawIconEx 画进 32bpp DIB 时 alpha 全 0 —— 得自己按掩码补。
void WinCap::captureCursorSnapshot()
{
    cursorValid = false;
    cursorBmp.Reset();
    cursorPix.clear();
    cursorW = cursorH = 0;
    CURSORINFO ci{ sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    auto cleanup = [&] {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    };

    int width = 32, height = 32;
    if (ii.hbmColor) {
        BITMAP bm{};
        if (GetObject(ii.hbmColor, sizeof(bm), &bm)) { width = bm.bmWidth; height = bm.bmHeight; }
    }
    else if (ii.hbmMask) {
        BITMAP bm{};
        if (GetObject(ii.hbmMask, sizeof(bm), &bm)) { width = bm.bmWidth; height = bm.bmHeight / 2; }
    }
    width = std::clamp(width, 1, 256);
    height = std::clamp(height, 1, 256);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        cleanup();
        return;
    }
    HGDIOBJ old = SelectObject(memDc, dib);
    memset(bits, 0, (size_t)width * (size_t)height * 4);
    DrawIconEx(memDc, 0, 0, ci.hCursor, width, height, 0, nullptr, DI_NORMAL);
    cursorPix.assign((size_t)width * (size_t)height * 4, 0);
    memcpy(cursorPix.data(), bits, cursorPix.size());

    // 补 alpha：掩码位 = 1 ⇒ 该点透明；掩码两种布局都试，用"本体必须不透明"挑对的
    bool anyAlpha = false;
    for (size_t i = 3; i < cursorPix.size(); i += 4) { if (cursorPix[i]) { anyAlpha = true; break; } }
    BITMAP bmMask{};
    const bool haveMask = ii.hbmMask && GetObject(ii.hbmMask, sizeof(bmMask), &bmMask)
        && bmMask.bmWidth >= width && bmMask.bmHeight >= height;
    if (!anyAlpha && haveMask) {
        BITMAPINFO mi{};
        mi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        mi.bmiHeader.biWidth = bmMask.bmWidth;
        mi.bmiHeader.biHeight = bmMask.bmHeight;   // bottom-up
        mi.bmiHeader.biPlanes = 1;
        mi.bmiHeader.biBitCount = 1;
        mi.bmiHeader.biCompression = BI_RGB;
        const int stride = ((bmMask.bmWidth + 31) / 32) * 4;
        std::vector<BYTE> maskBits((size_t)stride * bmMask.bmHeight, 0);
        if (GetDIBits(memDc, ii.hbmMask, 0, bmMask.bmHeight, maskBits.data(), &mi, DIB_RGB_COLORS) != 0) {
            const std::vector<BYTE> original = cursorPix;
            const int total = width * height;
            for (int cand = 0; cand < 2; ++cand) {
                const int firstRow = (cand == 0) ? 0 : (bmMask.bmHeight - height);
                cursorPix = original;
                int opaque = 0, mismatch = 0;
                for (int y = 0; y < height; ++y) {
                    const int br = firstRow + (height - 1 - y);
                    if (br < 0 || br >= bmMask.bmHeight) continue;
                    const BYTE* row = maskBits.data() + (size_t)br * stride;
                    BYTE* line = cursorPix.data() + (size_t)y * width * 4;
                    for (int x = 0; x < width; ++x) {
                        const bool transparent = (row[x >> 3] >> (7 - (x & 7))) & 1;
                        const bool hasRgb = (line[x * 4] | line[x * 4 + 1] | line[x * 4 + 2]) != 0;
                        if (transparent) {
                            line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = line[x * 4 + 3] = 0;
                            if (hasRgb) ++mismatch;
                        }
                        else {
                            line[x * 4 + 3] = 255;
                            ++opaque;
                        }
                    }
                }
                if (mismatch == 0 && opaque > 0 && opaque <= total * 3 / 4) break;
                cursorPix = original;
            }
        }
    }

    SelectObject(memDc, old);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    cleanup();

    cursorW = width;
    cursorH = height;
    cursorHotspot = POINT{ (LONG)ii.xHotspot, (LONG)ii.yHotspot };
    cursorScreenPos = ci.ptScreenPos;
    for (size_t i = 3; i < cursorPix.size(); i += 4) {
        if (cursorPix[i]) { cursorValid = true; break; }
    }
}

void WinCap::toggleShowCursor()
{
    showCursor = !showCursor;
    if (showCursor && !cursorValid) captureCursorSnapshot();
    refresh();
}

void WinCap::paintCursor(ID2D1DeviceContext* ctx)
{
    if (!showCursor || !cursorValid || cursorW <= 0 || cursorH <= 0 || !ctx) return;
    if (!cursorBmp) {
        // D2D 用预乘 alpha：把 straight BGRA 转一份出来建位图
        std::vector<BYTE> pm(cursorPix.size());
        for (size_t i = 0; i + 3 < pm.size(); i += 4) {
            const BYTE a = cursorPix[i + 3];
            pm[i] = (BYTE)(cursorPix[i] * a / 255);
            pm[i + 1] = (BYTE)(cursorPix[i + 1] * a / 255);
            pm[i + 2] = (BYTE)(cursorPix[i + 2] * a / 255);
            pm[i + 3] = a;
        }
        D2D1_BITMAP_PROPERTIES1 prop{
            .pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            .dpiX{ 96.0f }, .dpiY{ 96.0f },
            .bitmapOptions{ D2D1_BITMAP_OPTIONS_NONE }
        };
        Ling::D2D::get()->deviceContext->CreateBitmap(
            D2D1::SizeU((UINT32)cursorW, (UINT32)cursorH), pm.data(),
            (UINT32)cursorW * 4, &prop, cursorBmp.GetAddressOf());
    }
    if (!cursorBmp) return;
    POINT local = cursorScreenPos;
    ScreenToClient(hwnd, &local);
    const float l = (float)(local.x - cursorHotspot.x);
    const float t = (float)(local.y - cursorHotspot.y);
    ctx->DrawBitmap(cursorBmp.Get(), D2D1::RectF(l, t, l + cursorW, t + cursorH),
        1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void WinCap::compositeCursor(std::vector<BYTE>& pixels, int w, int h, int originX, int originY)
{
    if (!cursorValid || cursorW <= 0 || cursorH <= 0) return;
    POINT local = cursorScreenPos;
    ScreenToClient(hwnd, &local);
    const int x0 = local.x - cursorHotspot.x - originX;
    const int y0 = local.y - cursorHotspot.y - originY;
    for (int y = 0; y < cursorH; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= h) continue;
        for (int x = 0; x < cursorW; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= w) continue;
            const BYTE* s = cursorPix.data() + ((size_t)y * cursorW + x) * 4;
            const BYTE a = s[3];
            if (a == 0) continue;
            BYTE* d = pixels.data() + ((size_t)dy * w + dx) * 4;
            if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; continue; }
            const int inv = 255 - a;
            d[0] = (BYTE)((s[0] * a + d[0] * inv) / 255);
            d[1] = (BYTE)((s[1] * a + d[1] * inv) / 255);
            d[2] = (BYTE)((s[2] * a + d[2] * inv) / 255);
            d[3] = 255;
        }
    }
}
