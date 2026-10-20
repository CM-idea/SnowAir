#include "pch.h"
#include "ToolOcrCompare.h"
#include "../App.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"

ToolOcrCompare::ToolOcrCompare(const std::vector<BYTE>& pixels, int imgW, int imgH, float dpi)
	: imgW_(std::max(1, imgW)), imgH_(std::max(1, imgH))
{
	this->dpi = dpi;
	ToolbarTheme::refresh();
	// 像素只在建位图时用一次：之后浮窗不再持有原图内存
	App::createBitmapFromBGRA(imgW_, imgH_, pixels, bitmap_.GetAddressOf());

	onMouseDown.add([this](POINT, bool isRight) {
		if (isRight || !hwnd) return;
		dragging_ = true;
		GetCursorPos(&dragCursor_);
		dragStartX_ = x;
		dragStartY_ = y;
		SetCapture(hwnd);
	});
	onMouseMove.add([this](POINT) {
		if (!dragging_) return;
		POINT p{};
		GetCursorPos(&p);
		setPosition(dragStartX_ + (p.x - dragCursor_.x), dragStartY_ + (p.y - dragCursor_.y));
	});
	onMouseUp.add([this](POINT, bool) {
		if (!dragging_) return;
		dragging_ = false;
		if (GetCapture() == hwnd) ReleaseCapture();
	});
}

ToolOcrCompare::~ToolOcrCompare()
{
	// 与 ToolOcr 同理：不先 close 就析构，窗口 WndProc 会指向已释放的对象
	if (hwnd) { close(); hwnd = nullptr; }
}

BOOL ToolOcrCompare::setCursor()
{
	SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
	return TRUE;
}

void ToolOcrCompare::onCreated()
{
	body->setBg(0);
	body->setBorderRadius(0.f);
	canvas_ = body->makeChild<Ling::Canvas>();
	canvas_->enableSwapChain();
	canvas_->setSizePercent(100.f, 100.f);
}

void ToolOcrCompare::layout()
{
	Ling::WinBase::layout();
	if (!canvas_ || !bitmap_) return;
	auto* ctx = canvas_->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	// 原图直接铺满（无边框/底色）
	ctx->DrawBitmap(bitmap_.Get(), D2D1::RectF(0.f, 0.f, w, h), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
	canvas_->finishPaint();
}

void ToolOcrCompare::placeNear(const RECT& panel)
{
	const RECT wa = ToolbarChrome::workAreaNear(panel);
	const int waL = (int)wa.left, waT = (int)wa.top, waR = (int)wa.right, waB = (int)wa.bottom;
	const int gap = (int)std::lround(8.f * dpi);
	// 原图优先 1:1；超过当前屏可用空间才等比缩小，保证整个浮窗还能摆在屏内拖动
	const int maxW = (std::max)(1, (waR - waL) - gap * 2);
	const int maxH = (std::max)(1, (waB - waT) - gap * 2);
	float scale = 1.f;
	if (imgW_ > maxW || imgH_ > maxH)
		scale = (std::min)((float)maxW / imgW_, (float)maxH / imgH_);
	const int winW = (std::max)(1, (int)std::lround(imgW_ * scale));
	const int winH = (std::max)(1, (int)std::lround(imgH_ * scale));
	setSize((float)winW / dpi, (float)winH / dpi);

	// 优先面板左侧 → 右侧 → 上方（都没空间时夹进工作区，允许压住面板）
	const int panelL = (int)panel.left, panelT = (int)panel.top;
	const int panelW = (int)(panel.right - panel.left);
	const int panelH = (int)(panel.bottom - panel.top);
	int px = panelL - gap - winW;
	int py = panelT + (panelH - winH) / 2;
	if (px < waL) {
		px = (int)panel.right + gap;
		if (px + winW > waR) {
			px = panelL + (panelW - winW) / 2;
			py = panelT - gap - winH;
		}
	}
	px = std::clamp(px, waL, (std::max)(waL, waR - winW));
	py = std::clamp(py, waT, (std::max)(waT, waB - winH));
	setPosition(px, py);
}
