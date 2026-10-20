#include "pch.h"
#include "../Tool/ToolCap.h"
#include "../Tool/ToolSub.h"
#include "../Tool/IconCodes.h"
#include "../Tool/ToolbarTheme.h"
#include "../History.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeText.h"
#include "AnnotHost.h"
#include "WinPin.h"
#include "WinCap.h"
#include "PinMenu.h"
#include "../Tool/ToolOcr.h"
#include "../Ocr/OcrService.h"
#include "../Ocr/LwPpOcrEngine.h"
#include "../Translate/BergamotEngine.h"
#include "../Lang.h"
#include "../App.h"
#include "../Setting.h"
#include "../Util.h"
#include "../Update.h"
#include "CaptureEsc.h"

using namespace Microsoft::WRL;
namespace {
	std::vector<std::unique_ptr<WinPin>> winPins;
	// 「隐藏/显示贴图」的临时态：只影响当前这些窗口，不落盘
	bool pinsHidden{ false };

	int clampPos(float val, float size, int min, int max)
	{
		auto upper = max - static_cast<int>(size);
		if (upper < min) upper = min;
		auto result = static_cast<int>(val);
		if (result < min) result = min;
		if (result > upper) result = upper;
		return result;
	}

	bool ptInRect(const D2D1_RECT_F& r, POINT pos)
	{
		return pos.x >= r.left && pos.x < r.right && pos.y >= r.top && pos.y < r.bottom;
	}
}

WinPin::WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data, const std::wstring& initialTool, std::unique_ptr<ToolCap> cap)
	: AnnotHost()
{
	this->x = x;
	this->y = y;
	this->w = (float)w;
	this->h = (float)h;
	if (data) {
		D2D1_BITMAP_PROPERTIES1 props{};
		props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
		props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
		props.dpiX = 96.0f;
		props.dpiY = 96.0f;
		Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data->data(), w * 4, &props, screenImg.GetAddressOf());
	}
	else {
		screenImg = WinCap::get()->getCutImg();
	}
	ensureHistory();
	if (cap) {
		toolCap = std::move(cap);
		toolCap->attachToPin(this, initialTool);
		if (!initialTool.empty()) pendingTool = initialTool;
		// 移交来的截图栏当前是显示态；钉图默认非编辑，先收起（退出编辑同样走 hide）
		if (toolCap->hwnd) toolCap->hide();
	}
	else {
		toolCap = std::make_unique<ToolCap>(this);
		if (!initialTool.empty()) pendingTool = initialTool;
	}
	editing = false;
	opacityPercent = Setting::get()->getPinOpacity();
	onMoved.add([this]() { if (editing) layoutTools(); refreshOcrPanel(); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		applyWinSize();
		if (editing) layoutTools();
	});
	onMouseDown.add([this](POINT pos, BOOL isRight) { this->onDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { this->onMove(pos); });
	onMouseUp.add([this](POINT pos, BOOL isRight) { this->onUp(pos, isRight); });
	onMouseWheel.add([this](POINT pos, float space) {
		if (locked || thumbnail) return;
		if (GetKeyState(VK_CONTROL) & 0x8000) {
			applyScale(scale * (space > 0 ? 1.1f : 1.f / 1.1f), pos);
			return;
		}
		if (editing) {
			// 编辑态普通滚轮仍归标注（避免与标注交互冲突）
			if (!shapeHover) return;
			auto imgPos = toImgPos(pos);
			shapeHover->mouseWheel((float)imgPos.x, (float)imgPos.y, space > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
			return;
		}
		// 非编辑态：悬停滚轮直接缩放。
		// 锚点默认取鼠标所在一侧（边/角），画面朝反向伸缩；开启「以鼠标为中心缩放」时
		// 直接以光标点为锚（光标下的像素保持不动）。
		const float steps = space / 60.f;   // space = 格数 * 60 * dpi
		const float factor = std::pow(1.1f, steps);
		if (zoomMode == ZoomMode::Crop) {
			cropScale = std::clamp(cropScale * factor, 1.f, 10.f);
			refresh();
			return;
		}
		if (Setting::get()->getPinZoomAtCursor()) {
			applyScale(scale * factor, pos);
			return;
		}
		// 九宫格取锚点：左/中/右 × 上/中/下 → 边中点或角点（中间带取窗口 1/3）。
		const int zx = (pos.x < this->w / 3.f) ? 0 : (pos.x > this->w * 2.f / 3.f ? 2 : 1);
		const int zy = (pos.y < this->h / 3.f) ? 0 : (pos.y > this->h * 2.f / 3.f ? 2 : 1);
		POINT anchor{ zx == 0 ? 0 : (zx == 2 ? (LONG)std::lround(this->w) : (LONG)std::lround(this->w * 0.5f)),
					  zy == 0 ? 0 : (zy == 2 ? (LONG)std::lround(this->h) : (LONG)std::lround(this->h * 0.5f)) };
		applyScale(scale * factor, anchor);
	});
	onTimer.add([this](UINT id) { this->onTimerCB(id); });
	onKeyDown.add([this](UINT key) { this->onKey(key); });
	onDestroy.add([this]() { this->onClosed(); });
}

void WinPin::onClosed()
{
	if (isClosed) return;
	isClosed = true;
	CaptureEsc::release(hwnd);
	// 若本贴图处于专注模式，关闭时恢复被它隐藏的其它贴图
	if (focusMode) {
		for (auto& p : winPins) {
			if (p && p.get() != this && p->hiddenByFocus) { p->hiddenByFocus = false; p->show(); }
		}
	}
	if (toolOcr) { toolOcr->close(); toolOcr.reset(); }
	if (toolSub) toolSub->close();
	if (toolCap) toolCap->close();
	// 贴图上做过识别/翻译时，引擎（PP-OCRv6 约 100MB、离线翻译模型几百 MB）也在本进程常驻。
	// 与截图窗一致：这张贴图收工就把它们还回去，下次要用再重建（release 内部加锁，安全）。
	LwPpOcrEngine::release();
	BergamotEngine::release();
	shapeHover = nullptr;
	editingText = nullptr;
	Ling::App::get()->dq.TryEnqueue([this]() {
		std::erase_if(winPins, [this](const std::unique_ptr<WinPin>& p) { return p.get() == this; });
		if (winPins.empty()) {
			if (Ling::App::get()->args[L"--auto-quit"] == L"true") {
				Ling::App::get()->quit(0);
			}
			else {
				Update::checkLater();
			}
		}
	});
}

bool WinPin::hasWindow()
{
	return !winPins.empty();
}

void WinPin::dispose()
{
	// 同 WinSetting::dispose：~WinBase 不销毁 HWND，退出前把还开着的贴图窗逐个真正关掉
	for (auto& p : winPins) { if (p && p->hwnd) p->close(); }
	winPins.clear();
}

// 全局热键「隐藏/显示贴图」：一次把所有贴图藏起来，再按一次全部放回来。
// 纯临时操作 —— 不落盘、不改贴图自身状态；"放回来"时一并清除专注模式遗留的隐藏标记。
void WinPin::toggleAllHidden()
{
	if (winPins.empty()) return;
	pinsHidden = !pinsHidden;
	for (auto& p : winPins) {
		if (!p) continue;
		if (pinsHidden) {
			p->hide();
		}
		else {
			p->hiddenByFocus = false;
			p->show();
		}
	}
}

D2D1_SIZE_U WinPin::getImgSize() const
{
	if (!screenImg) return D2D1::SizeU(0, 0);
	return screenImg->GetPixelSize();
}

void WinPin::applyWinSize()
{
	auto sz = getImgSize();
	if (!hwnd || sz.width == 0 || sz.height == 0) return;
	auto newW = std::max(1, static_cast<int>(std::lround(sz.width * scale)));
	auto newH = std::max(1, static_cast<int>(std::lround(sz.height * scale)));
	w = static_cast<float>(newW);
	h = static_cast<float>(newH);
	SetWindowPos(hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void WinPin::applyScale(float newScale, POINT anchor)
{
	auto sz = getImgSize();
	if (sz.width == 0 || sz.height == 0) return;
	auto maxScale = std::max(1.f, std::min(10.f, 16000.f / std::max(sz.width, sz.height)));
	newScale = std::clamp(newScale, 0.1f, maxScale);
	if (std::abs(newScale - scale) < 0.0001f) return;
	if (editingText) editingText->finishEdit();
	auto imgX = anchor.x / scale;
	auto imgY = anchor.y / scale;
	scale = newScale;
	applyWinSize();
	setPosition(x + anchor.x - static_cast<int>(std::lround(imgX * scale)),
		y + anchor.y - static_cast<int>(std::lround(imgY * scale)));
	scaleTip = Ling::D2D::get()->makeTextLayout(std::format(L"{}%", static_cast<int>(std::lround(scale * 100.f))), 11.f * dpi);
	setTimer(800, 101);
	if (editing) layoutTools();
	refresh();
	// 识别结果是"持续显示"的：缩放期间只暂时收起（不销毁、不丢识别结果），
	// 停手 200ms 后再按新尺寸重建并显示回来（见 onTimerCB 的 102）
	if (toolOcr && toolOcr->hwnd) {
		ocrHiddenByZoom = true;
		toolOcr->hide();
		setTimer(200, 102);
	}
}

void WinPin::paintScaleTip(ID2D1DeviceContext* ctx)
{
	if (!scaleTip || !brushTipBg) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(scaleTip->GetMetrics(&tm))) return;
	auto pad = 3.f * dpi;
	auto margin = 5.f * dpi;
	D2D1_RECT_F bgRect{ w - margin - tm.width - pad * 2, margin, w - margin, margin + tm.height + pad * 2 };
	if (bgRect.left < margin) bgRect.left = margin;
	ctx->FillRectangle(bgRect, brushTipBg.Get());
	ctx->DrawTextLayout({ bgRect.left + pad, bgRect.top + pad }, scaleTip.Get(), brushTipText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void WinPin::layoutTools()
{
	if (!editing || !toolCap || !toolSub) return;
	RECT winRect{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
	MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
	auto monitor = MonitorFromRect(&winRect, MONITOR_DEFAULTTONEAREST);
	if (!monitor || !GetMonitorInfo(monitor, &mi)) {
		auto [sx, sy, sw, sh] = App::get()->getScreenArea();
		mi.rcWork = RECT{ sx, sy, sx + sw, sy + sh };
	}
	auto& wa = mi.rcWork;

	const auto gap = 5.f * dpi;
	const auto subGap = ToolSub::mainGap;
	const auto mainH = toolCap->h;
	const auto subH = toolSub->getDesiredHeight();
	const auto pad = ToolbarTheme::shadowPad * toolCap->dpi;
	const bool showSub = !getCurToolId().empty() && toolSub->hasContent();
	const auto stackH = showSub ? (mainH - pad + subGap + subH) : (mainH - pad);
	const auto visualGroupH = stackH - pad;

	float mainY;
	if (winRect.bottom + gap + visualGroupH <= wa.bottom) {
		mainY = winRect.bottom + gap - pad;
	}
	else if (winRect.top - gap - visualGroupH >= wa.top) {
		mainY = winRect.top - gap - stackH;
	}
	else {
		mainY = winRect.bottom - gap - stackH;
	}
	auto mainX = static_cast<float>(winRect.right) - toolCap->w + pad;
	if (!isMainToolUserPlaced()) {
		toolCap->setPosition(clampPos(mainX, toolCap->w, wa.left, wa.right), clampPos(mainY, stackH, wa.top, wa.bottom));
	}
	if (showSub) {
		toolSub->updatePosition(wa);
		raiseToolChrome();
	}
	else {
		toolSub->hideTools();
	}
}

void WinPin::bindToolChrome()
{
	if (!hwnd) return;
	if (toolCap) toolCap->bindOwner(hwnd);
	if (toolSub) toolSub->bindOwner(hwnd);
}

void WinPin::raiseToolChrome()
{
	if (!toolCap || !toolCap->hwnd) return;
	const bool subShown = toolSub && toolSub->hwnd && IsWindowVisible(toolSub->hwnd);
	if (subShown) {
		SetWindowPos(toolSub->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		SetWindowPos(toolCap->hwnd, toolSub->hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	else {
		SetWindowPos(toolCap->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void WinPin::captureEditBaseline()
{
	editBaselineUndo.clear();
	editBaselineSize = 0;
	hasEditBaseline = true;
	if (!history) return;
	editBaselineSize = history->shapes.size();
	editBaselineUndo.reserve(editBaselineSize);
	for (auto& s : history->shapes)
		editBaselineUndo.push_back(s && s->isUndo);
}

void WinPin::restoreEditBaseline()
{
	if (!hasEditBaseline || !history) return;
	shapeHover = nullptr;
	newShape = nullptr;
	if (history->shapes.size() > editBaselineSize)
		history->shapes.resize(editBaselineSize);
	const size_t n = std::min(editBaselineUndo.size(), history->shapes.size());
	for (size_t i = 0; i < n; ++i) {
		if (history->shapes[i])
			history->shapes[i]->isUndo = editBaselineUndo[i];
	}
	onHistoryChanged();
}

void WinPin::clearEditBaseline()
{
	hasEditBaseline = false;
	editBaselineSize = 0;
	editBaselineUndo.clear();
}

void WinPin::setEditing(bool on)
{
	if (on == editing) return;
	if (on && locked) return;
	if (on && thumbnail) exitThumbnail();
	// 编辑按 1:1 坐标落笔；裁剪放大是取景视图，进编辑前回到「直接放大」避免标注错位。
	if (on && zoomMode != ZoomMode::Direct) {
		zoomMode = ZoomMode::Direct;
		cropScale = 1.f;
		cropCenterX = -1.f; cropCenterY = -1.f;
	}
	editing = on;
	hoverBtn = HoverBtn::None;
	hoverChromeVisible = false;
	if (on) {
		captureEditBaseline();
		clearMainToolUserPlaced();
		if (toolCap) {
			if (!toolCap->hwnd) {
				toolCap->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
			}
			toolCap->applyPinLayout();
			bindToolChrome();
			if (!pendingTool.empty()) {
				toolCap->selectTool(pendingTool);
				pendingTool.clear();
			}
			layoutTools();
			toolCap->show();
			raiseToolChrome();
		}
	}
	else {
		// 退出编辑：收起工具栏
		if (toolCap) {
			toolCap->cancelSelect();
			if (toolCap->hwnd) toolCap->hide();
		}
		if (toolSub) toolSub->hideTools();
		clearAnnotSelection();
	}
	refresh();
}

void WinPin::finishEditing(bool apply)
{
	if (!editing) return;
	if (editingText) editingText->finishEdit();
	shapeHover = nullptr;
	newShape = nullptr;
	if (apply) {
		// 保留矢量标注，仅清掉已撤销项；再次编辑仍可改/删
		if (history) history->purgeUndone();
	}
	else {
		restoreEditBaseline();
	}
	clearEditBaseline();
	setEditing(false);
}

void WinPin::toggleBorderVisible()
{
	borderVisible = !borderVisible;
	refresh();
}

WinPin::~WinPin()
{
}

void WinPin::init(int x, int y, int w, int h, const std::wstring& initialTool, std::unique_ptr<ToolCap> cap)
{
	auto ptr = new WinPin(x, y, w, h, nullptr, initialTool, std::move(cap));
	std::unique_ptr<WinPin> winPin{ ptr };
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
	// 未编辑时工具栏尚未建窗或已 hide；编辑中再绑/抬栏
	if (ptr->editing) {
		ptr->bindToolChrome();
		ptr->raiseToolChrome();
	}
	winPins.push_back(std::move(winPin));
}

void WinPin::initFromData(int x, int y, int w, int h, std::vector<BYTE>& data)
{
	auto ptr = new WinPin(x, y, w, h, &data);
	std::unique_ptr<WinPin> winPin{ ptr };
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
	if (ptr->editing) {
		ptr->bindToolChrome();
		ptr->raiseToolChrome();
	}
	winPins.push_back(std::move(winPin));
}

Microsoft::WRL::ComPtr<IDWriteTextLayout> WinPin::makeHoverIcon(const wchar_t* code)
{
	ComPtr<IDWriteTextLayout> out;
	auto d2d = Ling::D2D::get();
	auto* format = d2d->getTextFormat(Icon::Family);
	const float slot = kBtn * dpi;
	d2d->dwriteFactory->CreateTextLayout(code, 1, format, slot, slot, out.GetAddressOf());
	if (out) {
		out->SetFontSize(18.f * dpi, { 0, 1 });
		out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}
	return out;
}

void WinPin::onCreated()
{
	disableBorderRadius();
	auto d2d = Ling::D2D::get();
	canvas = body->makeChild<Ling::Canvas>();
	canvas->enableSwapChain();
	canvas->setSizePercent(100.f, 100.f);
	// 贴图边框：颜色/粗细/圆角走设置，由「软件外观」页配置
	borderVisible = Setting::get()->getPinBorderDefaultEnabled();
	d2d->deviceContext->CreateSolidColorBrush(
		Ling::Color(Setting::get()->getPinBorderColor()).getD2DColor(), borderBrush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushTipBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushTipText.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.45f), hoverBtnBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), hoverIconBrush.GetAddressOf());
	iconLock = makeHoverIcon(Icon::Lock);
	iconUnlock = makeHoverIcon(Icon::Unlock);
	iconEdit = makeHoverIcon(Icon::Pen);
	iconClose = makeHoverIcon(Icon::Cancel);
	// 功能设置 · 贴图：自动缩放窗口 —— 图比工作区大时等比缩到工作区内（只缩不放）
	if (Setting::get()->getPinAutoFit()) {
		const auto imgSz = getImgSize();
		RECT rc{ x, y, x + (int)std::lround(w), y + (int)std::lround(h) };
		MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
		HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
		if (imgSz.width > 0 && imgSz.height > 0 && mon && GetMonitorInfo(mon, &mi)) {
			const float availW = (float)(mi.rcWork.right - mi.rcWork.left);
			const float availH = (float)(mi.rcWork.bottom - mi.rcWork.top);
			const float fit = (std::min)(availW / (float)imgSz.width, availH / (float)imgSz.height);
			if (fit < 1.f) {
				scale = (std::max)(0.1f, fit);
				applyWinSize();
				const int maxX = (std::max)((int)mi.rcWork.left, (int)mi.rcWork.right - (int)std::lround(w));
				const int maxY = (std::max)((int)mi.rcWork.top, (int)mi.rcWork.bottom - (int)std::lround(h));
				x = std::clamp(x, (int)mi.rcWork.left, maxX);
				y = std::clamp(y, (int)mi.rcWork.top, maxY);
				setPosition(x, y);
			}
		}
	}
	// 功能设置 · 贴图：自动文本识别 —— 钉好后直接出识别结果（不抢剪贴板）
	if (Setting::get()->getPinAutoOcr()) toggleOcrPanel(false);
	show();
	if (!WinCap::get()) CaptureEsc::claim(hwnd);
}

D2D1_RECT_F WinPin::lockBtnRect() const
{
	const float s = kBtn * dpi;
	const float inset = kBtnInset * dpi;
	return D2D1::RectF(inset, inset, inset + s, inset + s);
}

D2D1_RECT_F WinPin::closeBtnRect() const
{
	const float s = kBtn * dpi;
	const float inset = kBtnInset * dpi;
	return D2D1::RectF(w - inset - s, inset, w - inset, inset + s);
}

D2D1_RECT_F WinPin::editBtnRect() const
{
	const auto close = closeBtnRect();
	const float s = kBtn * dpi;
	const float gap = kBtnGap * dpi;
	return D2D1::RectF(close.left - gap - s, close.top, close.left - gap, close.bottom);
}

WinPin::HoverBtn WinPin::hitHoverBtn(POINT pos) const
{
	if (editing || thumbnail || !isMouseIn) return HoverBtn::None;
	if (ptInRect(lockBtnRect(), pos)) return HoverBtn::Lock;
	if (!locked) {
		if (ptInRect(editBtnRect(), pos)) return HoverBtn::Edit;
		if (ptInRect(closeBtnRect(), pos)) return HoverBtn::Close;
	}
	return HoverBtn::None;
}

void WinPin::paintHoverBtn(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, IDWriteTextLayout* icon, uint32_t iconColor, bool hovered)
{
	if (!hoverBtnBg || !hoverIconBrush) return;
	hoverBtnBg->SetColor(D2D1::ColorF(0, 0, 0, hovered ? 0.6f : 0.45f));
	const float cx = (r.left + r.right) * 0.5f;
	const float cy = (r.top + r.bottom) * 0.5f;
	const float rad = (r.right - r.left) * 0.5f;
	ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), hoverBtnBg.Get());
	if (icon) {
		hoverIconBrush->SetColor(Ling::Color(iconColor).getD2DColor());
		ctx->DrawTextLayout({ r.left, r.top }, icon, hoverIconBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	}
}

void WinPin::paintHoverChrome(ID2D1DeviceContext* ctx)
{
	if (editing || !hoverChromeVisible) return;
	paintHoverBtn(ctx, lockBtnRect(), locked ? iconLock.Get() : iconUnlock.Get(),
		locked ? Icon::ColorActive : 0xFFFFFFFFu, hoverBtn == HoverBtn::Lock);
	if (!locked) {
		paintHoverBtn(ctx, editBtnRect(), iconEdit.Get(), 0xFFFFFFFFu, hoverBtn == HoverBtn::Edit);
		paintHoverBtn(ctx, closeBtnRect(), iconClose.Get(), Icon::ColorCancel, hoverBtn == HoverBtn::Close);
	}
}

void WinPin::layout()
{
	Ling::WinBase::layout();
	if (!screenImg || !canvas) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	auto sz = screenImg->GetSize();
	if (sz.width == 0 || sz.height == 0) { canvas->finishPaint(); return; }

	const int bw = Setting::get()->getPinBorderWidth();
	const float sw = (bw > 0 ? bw : 1) * dpi;
	const float hw = sw * 0.5f;
	const float borderRad = (float)Setting::get()->getPinBorderRadius() * dpi;

	// 取景：Direct = 整图；Crop/缩略图 = 等比 cover 取一块。统一用
	//   client = f * (img - srcTopLeft)
	// 这一个变换同时映射底图与标注（标注本就存的是图像坐标）。
	float f = 1.f;
	const D2D1_RECT_F src = cropSourceRect(f);
	const D2D1::Matrix3x2F contentXform = D2D1::Matrix3x2F::Scale(f, f)
		* D2D1::Matrix3x2F::Translation(-f * src.left, -f * src.top);

	// 画面层不透明度（最外层）：淡「底图 + 标注 + 边框」三者；
	// 悬停按钮 / 缩放提示留在图层外（不变淡）。
	const float op = effectiveOpacity();
	const bool useLayer = op < 0.999f;
	if (useLayer) {
		if (!opacityLayer) ctx->CreateLayer(nullptr, opacityLayer.GetAddressOf());
		D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
		lp.contentBounds = D2D1::InfiniteRect();
		lp.opacity = op;
		ctx->PushLayer(lp, opacityLayer.Get());
	}

	// 圆角裁剪：边框开启且圆角 > 0 时，把画面裁进与边框外缘一致的圆角形状。
	// 裁剪层在「单位变换」下推入 —— 蒙版按窗口坐标（不受随后的取景/缩放变换影响），
	// 再把取景变换设回去、在层内绘制，于是画面四角随边框一起变圆而不是方角。
	const bool clipRound = borderVisible && borderRad > 0.f;
	if (clipRound) {
		ctx->SetTransform(D2D1::Matrix3x2F::Identity());
		if (!clipLayer) ctx->CreateLayer(nullptr, clipLayer.GetAddressOf());
		const float outerRad = borderRad + hw;   // 描边以中线为基准，外缘圆角 = 圆角 + 半线宽
		if (!clipGeo || clipGeoW != w || clipGeoH != h || clipGeoRad != outerRad) {
			clipGeo = nullptr;   // ID2D1RoundedRectangleGeometry 无法原地改，尺寸/圆角变了就重建
			Ling::D2D::get()->d2dFactory->CreateRoundedRectangleGeometry(
				D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), outerRad, outerRad), clipGeo.GetAddressOf());
			clipGeoW = w; clipGeoH = h; clipGeoRad = outerRad;
		}
		D2D1_LAYER_PARAMETERS clp = D2D1::LayerParameters();
		clp.contentBounds = D2D1::InfiniteRect();
		clp.geometricMask = clipGeo.Get();
		clp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
		ctx->PushLayer(clp, clipLayer.Get());
	}

	ctx->SetTransform(contentXform);
	D2D1_RECT_F destRect = D2D1::RectF(0, 0, sz.width, sz.height);
	ctx->DrawBitmap(screenImg.Get(), destRect);
	paintShapes(ctx);
	if (sizePreviewHold || GetTickCount64() < sizePreviewPulseUntil)
		paintSizePreview(ctx);
	if (clipRound) ctx->PopLayer();

	// 边框：不参与圆角裁剪（画在圆角形状的外缘），但在透明度层内 → 随画面一起变淡。
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	if (borderVisible) {
		ctx->DrawRoundedRectangle(
			D2D1::RoundedRect(D2D1::RectF(hw, hw, w - hw, h - hw), borderRad, borderRad), borderBrush.Get(), sw);
	}
	if (useLayer) ctx->PopLayer();
	if (!editing && !thumbnail) paintScaleTip(ctx);
	if (!thumbnail) paintHoverChrome(ctx);
	canvas->finishPaint();
}

D2D1_RECT_F WinPin::cropSourceRect(float& f) const
{
	auto sz = getImgSize();
	const float imgW = (float)sz.width, imgH = (float)sz.height;
	if (imgW <= 0 || imgH <= 0 || w <= 0 || h <= 0) { f = 1.f; return D2D1::RectF(0, 0, imgW, imgH); }
	if (zoomMode == ZoomMode::Direct && !thumbnail) {
		f = w / imgW;
		return D2D1::RectF(0, 0, imgW, imgH);
	}
	const float sW = w / imgW, sH = h / imgH;
	const float zoom = thumbnail ? 1.f : std::max(1.f, cropScale);
	const float cover = std::max(sW, sH) * zoom;
	const float srcW = std::max(1.f, std::floor(w / cover));
	const float srcH = std::max(1.f, std::floor(h / cover));
	const bool useCenter = !thumbnail && cropCenterX >= 0 && cropCenterY >= 0;
	const float cx = useCenter ? cropCenterX : imgW * 0.5f;
	const float cy = useCenter ? cropCenterY : imgH * 0.5f;
	const float sx = std::clamp(cx - srcW * 0.5f, 0.f, std::max(0.f, imgW - srcW));
	const float sy = std::clamp(cy - srcH * 0.5f, 0.f, std::max(0.f, imgH - srcH));
	f = w / srcW;
	return D2D1::RectF(sx, sy, sx + srcW, sy + srcH);
}

float WinPin::effectiveOpacity() const
{
	// 悬停显示原图（未编辑时按 100%），移开恢复设定值。
	// 拖动过程中不恢复原图（否则一拖就变实，手感和观感都很跳）。
	// 编辑态同样按设定值绘制 —— 否则编辑工具栏里的「透明度」滑条拖了没反应。
	if (!editing && isMouseIn && !annotMouseDown) return 1.f;
	return std::clamp(opacityPercent, 10, 100) / 100.f;
}

void WinPin::onMinMaxInfo(MINMAXINFO* mmi)
{
	auto [x, y, w, h] = App::get()->getScreenArea();
	mmi->ptMaxPosition.x = x;
	mmi->ptMaxPosition.y = y;
	mmi->ptMaxSize.x = w;
	mmi->ptMaxSize.y = h;
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
	mmi->ptMaxTrackSize.x = 20000;
	mmi->ptMaxTrackSize.y = 20000;
}

void WinPin::onDown(POINT pos, BOOL isRight)
{
	const auto hit = hitHoverBtn(pos);
	if (hit != HoverBtn::None) {
		if (hit == HoverBtn::Lock) {
			locked = !locked;
			refresh();
		}
		else if (hit == HoverBtn::Edit) {
			setEditing(true);
		}
		else if (hit == HoverBtn::Close) {
			close();
		}
		return;
	}
	if (locked) return;
	if (!editing) {
		// 非编辑：右键弹菜单（菜单已开时收起）
		if (isRight) {
			if (PinMenu::isOpen()) { PinMenu::closeMenu(); return; }
			POINT screenPt = pos;
			ClientToScreen(hwnd, &screenPt);
			showContextMenu(screenPt);
			return;
		}
		// 双击：按设置 → 缩略图 / 关闭
		POINT screenPos = pos;
		ClientToScreen(hwnd, &screenPos);
		const auto now = GetTickCount64();
		const bool dbl = (now - annotLastDownTime <= GetDoubleClickTime())
			&& std::abs(screenPos.x - annotLastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
			&& std::abs(screenPos.y - annotLastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
		annotLastDownTime = now;
		annotLastDownPos = screenPos;
		if (dbl) {
			if (Setting::get()->getPinDoubleClickAction() == 1) close();
			else if (thumbnail) exitThumbnail();
			else enterThumbnail(pos);
			return;
		}
		// 拖边缩放（缩略图态禁用）优先于点选拖窗
		const int edges = thumbnail ? 0 : hitResizeEdges(pos);
		if (edges) {
			edgeResize = edges;
			GetCursorPos(&edgeStartGlobal);
			edgeStartX = x; edgeStartY = y;
			edgeStartW = (int)std::lround(w); edgeStartH = (int)std::lround(h);
			edgeStartScale = scale;
			SetCapture(hwnd);
			return;
		}
		annotPressPos = pos;
		hasDragged = false;
		annotMouseDown = true;
		return;
	}
	annotDown(pos, isRight);
}

void WinPin::onMove(POINT pos)
{
	if (pos.x == INT_MAX) {
		if (hoverBtn != HoverBtn::None || hoverChromeVisible) {
			hoverBtn = HoverBtn::None;
			hoverChromeVisible = false;
			refresh();
		}
		else {
			refresh();   // 悬停原图 → 移开恢复设定不透明度
		}
		if (editing) annotMove(pos);
		return;
	}
	if (edgeResize) {
		POINT cur{};
		GetCursorPos(&cur);
		const int dx = cur.x - edgeStartGlobal.x;
		const int dy = cur.y - edgeStartGlobal.y;
		float newW = (float)edgeStartW, newH = (float)edgeStartH;
		if (edgeResize & 1) newW -= dx;   // left
		if (edgeResize & 4) newW += dx;   // right
		if (edgeResize & 2) newH -= dy;   // top
		if (edgeResize & 8) newH += dy;   // bottom
		const bool horiz = (edgeResize & (1 | 4)) != 0;
		const float ratio = horiz ? newW / std::max(1.f, (float)edgeStartW)
								  : newH / std::max(1.f, (float)edgeStartH);
		// 锚点 = 对边/对角（拖动边之外的那一侧固定）
		POINT anchor{ (edgeResize & 1) ? (LONG)std::lround(w) : 0,
					  (edgeResize & 2) ? (LONG)std::lround(h) : 0 };
		applyScale(edgeStartScale * ratio, anchor);
		return;
	}
	if (!editing) {
		const auto hit = hitHoverBtn(pos);
		const bool show = isMouseIn;
		if (hit != hoverBtn || show != hoverChromeVisible) {
			hoverBtn = hit;
			hoverChromeVisible = show;
			refresh();
		}
		if (annotMouseDown && !locked) {
			hasDragged = true;
			setPosition(x + pos.x - annotPressPos.x, y + pos.y - annotPressPos.y);
		}
		return;
	}
	annotMove(pos);
}

void WinPin::onUp(POINT pos, BOOL isRight)
{
	if (edgeResize) {
		edgeResize = 0;
		ReleaseCapture();
		return;
	}
	if (!editing) {
		annotMouseDown = false;
		hasDragged = false;
		return;
	}
	annotUp(pos, isRight);
}

bool WinPin::annotRightClick()
{
	if (locked) return true;
	if (!editing) {
		setEditing(true);
		return true;
	}
	if (!toolCap->hwnd || !IsWindowVisible(toolCap->hwnd)) {
		layoutTools();
		toolCap->show();
		return true;
	}
	toolCap->cancelSelect();
	toolCap->hide();
	toolSub->hideTools();
	return true;
}

bool WinPin::annotDoubleClick()
{
	// 双击在非编辑态由 onDown 处理（缩略图 / 关闭）；编辑态双击留给标注（文字重编辑等）。
	return true;
}

bool WinPin::annotEmptyToolDrag(POINT pos)
{
	if (locked) return true;
	if (toolCap) toolCap->hide();
	setPosition(x + pos.x - annotPressPos.x, y + pos.y - annotPressPos.y);
	return true;
}

void WinPin::annotAfterEmptyToolUp()
{
	if (!editing) return;
	layoutTools();
	if (toolCap) toolCap->show();
}

void WinPin::onTimerCB(UINT id)
{
	if (id == 101) {
		killTimer(101);
		scaleTip = nullptr;
		refresh();
		return;
	}
	if (id == 102) {
		// 缩放停手：按新尺寸把识别结果重新对齐一次再显示回来。
		// 顺序不能反：必须先 sync（改尺寸 + 重建叠加层），再 repaint 让它真的画出来，最后 show。
		killTimer(102);
		if (!toolOcr || !toolOcr->hwnd) { ocrHiddenByZoom = false; return; }
		ocrHiddenByZoom = false;
		if (thumbnail) return;   // 缩略图态不显示识别结果（菜单项也置灰）
		refreshOcrPanel();
		toolOcr->repaint();
		toolOcr->show();
		return;
	}
	annotTimer(id);
}

bool WinPin::sizePreviewOrigin(float& cx, float& cy, float& halfSpan) const
{
	auto imgSize = getImgSize();
	if (imgSize.width == 0 || imgSize.height == 0) return false;
	cx = imgSize.width * 0.5f;
	cy = imgSize.height * 0.5f;
	halfSpan = std::min(80.f, std::max(40.f, imgSize.width * 0.14f));
	return true;
}

void WinPin::forwardKey(UINT key)
{
	onKey(key);
}

void WinPin::onKey(UINT key)
{
	if (editingText) {
		annotKey(key);
		return;
	}
	if (editing) annotKey(key);
	bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	if (ctrl && key == 'C') {
		copyToClipboard();
	}
	else if (ctrl && key == 'S') {
		saveToFile();
	}
	else if (key == VK_RETURN) {
		if (editing) finishEditing(true);
		else { copyToClipboard(); close(); }
	}
	else if (key == VK_ESCAPE) {
		if (editing) finishEditing(false);
		else close();
	}
}

void WinPin::copyToClipboard()
{
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getAnnotPixels(pixels, size)) return;
	Util::saveToClipboard((int)size.width, (int)size.height, pixels.data());
	// 复制不再关闭贴图（用户确认）
}

void WinPin::saveToFile()
{
	auto foregroundBeforeDialog = GetForegroundWindow();
	// 「保存格式」决定扩展名；文件名默认给「手动保存文件名格式」的末段；起始目录固定「桌面」
	const std::wstring saveExt = Util::extensionFromSaveFormat(Setting::get()->getSaveFormat());
	std::wstring defName = Util::previewFileName(
		Setting::get()->getManualSaveFormat(), Setting::defaultManualSaveFormat(), L"");
	if (auto slash = defName.find_last_of(L'/'); slash != std::wstring::npos) defName = defName.substr(slash + 1);
	auto path = Util::getSaveFilePath(hwnd, saveExt, defName, Util::desktopDir().wstring());
	if (path.empty()) {
		restoreWindowState(foregroundBeforeDialog);
		return;
	}
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getAnnotPixels(pixels, size)) {
		restoreWindowState(foregroundBeforeDialog);
		return;
	}
	if (Util::saveToFile(path, (int)size.width, (int)size.height, pixels.data())) {
		close();
	}
	else {
		restoreWindowState(foregroundBeforeDialog);
	}
}

void WinPin::restoreWindowState(HWND foregroundBeforeDialog)
{
	if (hwnd) {
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	bindToolChrome();
	if (editing) raiseToolChrome();
	if (foregroundBeforeDialog && foregroundBeforeDialog != hwnd && IsWindowVisible(foregroundBeforeDialog)) {
		SetForegroundWindow(foregroundBeforeDialog);
	}
}

BOOL WinPin::setCursor()
{
	if (thumbnail) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return TRUE;
	}
	if (!editing) {
		if (hoverBtn != HoverBtn::None) SetCursor(LoadCursor(nullptr, IDC_HAND));
		else if (locked) SetCursor(LoadCursor(nullptr, IDC_ARROW));
		else {
			POINT pt{};
			GetCursorPos(&pt);
			ScreenToClient(hwnd, &pt);
			const int e = hitResizeEdges(pt);
			if (e) {
				const bool l = (e & 1) != 0, t = (e & 2) != 0, r = (e & 4) != 0, b = (e & 8) != 0;
				LPCTSTR id = ((l && t) || (r && b)) ? IDC_SIZENWSE
					: ((r && t) || (l && b)) ? IDC_SIZENESW
					: (l || r) ? IDC_SIZEWE : IDC_SIZENS;
				SetCursor(LoadCursor(nullptr, id));
			}
			else {
				SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
			}
		}
		return TRUE;
	}
	applyAnnotCursor();
	return TRUE;
}

void WinPin::applyEmptyToolCursor()
{
	// 没选工具时：图上还空着 → 按住能拖整张图（SizeAll）；**已经有标注 → 普通箭头**
	//（用户要的：有标注 + 没选工具 = 正常鼠标样式，十字只属于"选了工具"）
	bool any = false;
	if (history) {
		for (auto& s : history->shapes) {
			if (s && !s->isUndo) { any = true; break; }
		}
	}
	SetCursor(LoadCursor(nullptr, any ? IDC_ARROW : IDC_SIZEALL));
}

// ============================================================================
// 本次新增：右键菜单 / 缩略图 / 缩放 / 变换 / 外观 / 识别文字
// ============================================================================

int WinPin::hitResizeEdges(POINT pos) const
{
	if (thumbnail) return 0;
	const int m = (int)std::lround(7 * dpi);
	int e = 0;
	if (pos.x <= m) e |= 1;
	if (pos.y <= m) e |= 2;
	if (pos.x >= (int)std::lround(w) - m) e |= 4;
	if (pos.y >= (int)std::lround(h) - m) e |= 8;
	return e;
}

void WinPin::enterThumbnail(POINT cursorClientPos)
{
	if (thumbnail) return;
	if (editing) finishEditing(true);
	preThumbX = x; preThumbY = y;
	preThumbW = (int)std::lround(w); preThumbH = (int)std::lround(h);
	preThumbScale = scale;
	thumbnail = true;
	locked = false;
	hoverBtn = HoverBtn::None;
	hoverChromeVisible = false;

	const int thumb = std::max(16, (int)std::lround(50 * dpi));
	POINT g = cursorClientPos;
	ClientToScreen(hwnd, &g);
	auto [sx, sy, sw, sh] = App::get()->getScreenArea();
	int nx = g.x - thumb / 2, ny = g.y - thumb / 2;
	if (sw >= thumb) nx = std::clamp(nx, sx, sx + sw - thumb);
	if (sh >= thumb) ny = std::clamp(ny, sy, sy + sh - thumb);
	setSize((float)thumb / dpi, (float)thumb / dpi);
	setPosition(nx, ny);
	refresh();
	// 缩略图态不显示识别结果（50px 的窗口里叠加层没有意义，菜单里那一项也会置灰）。
	// 只隐藏、不销毁：退出缩略图后如果还勾着，原样恢复出来。
	killTimer(102);
	ocrHiddenByZoom = false;
	if (toolOcr && toolOcr->hwnd) toolOcr->hide();
}

void WinPin::exitThumbnail()
{
	if (!thumbnail) return;
	thumbnail = false;
	setSize((float)preThumbW / dpi, (float)preThumbH / dpi);
	setPosition(preThumbX, preThumbY);
	scale = preThumbScale;
	refresh();
	// 退出缩略图：还勾着"显示识别结果"就原样恢复（缩略图期间只是藏起来了）
	if (toolOcr && toolOcr->hwnd) {
		refreshOcrPanel();
		toolOcr->repaint();
		toolOcr->show();
	}
}

void WinPin::applyTopMost(bool on)
{
	topMost = on;
	SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	refresh();
}

void WinPin::setFocusMode(bool on)
{
	focusMode = on;
	for (auto& p : winPins) {
		if (!p || p.get() == this) continue;
		if (on) {
			if (IsWindowVisible(p->hwnd)) { p->hiddenByFocus = true; p->hide(); }
		}
		else if (p->hiddenByFocus) {
			p->hiddenByFocus = false; p->show();
		}
	}
	refresh();
}

void WinPin::setZoomMode(ZoomMode m)
{
	if (zoomMode == m) return;
	zoomMode = m;
	cropScale = 1.f;
	cropCenterX = -1.f; cropCenterY = -1.f;
	refresh();
}

void WinPin::setOpacityPercent(int percent)
{
	percent = std::clamp(percent, 10, 100);
	if (opacityPercent == percent) return;
	opacityPercent = percent;
	Setting::get()->setPinOpacity(percent);
	refresh();
}

void WinPin::rotateImage90(bool clockwise) { applyImageTransform(clockwise ? 1 : -1, false); }
void WinPin::mirrorImage(bool horizontal) { applyImageTransform(0, horizontal); }

void WinPin::applyImageTransform(int quarterTurns, bool flipH)
{
	if (quarterTurns == 0 && !flipH) return;
	if (editing) finishEditing(true);
	std::vector<BYTE> px;
	D2D1_SIZE_U sz{};
	if (!getAnnotPixels(px, sz)) return;
	int W = (int)sz.width, H = (int)sz.height;
	if (W <= 0 || H <= 0) return;

	if (quarterTurns != 0) {
		const bool cw = quarterTurns > 0;
		const int nW = H, nH = W;
		std::vector<BYTE> out((size_t)nW * nH * 4);
		for (int y = 0; y < H; ++y) {
			for (int x = 0; x < W; ++x) {
				const int dx = cw ? (H - 1 - y) : y;
				const int dy = cw ? x : (W - 1 - x);
				memcpy(&out[((size_t)dy * nW + dx) * 4], &px[((size_t)y * W + x) * 4], 4);
			}
		}
		px.swap(out);
		W = nW; H = nH;
	}
	if (flipH) {
		for (int y = 0; y < H; ++y) {
			BYTE* row = &px[(size_t)y * W * 4];
			for (int x = 0; x < W / 2; ++x)
				for (int c = 0; c < 4; ++c) std::swap(row[x * 4 + c], row[(W - 1 - x) * 4 + c]);
		}
	}

	D2D1_BITMAP_PROPERTIES1 props{};
	props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
	props.dpiX = 96.0f; props.dpiY = 96.0f;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
	Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(W, H), px.data(), W * 4, &props, bmp.GetAddressOf());
	if (!bmp) return;
	screenImg = bmp;
	clearAllShapes();   // 标注已烘焙进底图，清掉矢量层

	// 保持中心，按原比例重排窗口
	const int cx = x + (int)std::lround(w * 0.5f), cy = y + (int)std::lround(h * 0.5f);
	const int nWinW = std::max(1, (int)std::lround(W * scale));
	const int nWinH = std::max(1, (int)std::lround(H * scale));
	x = cx - nWinW / 2; y = cy - nWinH / 2;
	w = (float)nWinW; h = (float)nWinH;
	SetWindowPos(hwnd, nullptr, x, y, nWinW, nWinH, SWP_NOZORDER | SWP_NOACTIVATE);
	cropCenterX = -1.f; cropCenterY = -1.f;
	refresh();
	refreshOcrPanel();
}

void WinPin::refreshOcrPanel()
{
	// 缩放中 / 缩略图态：先不动叠加层。缩放是逐帧的，每帧重建一遍叠加层既慢，
	// 重建又会撞上"窗口改尺寸（SWP_NOREDRAW）还没重绘"的空档，看起来就是"一缩放结果就没了"。
	// 这两种情况下统一等停手（定时器 102）或退出缩略图后再重建一次。
	if (ocrHiddenByZoom || thumbnail) return;
	if (toolOcr && toolOcr->hwnd) {
		const D2D1_RECT_F r = D2D1::RectF(0, 0, w, h);
		toolOcr->syncToMask(r, x, y, dpi);
	}
}

void WinPin::toggleOcrPanel(bool copyText)
{
	killTimer(102);
	ocrHiddenByZoom = false;
	if (toolOcr) { toolOcr.reset(); refresh(); return; }
	std::vector<BYTE> px;
	D2D1_SIZE_U sz{};
	if (!getAnnotPixels(px, sz)) return;
	auto result = OcrService::instance().recognize((int)sz.width, (int)sz.height, px.data());
	toolOcr = std::make_unique<ToolOcr>(this, result, false, std::move(px), (int)sz.width, (int)sz.height);
	// 贴图叠加：不显示右上角复制按钮；只有识别文本框吃鼠标（可选中/复制），其余穿透。
	// 必须在建窗之前设置。
	toolOcr->setPinOverlay(true);
	toolOcr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	const D2D1_RECT_F r = D2D1::RectF(0, 0, w, h);
	toolOcr->syncToMask(r, x, y, dpi);
	// 自动识别（钉图即识别）不抢剪贴板，避免把用户剪贴板里的东西冲掉
	if (copyText && result.ok && !result.text.empty()) Ling::Util::setTextToClipboard(result.text);
	// 贴图识别的是同一张图，结果出来之后就不会再需要引擎了 —— 立刻还回去（常驻约 100MB），
	// 换一张贴图或重新识别时再重建，别让它陪着贴图一直挂着。
	LwPpOcrEngine::release();
	refresh();
}

void WinPin::showContextMenu(POINT screenPt)
{
	std::vector<PinMenu::Item> items;
	auto add = [&](const std::wstring& t, std::function<void()> a, bool checkable = false, bool checked = false) {
		PinMenu::Item it;
		it.text = t;
		it.action = std::move(a);
		it.checkable = checkable;
		it.checked = checked;
		items.push_back(std::move(it));
	};
	// 生成一个子菜单项（文本 + 回调 + 可选勾选态）
	auto sub = [](const std::wstring& t, std::function<void()> a, bool checkable = false, bool checked = false) {
		PinMenu::Item it;
		it.text = t;
		it.action = std::move(a);
		it.checkable = checkable;
		it.checked = checked;
		return it;
	};
	// 生成一个带右侧箭头的子菜单父项
	auto addSub = [&](const std::wstring& t, std::vector<PinMenu::Item> children) {
		PinMenu::Item it;
		it.text = t;
		it.submenu = std::move(children);
		items.push_back(std::move(it));
	};
	auto sep = [&]() { PinMenu::Item it; it.separator = true; items.push_back(it); };

	add(Lang::get(L"pin.menuCopy"), [this]() { copyToClipboard(); });
	add(Lang::get(L"pin.menuCopyFile"), [this]() {
		std::vector<BYTE> pixels;
		D2D1_SIZE_U size{};
		if (!getAnnotPixels(pixels, size)) return;
		const std::wstring path = Util::buildSavePath(
			Setting::get()->getManualSaveFormat(), Setting::defaultManualSaveFormat(),
			Util::extensionFromSaveFormat(Setting::get()->getSaveFormat()));
		if (path.empty() || !Util::saveToFile(path, (int)size.width, (int)size.height, pixels.data())) return;
		Util::addFileToClipboard(path);
	});
	add(Lang::get(L"pin.menuSaveAs"), [this]() { saveToFile(); });
	add(Lang::get(L"pin.menuOcr"), [this]() { toggleOcrPanel(); }, true, toolOcr != nullptr);
	// 缩略图态不提供识别结果（窗口只有 50px，叠加层没有意义）：整项置灰、不可点
	items.back().enabled = !thumbnail;
	sep();
	add(Lang::get(L"pin.menuEdit"), [this]() { if (thumbnail) exitThumbnail(); setEditing(true); });
	addSub(Lang::get(L"pin.menuTransform"), {
		sub(Lang::get(L"pin.menuRotateL"), [this]() { rotateImage90(false); }),
		sub(Lang::get(L"pin.menuRotateR"), [this]() { rotateImage90(true); }),
		sub(Lang::get(L"pin.menuFlipH"), [this]() { mirrorImage(true); }),
		sub(Lang::get(L"pin.menuFlipV"), [this]() { mirrorImage(false); }),
	});
	sep();
	add(Lang::get(L"pin.menuThumb"), [this]() {
		if (thumbnail) { exitThumbnail(); }
		else { POINT c{ (LONG)std::lround(w * 0.5f), (LONG)std::lround(h * 0.5f) }; enterThumbnail(c); }
	}, true, thumbnail);
	add(Lang::get(L"pin.menuFocus"), [this]() { setFocusMode(!focusMode); }, true, focusMode);
	add(Lang::get(L"pin.menuTop"), [this]() { applyTopMost(!topMost); }, true, topMost);
	add(Lang::get(L"pin.showBorder"), [this]() { toggleBorderVisible(); }, true, borderVisible);
	sep();
	{
		std::vector<PinMenu::Item> opItems;
		for (int v : { 25, 50, 75, 100 })
			opItems.push_back(sub(std::to_wstring(v) + L"%",
				[this, v]() { setOpacityPercent(v); }, true, opacityPercent == v));
		addSub(Lang::get(L"pin.menuOpacity"), std::move(opItems));
	}
	addSub(Lang::get(L"pin.menuZoomMode"), {
		sub(Lang::get(L"pin.zoomDirect"), [this]() { setZoomMode(ZoomMode::Direct); }, true, zoomMode == ZoomMode::Direct),
		sub(Lang::get(L"pin.zoomCrop"), [this]() { setZoomMode(ZoomMode::Crop); }, true, zoomMode == ZoomMode::Crop),
	});
	sep();
	add(Lang::get(L"pin.menuClose"), [this]() { close(); });

	PinMenu::popup(std::move(items), screenPt);
}
