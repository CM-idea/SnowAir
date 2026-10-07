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
#include "../App.h"
#include "../Setting.h"
#include "../Util.h"
#include "../Update.h"
#include "CaptureEsc.h"

using namespace Microsoft::WRL;
namespace {
	std::vector<std::unique_ptr<WinPin>> winPins;

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
	onMoved.add([this]() { if (editing) layoutTools(); });
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
		if (locked) return;
		if (GetKeyState(VK_CONTROL) & 0x8000) {
			applyScale(scale * (space > 0 ? 1.1f : 1.f / 1.1f), pos);
			return;
		}
		if (!editing || !shapeHover) return;
		auto imgPos = toImgPos(pos);
		shapeHover->mouseWheel((float)imgPos.x, (float)imgPos.y, space > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
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
	if (toolSub) toolSub->close();
	if (toolCap) toolCap->close();
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
	auto maxScale = std::max(1.f, std::min(8.f, 16000.f / std::max(sz.width, sz.height)));
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
	if (editing || !isMouseIn) return HoverBtn::None;
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
	D2D1_RECT_F destRect = D2D1::RectF(0, 0, sz.width, sz.height);
	ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
	ctx->DrawBitmap(screenImg.Get(), destRect);
	paintShapes(ctx);
	if (sizePreviewHold || GetTickCount64() < sizePreviewPulseUntil)
		paintSizePreview(ctx);
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	if (borderVisible) {
		const int bw = Setting::get()->getPinBorderWidth();
		const float sw = (bw > 0 ? bw : 1) * dpi;
		const float hw = sw * 0.5f;
		const float rad = (float)Setting::get()->getPinBorderRadius() * dpi;
		ctx->DrawRoundedRectangle(
			D2D1::RoundedRect(D2D1::RectF(hw, hw, w - hw, h - hw), rad, rad), borderBrush.Get(), sw);
	}
	if (!editing) paintScaleTip(ctx);
	paintHoverChrome(ctx);
	canvas->finishPaint();
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
		// 非编辑：左键拖窗
		if (!isRight) {
			annotPressPos = pos;
			hasDragged = false;
			annotMouseDown = true;
		}
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
		if (editing) annotMove(pos);
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
	if (locked || editing) return true;
	copyToClipboard();
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
		else copyToClipboard();
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
	close();
}

void WinPin::saveToFile()
{
	auto foregroundBeforeDialog = GetForegroundWindow();
	auto path = Util::getSaveFilePath(hwnd);
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
	if (!editing) {
		if (hoverBtn != HoverBtn::None) SetCursor(LoadCursor(nullptr, IDC_HAND));
		else if (locked) SetCursor(LoadCursor(nullptr, IDC_ARROW));
		else SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
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
