#include "pch.h"
#include <include/Ling.h>
#include "CapLong.h"
#include "WinCap.h"
#include "CutMask.h"
#include "WinPin.h"
#include "../Tool/ToolLong.h"
#include "../Tool/ToolbarTheme.h"
#include "../App.h"
#include "../Util.h"
#include "../Lang.h"
#include "../Setting.h"
using namespace Microsoft::WRL;

namespace {
	constexpr UINT scrollMsgId = 18;
	constexpr UINT scrollEndMsgId = 19;
	constexpr UINT previewDragPanMsgId = 20; // 拖动时边缘缓滚缩略图
	constexpr int comparisonH = 100;
	constexpr int maxDismissTime = 8;
	constexpr int scrollSettleMs = 100;
	constexpr int settleRecheckMs = 100;
	constexpr int maxSettleRecheck = 2;
	constexpr int scrollIntervalMs = 140;
	constexpr int previewDragPanMs = 16;
	constexpr double bottomMatchMinRatio = 0.9;
	constexpr double bottomMatchMaxError = 40000;

	std::vector<BYTE> toGrayscale(const BYTE* bgra, int width, int height, int stride)
	{
		std::vector<BYTE> gray(width * height);
		for (int y = 0; y < height; y++) {
			const BYTE* src = bgra + y * stride;
			BYTE* dst = gray.data() + y * width;
			for (int x = 0; x < width; x++) {
				dst[x] = (BYTE)((src[x * 4] * 114 + src[x * 4 + 1] * 587 + src[x * 4 + 2] * 299) / 1000);
			}
		}
		return gray;
	}

	int findMostSimilarY(const BYTE* gray1, int gray1H, const BYTE* gray2, int gray2H, int width)
	{
		int searchH = gray1H - gray2H + 1;
		if (searchH <= 0) return 0;
		double minAvgError = DBL_MAX;
		int bestY = 0;
		for (int y = 0; y < searchH; y++) {
			double error = 0.0;
			for (int row = 0; row < gray2H; row++) {
				const BYTE* row1 = gray1 + (y + row) * width;
				const BYTE* row2 = gray2 + row * width;
				for (int x = 0; x < width; x++) {
					int diff = (int)row1[x] - (int)row2[x];
					error += diff * diff;
				}
			}
			double avgError = error / gray2H;
			if (avgError < minAvgError) {
				minAvgError = avgError;
				bestY = y;
			}
		}
		return bestY;
	}

	bool framesDiffer(const std::vector<BYTE>& a, const std::vector<BYTE>& b)
	{
		if (a.size() != b.size()) return true;
		return memcmp(a.data(), b.data(), a.size()) != 0;
	}

	int findScrollByBottomStrip(const BYTE* grayOld, const BYTE* grayNew, int width, int stripH)
	{
		double minAvgError = DBL_MAX;
		double avgAtZero = DBL_MAX;
		int bestS = 0;
		for (int s = 0; s < stripH; s++) {
			int rows = stripH - s;
			double error = 0.0;
			for (int r = 0; r < rows; r++) {
				const BYTE* row1 = grayOld + (size_t)(s + r) * width;
				const BYTE* row2 = grayNew + (size_t)r * width;
				for (int x = 0; x < width; x++) {
					int diff = (int)row1[x] - (int)row2[x];
					error += diff * diff;
				}
			}
			double avgError = error / rows;
			if (s == 0) avgAtZero = avgError;
			if (avgError < minAvgError) {
				minAvgError = avgError;
				bestS = s;
			}
		}
		if (bestS <= 0) return 0;
		if (minAvgError >= avgAtZero * bottomMatchMinRatio) return 0;
		if (minAvgError > bottomMatchMaxError) return 0;
		return bestS;
	}
}

CapLong::CapLong(WinCap* win) : win(win)
{
	startCircleR *= win->dpi;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), textBrush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.68f), bgBrush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x14 / 255.f, 0x16 / 255.f, 0x1c / 255.f, 1.f), viewportBgBrush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x34 / 255.f, 0xC7 / 255.f, 0x59 / 255.f, 0.95f), previewFrameBrush.GetAddressOf());
	auto size{ startCircleR * 2 };
	layoutTextStart = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.start"), 16 * win->dpi, size, size);
	if (layoutTextStart) {
		DWRITE_TEXT_METRICS tm{};
		layoutTextStart->GetMetrics(&tm);
		startTextSize = { tm.width, tm.height };
	}
}

CapLong::~CapLong()
{
}

void CapLong::dispose()
{
	win->killTimer(scrollMsgId);
	win->killTimer(scrollEndMsgId);
	win->killTimer(previewDragPanMsgId);
	ensureExcludeFromCapture(false);
	if (tool) tool->close();
}

void CapLong::ensureExcludeFromCapture(bool on)
{
	if (on == excludedFromCapture) return;
	excludedFromCapture = on;
	if (on) App::excludeFromCapture(win->hwnd);
	else App::clearCaptureExclusion(win->hwnd);
}

bool CapLong::showSelViewport() const
{
	if (imgData.empty()) return false;
	return isFinish || isPaused || previewHot || previewPanning
		|| selViewportPanning || isEditMode();
}

float CapLong::previewTopRatio() const
{
	const float maxPan = std::max(0.f, previewFullH - previewViewH);
	return maxPan > 0.f ? previewPanY / maxPan : 0.f;
}

float CapLong::viewRatio() const
{
	return std::clamp(inspectRatio, 0.f, 1.f);
}

void CapLong::paint(ID2D1DeviceContext* ctx)
{
	paintImgPreview(ctx);
	paintSelViewport(ctx);
	if (isFinish) {
		auto borderRadius{ 4.f * win->dpi };
		ctx->FillRoundedRectangle(D2D1::RoundedRect(stopTextRect, borderRadius, borderRadius), bgBrush.Get());
		ctx->DrawTextLayout(stopTextPos, layoutTextEnd.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	}
	else if (!isCapturing) {
		paintStartBtn(ctx);
	}
}

void CapLong::updatePreviewLayout()
{
	previewViewRect = {};
	previewFullH = 0.f;
	previewViewH = 0.f;
	if (!imgPreview || !tool) return;
	const auto sz = imgPreview->GetPixelSize();
	previewFullH = (float)sz.height;
	const float drawW = (float)sz.width;
	// 与工具栏到选区的间距一致：strokeWidth + 2×dpi
	const float gap = (win->cutMask ? win->cutMask->strokeWidth : 0.f) + 2.f * win->dpi;
	const float topMargin = 12.f * win->dpi; // 顶部留白，不贴死屏幕上沿

	HMONITOR mon = MonitorFromWindow(tool->hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(mi) };
	GetMonitorInfo(mon, &mi);
	const float maxH = std::max(80.f, (float)tool->y - gap - (float)mi.rcWork.top - topMargin);
	previewViewH = std::min(previewFullH, maxH);
	const float maxPan = std::max(0.f, previewFullH - previewViewH);

	// 悬停/拖预览时冻结贴底，否则内容被每帧拽到底部（看不清）
	const bool userInspecting = previewHot || previewPanning;
	if (userInspecting) {
		previewPanY = std::clamp(previewPanY, 0.f, maxPan);
		inspectRatio = std::clamp(inspectRatio, 0.f, 1.f);
	}
	else if (previewFollowBottom || (isCapturing && !isPaused && !isFinish)) {
		previewPanY = maxPan;
		previewFollowBottom = true;
		inspectRatio = 1.f;
	}
	else {
		previewPanY = std::clamp(previewPanY, 0.f, maxPan);
		inspectRatio = std::clamp(inspectRatio, 0.f, 1.f);
	}

	POINT pos{ tool->x, tool->y - (int)previewViewH - (int)gap };
	// 工具栏在选区底部时，预览改贴选区右侧（放不下则左侧），避免盖住选区
	if (win->cutMask) {
		auto& mask = win->cutMask->maskRect;
		POINT maskR{ (LONG)mask.right, (LONG)mask.top };
		POINT maskL{ (LONG)mask.left, (LONG)mask.top };
		ClientToScreen(win->hwnd, &maskR);
		ClientToScreen(win->hwnd, &maskL);
		HMONITOR monPrev = MonitorFromWindow(tool->hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO miPrev{ sizeof(miPrev) };
		GetMonitorInfo(monPrev, &miPrev);
		POINT screenPos{ 0, tool->y - (int)previewViewH - (int)gap };
		if (maskR.x + (int)gap + (int)drawW <= miPrev.rcWork.right)
			screenPos.x = maskR.x + (int)gap;
		else
			screenPos.x = maskL.x - (int)drawW - (int)gap;
		if (screenPos.x < miPrev.rcWork.left) screenPos.x = miPrev.rcWork.left;
		if (screenPos.x + (int)drawW > miPrev.rcWork.right)
			screenPos.x = miPrev.rcWork.right - (int)drawW;
		if (screenPos.y < miPrev.rcWork.top) screenPos.y = miPrev.rcWork.top;
		pos = screenPos;
	}
	ScreenToClient(win->hwnd, &pos);
	previewViewRect = D2D1::RectF((float)pos.x, (float)pos.y, pos.x + drawW, pos.y + previewViewH);
}

void CapLong::paintImgPreview(ID2D1DeviceContext* ctx)
{
	if (!imgPreview || !tool) return;
	updatePreviewLayout();
	if (previewViewH <= 0.f) return;
	const auto sz = imgPreview->GetPixelSize();
	const float rr = std::min(10.f * win->dpi,
		std::min((previewViewRect.right - previewViewRect.left), (previewViewRect.bottom - previewViewRect.top)) * 0.5f);
	Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRoundedRectangleGeometry(
		D2D1::RoundedRect(previewViewRect, rr, rr), clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	D2D1_RECT_F dest{
		previewViewRect.left,
		previewViewRect.top - previewPanY,
		previewViewRect.left + (float)sz.width,
		previewViewRect.top - previewPanY + (float)sz.height
	};
	ctx->DrawBitmap(imgPreview.Get(), dest);
	ctx->PopLayer();
	paintPreviewViewFrame(ctx);
}

void CapLong::paintPreviewViewFrame(ID2D1DeviceContext* ctx)
{
	// 悬停/拖/暂停/结束：线框标出选区当前预览对应的长图片段
	if (!previewFrameBrush) return;
	if (!(previewHot || previewPanning || isPaused || isFinish)) return;
	if (previewViewH <= 0.f || resultH <= imgH || !win->cutMask) return;

	const float frameH = std::clamp(
		previewFullH * ((float)imgH / (float)resultH),
		4.f * win->dpi,
		previewFullH);
	const float maxTop = std::max(0.f, previewFullH - frameH);
	const float frameTop = viewRatio() * maxTop;
	const float y0 = previewViewRect.top + frameTop - previewPanY;
	const float y1 = y0 + frameH;
	const float left = previewViewRect.left;
	const float right = previewViewRect.right;
	const float top = previewViewRect.top;
	const float bottom = previewViewRect.bottom;
	const float cy0 = std::clamp(y0, top, bottom);
	const float cy1 = std::clamp(y1, top, bottom);
	if (cy1 - cy0 < 1.f) return;

	const float rr = std::min(10.f * win->dpi,
		std::min(right - left, bottom - top) * 0.5f);
	Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRoundedRectangleGeometry(
		D2D1::RoundedRect(previewViewRect, rr, rr), clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	const float stroke = std::max(1.5f, 1.5f * win->dpi);
	const float boxW = right - left - stroke;
	const float boxH = cy1 - cy0 - stroke;
	const float boxR = std::min(6.f * win->dpi, std::min(boxW, boxH) * 0.5f);
	ctx->DrawRoundedRectangle(
		D2D1::RoundedRect(
			D2D1::RectF(left + stroke * 0.5f, cy0 + stroke * 0.5f, right - stroke * 0.5f, cy1 - stroke * 0.5f),
			boxR, boxR),
		previewFrameBrush.Get(), stroke);
	ctx->PopLayer();
}

void CapLong::paintSelViewport(ID2D1DeviceContext* ctx)
{
	if (!showSelViewport() || !fullPreviewBmp) return;
	auto& sel = win->cutMask->maskRect;
	ctx->FillRectangle(sel, viewportBgBrush.Get());

	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH)) return;
	const auto bmpSize = fullPreviewBmp->GetPixelSize();

	Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRectangleGeometry(sel, clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	const float drawH = srcH * scale;
	D2D1_RECT_F dest{ sel.left, sel.top, sel.right, sel.top + std::min(drawH, sel.bottom - sel.top) };
	D2D1_RECT_F src{ 0.f, (float)srcY, (float)bmpSize.width, (float)(srcY + srcH) };
	ctx->DrawBitmap(fullPreviewBmp.Get(), dest, 1.f, D2D1_INTERPOLATION_MODE_LINEAR, src);
	ctx->PopLayer();
}

bool CapLong::getSelViewMapping(float& scale, int& srcY, int& srcH) const
{
	if (!win->cutMask || resultH <= 0 || imgW <= 0) return false;
	const auto& sel = win->cutMask->maskRect;
	scale = (sel.right - sel.left) / (float)imgW;
	if (scale <= 0.f) return false;
	// 用 floor，避免 lround 多算 1 行导致暂停时画面往上偏
	srcH = std::max(1, (int)std::floor((sel.bottom - sel.top) / scale + 1e-3f));
	srcH = std::min(srcH, resultH);
	if (imgH > 0 && std::abs(srcH - imgH) <= 1)
		srcH = std::min(imgH, resultH);
	const int maxSrcY = std::max(0, resultH - srcH);
	srcY = std::clamp((int)std::lround(viewRatio() * maxSrcY), 0, maxSrcY);
	return true;
}

POINT CapLong::clientToLongImg(POINT client) const
{
	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH) || !win->cutMask) return client;
	const auto& sel = win->cutMask->maskRect;
	POINT out;
	out.x = (LONG)std::lround((client.x - sel.left) / scale);
	out.y = (LONG)std::lround((client.y - sel.top) / scale) + srcY;
	return out;
}

D2D1_POINT_2F CapLong::longImgToClient(float ix, float iy) const
{
	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH) || !win->cutMask)
		return D2D1::Point2F(ix, iy);
	const auto& sel = win->cutMask->maskRect;
	return D2D1::Point2F(sel.left + ix * scale, sel.top + (iy - (float)srcY) * scale);
}

bool CapLong::beginLongAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform)
{
	if (!ctx || !showSelViewport() || !win->cutMask) return false;
	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH)) return false;
	const auto& sel = win->cutMask->maskRect;
	Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRectangleGeometry(sel, clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	const auto xform = D2D1::Matrix3x2F::Scale(scale, scale)
		* D2D1::Matrix3x2F::Translation(sel.left, sel.top - (float)srcY * scale);
	ctx->SetTransform(xform * parentXform);
	return true;
}

bool CapLong::beginLongPreviewAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform)
{
	// 与选区同一套 paintShapes，只是缩放到侧栏；每帧绘制，标注立即可见
	if (!ctx || !imgPreview || imgW <= 0) return false;
	updatePreviewLayout();
	if (previewViewH <= 0.f) return false;
	const auto sz = imgPreview->GetPixelSize();
	const float scale = (float)sz.width / (float)imgW;
	if (scale <= 0.f) return false;
	const float rr = std::min(10.f * win->dpi,
		std::min((previewViewRect.right - previewViewRect.left),
			(previewViewRect.bottom - previewViewRect.top)) * 0.5f);
	Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
	if (FAILED(Ling::D2D::get()->d2dFactory->CreateRoundedRectangleGeometry(
			D2D1::RoundedRect(previewViewRect, rr, rr), clipGeo.GetAddressOf())) || !clipGeo)
		return false;
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	const auto xform = D2D1::Matrix3x2F::Scale(scale, scale)
		* D2D1::Matrix3x2F::Translation(previewViewRect.left, previewViewRect.top - previewPanY);
	ctx->SetTransform(xform * parentXform);
	return true;
}

void CapLong::endLongAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform)
{
	if (!ctx) return;
	ctx->SetTransform(parentXform);
	ctx->PopLayer();
}

bool CapLong::exportPixels(std::vector<BYTE>& out)
{
	if (imgData.empty()) return false;
	return win->composeAnnotOnImage(imgData.data(), imgW, resultH, out);
}

void CapLong::paintStartBtn(ID2D1DeviceContext* ctx)
{
	if (!isShowStartBtn) return;
	ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)circleCenter.x, (float)circleCenter.y), startCircleR, startCircleR), bgBrush.Get());
	ctx->DrawTextLayout({ circleCenter.x - startTextSize.width / 2, circleCenter.y - startTextSize.height / 2 },
		layoutTextStart.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

bool CapLong::hitPreview(POINT clientPos) const
{
	if (previewViewH <= 0.f) return false;
	return clientPos.x >= previewViewRect.left && clientPos.x <= previewViewRect.right
		&& clientPos.y >= previewViewRect.top && clientPos.y <= previewViewRect.bottom;
}

bool CapLong::hitSelViewport(POINT clientPos) const
{
	if (!showSelViewport() || !win->cutMask) return false;
	const auto& r = win->cutMask->maskRect;
	return clientPos.x >= (int)r.left && clientPos.x <= (int)r.right
		&& clientPos.y >= (int)r.top && clientPos.y <= (int)r.bottom;
}

bool CapLong::beginSelViewportPan(POINT clientPos)
{
	if (!hitSelViewport(clientPos) || resultH <= imgH) return false;
	selViewportPanning = true;
	selPanLast = clientPos;
	previewFollowBottom = false;
	previewHot = false;
	ensureSelPreviewBmp();
	win->refresh();
	return true;
}

void CapLong::nudgeInspectBySelDelta(float dyPx)
{
	// 与侧栏预览滚轮同一套灵敏度，避免选区拖/滚过慢
	nudgeInspectByDelta(dyPx);
}

void CapLong::setPreviewPanY(float y, bool fromUser)
{
	const float maxPan = std::max(0.f, previewFullH - previewViewH);
	const float next = std::clamp(y, 0.f, maxPan);
	if (fromUser) {
		previewFollowBottom = (next >= maxPan - 0.5f);
		if (maxPan > 0.5f) inspectRatio = next / maxPan;
	}
	if (std::abs(next - previewPanY) < 0.5f && !fromUser) return;
	previewPanY = next;
	win->refresh();
}

void CapLong::setInspectRatio(float r, bool fromUser)
{
	const float next = std::clamp(r, 0.f, 1.f);
	if (fromUser) previewFollowBottom = (next >= 0.999f);
	if (std::abs(next - inspectRatio) < 0.001f && !fromUser) return;
	inspectRatio = next;
	const float maxPan = std::max(0.f, previewFullH - previewViewH);
	if (maxPan > 0.5f) previewPanY = inspectRatio * maxPan;
	win->refresh();
}

void CapLong::nudgeInspectByDelta(float dyPx)
{
	// 仅滚轮：相对滚动（与点击/拖动的绝对跟手分开）
	dyPx *= 0.28f;
	const float oneScreen = std::max(1.f, previewFullH * ((float)std::max(1, imgH) / (float)std::max(1, resultH)));
	setInspectRatio(inspectRatio + dyPx / (oneScreen * 2.2f), true);
}

void CapLong::scrubInspectAtClientY(float clientY)
{
	updatePreviewLayout();
	if (previewViewH <= 0.f || previewFullH <= 0.f || resultH <= 0) return;

	// 光标落在缩略长图上的内容坐标 → 绿框中心跟手
	float contentY = previewPanY + (clientY - previewViewRect.top);
	contentY = std::clamp(contentY, 0.f, previewFullH);
	const float frameH = std::clamp(
		previewFullH * ((float)std::max(1, imgH) / (float)std::max(1, resultH)),
		1.f, previewFullH);
	const float maxTop = std::max(0.f, previewFullH - frameH);
	const float frameTop = std::clamp(contentY - frameH * 0.5f, 0.f, maxTop);
	const float ratio = maxTop > 0.f ? frameTop / maxTop : 0.f;

	previewFollowBottom = (ratio >= 0.999f);
	inspectRatio = ratio;
	win->refresh();
}

bool CapLong::autoPanPreviewAtEdge(float clientY)
{
	const float maxPan = std::max(0.f, previewFullH - previewViewH);
	if (maxPan < 0.5f || previewViewH <= 0.f) return false;

	const float edge = std::max(36.f * win->dpi, previewViewH * 0.22f);
	const float maxStep = 4.5f * win->dpi; // 缓慢，方便扫完整长图
	const float ly = clientY - previewViewRect.top;
	float dy = 0.f;
	if (ly < edge) {
		const float t = 1.f - std::clamp(ly / edge, 0.f, 1.f);
		dy = -maxStep * (0.35f + 0.65f * t); // 上沿 → 缩略图上滚
	}
	else if (ly > previewViewH - edge) {
		const float t = 1.f - std::clamp((previewViewH - ly) / edge, 0.f, 1.f);
		dy = maxStep * (0.35f + 0.65f * t); // 下沿 → 缩略图下滚
	}
	if (std::abs(dy) < 0.05f) return false;

	const float next = std::clamp(previewPanY + dy, 0.f, maxPan);
	if (std::abs(next - previewPanY) < 0.05f) return false;
	previewPanY = next;
	previewFollowBottom = (previewPanY >= maxPan - 0.5f);
	return true;
}

void CapLong::startPreviewDragPanTimer()
{
	win->killTimer(previewDragPanMsgId);
	win->setTimer(previewDragPanMs, previewDragPanMsgId);
}

void CapLong::stopPreviewDragPanTimer()
{
	win->killTimer(previewDragPanMsgId);
}

void CapLong::ensureSelPreviewBmp()
{
	if (imgData.empty() || imgW <= 0 || resultH <= 0) return;
	if (fullPreviewBmp) {
		const auto s = fullPreviewBmp->GetPixelSize();
		if ((int)s.width == imgW && (int)s.height == resultH) return;
	}
	D2D1_BITMAP_PROPERTIES1 props = {
		.pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
		.dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
	};
	fullPreviewBmp.Reset();
	Ling::D2D::get()->deviceContext->CreateBitmap(
		D2D1::SizeU(imgW, resultH), imgData.data(), imgW * 4, props, fullPreviewBmp.GetAddressOf());
}

void CapLong::syncPierceWithPreviewHot()
{
	if (!isCapturing || isPaused || isFinish) return;
	// 悬停预览时要在选区画大图，必须暂时取消挖洞；离开后继续滚再挖洞
	if (previewHot || previewPanning) {
		win->restoreWin();
		ensureSelPreviewBmp();
	}
	else {
		win->hollowWin();
		raiseTool();
	}
}

void CapLong::setCursor()
{
	POINT cur{};
	GetCursorPos(&cur);
	ScreenToClient(win->hwnd, &cur);
	if (previewHot || previewPanning || selViewportPanning
		|| (isEditMode() && resultH > imgH && hitSelViewport(cur))) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENS));
		return;
	}
	if (!isFinish && !isCapturing && isShowStartBtn) {
		SetCursor(NULL);
	}
	else {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
}

void CapLong::onDown(POINT pos, bool isRight)
{
	if (isRight) return;
	updatePreviewLayout();
	if (hitPreview(pos) && resultH > imgH) {
		previewPanning = true;
		previewHot = true;
		previewFollowBottom = false;
		syncPierceWithPreviewHot();
		if (isPaused || isFinish || isEditMode()) ensureSelPreviewBmp();
		scrubInspectAtClientY((float)pos.y);
		startPreviewDragPanTimer();
	}
}

void CapLong::onMove(POINT pos)
{
	if (selViewportPanning) {
		const float dy = (float)(pos.y - selPanLast.y);
		selPanLast = pos;
		if (std::abs(dy) > 0.01f) nudgeInspectBySelDelta(dy);
		return;
	}
	if (previewPanning) {
		updatePreviewLayout();
		autoPanPreviewAtEdge((float)pos.y);
		scrubInspectAtClientY((float)pos.y);
		return;
	}

	updatePreviewLayout();
	const bool hot = hitPreview(pos);
	if (hot != previewHot) {
		previewHot = hot;
		if (hot) {
			previewFollowBottom = false; // 悬停冻结贴底，便于滚轮/拖动浏览
			inspectRatio = viewRatio();
		}
		else if (isCapturing && !isPaused && !isFinish) {
			previewFollowBottom = true;
			inspectRatio = 1.f;
		}
		syncPierceWithPreviewHot();
		if (hot && (isPaused || isFinish || isEditMode())) ensureSelPreviewBmp();
		win->refresh();
	}

	if (isFinish || isCapturing) {
		if (isShowStartBtn) {
			isShowStartBtn = false;
			win->refresh();
		}
		return;
	}
	circleCenter = pos;
	auto& r = win->cutMask->maskRect;
	if (pos.x > r.left && pos.x < r.right && pos.y > r.top && pos.y < r.bottom) {
		isShowStartBtn = true;
		win->refresh();
	}
	else if (isShowStartBtn) {
		isShowStartBtn = false;
		win->refresh();
	}
}

void CapLong::onUp(POINT pos)
{
	if (selViewportPanning) {
		selViewportPanning = false;
		win->refresh();
		return;
	}
	if (previewPanning) {
		previewPanning = false;
		stopPreviewDragPanTimer();
		syncPierceWithPreviewHot();
		win->refresh();
		return;
	}
	if (isCapturing || isFinish) return;
	if (isShowStartBtn) startCapture();
}

bool CapLong::onWheel(POINT pos, float space)
{
	// 仅侧栏预览可滚轮浏览；截图视窗不响应滚轮翻页
	updatePreviewLayout();
	if (resultH <= imgH) return false;
	if (!hitPreview(pos)) return false;
	previewHot = true;
	previewFollowBottom = false;
	syncPierceWithPreviewHot();
	ensureSelPreviewBmp();
	nudgeInspectByDelta(-space * 0.16f);
	return true;
}

void CapLong::toggleCapture()
{
	if (isFinish) return;
	if (!isCapturing) startCapture();
	else setPaused(!isPaused);
}

void CapLong::startCapture()
{
	if (isCapturing || isFinish) return;
	isCapturing = true;
	isPaused = false;
	isShowStartBtn = false;
	if (!tool) makeTool();
	// 滚动必须挖洞才能 SendInput；不强制挪动用户鼠标
	win->hollowWin();
	ensureExcludeFromCapture(true);
	raiseTool();
	previewFollowBottom = true;
	firstStep();
	syncToolBtn();
	win->refresh();
}

void CapLong::setPaused(bool on)
{
	if (!isCapturing || isFinish || isPaused == on) return;
	if (on) {
		win->killTimer(scrollMsgId);
		win->killTimer(scrollEndMsgId);
		isPaused = true; // 先置位，避免 tryAppendFrame 再排下轮滚动
		// 滚轮已发出、尚未拼帧：等页面停稳再采一帧，避免暂停画面偏上
		if (scrollPending_) {
			scrollPending_ = false;
			Sleep(scrollSettleMs);
			tryAppendFrame();
		}
		win->restoreWin();
		win->ensureNoMousePierce();
		raiseTool();
		previewFollowBottom = true;
		inspectRatio = 1.f;
		makeImgPreview();
	}
	else {
		isPaused = false;
		scrollPending_ = false;
		previewHot = false;
		previewPanning = false;
		selViewportPanning = false;
		stopPreviewDragPanTimer();
		win->hollowWin();
		ensureExcludeFromCapture(true);
		raiseTool();
		previewFollowBottom = true;
		win->setTimer(50, scrollMsgId);
	}
	syncToolBtn();
	win->refresh();
}

void CapLong::onPreviewPanChanged()
{
	win->refresh();
}

void CapLong::syncToolBtn()
{
	if (!tool) return;
	tool->syncCaptureState(isCapturing, isPaused, isFinish);
}

void CapLong::resolveScrollTarget()
{
	auto& r = win->cutMask->maskRect;
	scrollCenterScreen.x = (LONG)((r.left + r.right) * 0.5f);
	scrollCenterScreen.y = (LONG)((r.top + r.bottom) * 0.5f);
	ClientToScreen(win->hwnd, &scrollCenterScreen);
}

void CapLong::sendScrollWheel()
{
	// 与 ScreenCapture 相同：洞内 SendInput；浏览预览时不挪鼠标，避免打断悬停
	resolveScrollTarget();
	POINT old{};
	GetCursorPos(&old);
	SetCursorPos(scrollCenterScreen.x, scrollCenterScreen.y);
	INPUT input = { 0 };
	input.type = INPUT_MOUSE;
	input.mi.dwFlags = MOUSEEVENTF_WHEEL;
	input.mi.mouseData = -WHEEL_DELTA;
	SendInput(1, &input, sizeof(INPUT));
	SetCursorPos(old.x, old.y);
}

void CapLong::onTimerCB(UINT timerId)
{
	// 拖动预览：贴边时持续缓滚缩略图（暂停/结束后也能用）
	if (timerId == previewDragPanMsgId) {
		win->killTimer(previewDragPanMsgId);
		if (!previewPanning) return;
		POINT pt{};
		GetCursorPos(&pt);
		ScreenToClient(win->hwnd, &pt);
		updatePreviewLayout();
		if (autoPanPreviewAtEdge((float)pt.y))
			scrubInspectAtClientY((float)pt.y);
		startPreviewDragPanTimer();
		return;
	}

	if (!isCapturing || isPaused || isFinish) return;
	// 悬停/拖预览时暂停自动滚，否则 SetCursorPos 会把鼠标拽出预览导致无法浏览
	if (isInspecting()) {
		if (timerId == scrollMsgId || timerId == scrollEndMsgId) {
			win->killTimer(timerId);
			win->setTimer(scrollIntervalMs, scrollMsgId);
		}
		return;
	}
	if (timerId == scrollMsgId) {
		win->killTimer(scrollMsgId);
		sendScrollWheel();
		scrollPending_ = true;
		win->setTimer(scrollSettleMs, scrollEndMsgId);
	}
	else if (scrollEndMsgId == timerId) {
		win->killTimer(scrollEndMsgId);
		capStep();
	}
}

void CapLong::firstStep()
{
	auto& maskRect = win->cutMask->maskRect;
	imgW = int(maskRect.right - maskRect.left);
	imgH = int(maskRect.bottom - maskRect.top);
	resultH = imgH;
	capStartPos.x = (int)maskRect.left;
	capStartPos.y = (int)maskRect.top;
	ClientToScreen(win->hwnd, &capStartPos);
	imgData = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
	img1 = imgData;
	previewFollowBottom = true;
	makeImgPreview();
	win->refresh();
	win->setTimer(50, scrollMsgId);
}

void CapLong::makeImgPreview()
{
	imgPreview.Reset();
	if (!tool || imgData.empty() || imgW <= 0 || resultH <= 0) return;

	// 预览底图=原图；标注与选区共用 paintShapes，每帧叠上（即时）
	const float previewTargetW = (42.f * 5.f + ToolbarTheme::shadowPad * 2.f) * tool->dpi;
	const float previewScaleW = previewTargetW / (float)imgW;
	const int previewW = std::max(1, (int)((float)imgW * previewScaleW));
	const int previewH = std::max(1, (int)((float)resultH * previewScaleW));
	std::vector<BYTE> scaledData((size_t)previewW * 4 * previewH);
	for (int y = 0; y < previewH; y++) {
		int srcY = (int)((float)y / previewScaleW);
		if (srcY >= resultH) srcY = resultH - 1;
		for (int x = 0; x < previewW; x++) {
			int srcX = (int)((float)x / previewScaleW);
			if (srcX >= imgW) srcX = imgW - 1;
			int srcIdx = (srcY * imgW + srcX) * 4;
			int dstIdx = (y * previewW + x) * 4;
			scaledData[dstIdx] = imgData[srcIdx];
			scaledData[dstIdx + 1] = imgData[srcIdx + 1];
			scaledData[dstIdx + 2] = imgData[srcIdx + 2];
			scaledData[dstIdx + 3] = imgData[srcIdx + 3];
		}
	}
	D2D1_BITMAP_PROPERTIES1 props = {
		.pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
		.dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
	};
	Ling::D2D::get()->deviceContext->CreateBitmap(
		D2D1::SizeU(previewW, previewH), scaledData.data(), previewW * 4, props, imgPreview.GetAddressOf());
	updatePreviewLayout();

	if (previewHot || previewPanning || selViewportPanning || isPaused || isFinish || isEditMode()) {
		fullPreviewBmp.Reset();
		Ling::D2D::get()->deviceContext->CreateBitmap(
			D2D1::SizeU(imgW, resultH), imgData.data(), imgW * 4, props, fullPreviewBmp.GetAddressOf());
	}
}

void CapLong::capStep()
{
	scrollPending_ = false;
	if (isPaused || isFinish || !isCapturing) return;
	tryAppendFrame();
}

bool CapLong::tryAppendFrame()
{
	auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
	if (firstCheck) {
		changeStartY = -1;
		for (int y = 0; y < imgH; y++) {
			for (int x = 0; x < imgW; x++) {
				int idx = (y * imgW + x) * 4;
				if (img1[idx] != data[idx] || img1[idx + 1] != data[idx + 1] || img1[idx + 2] != data[idx + 2]) {
					if (changeStartY == -1) changeStartY = y;
					break;
				}
			}
			if (changeStartY != -1) break;
		}
		if (changeStartY == -1) {
			dismissTime++;
			if (dismissTime > maxDismissTime) { stopCap(); return false; }
			if (!isPaused) win->setTimer(scrollIntervalMs, scrollMsgId);
			return false;
		}
		firstCheck = false;
	}
	int rowPix{ imgW * 4 };
	int stripH = std::min(comparisonH, imgH - changeStartY);
	if (stripH <= 0) {
		if (!isPaused) win->setTimer(scrollIntervalMs, scrollMsgId);
		return false;
	}
	int img1StripH = imgH - changeStartY;
	auto gray1 = toGrayscale(img1.data() + changeStartY * rowPix, imgW, img1StripH, rowPix);
	auto gray2 = toGrayscale(data.data() + changeStartY * rowPix, imgW, stripH, rowPix);
	int y = findMostSimilarY(gray1.data(), img1StripH, gray2.data(), stripH, imgW);
	if (y == 0) {
		auto gray1Bottom = toGrayscale(img1.data() + (imgH - stripH) * rowPix, imgW, stripH, rowPix);
		auto gray2Bottom = toGrayscale(data.data() + (imgH - stripH) * rowPix, imgW, stripH, rowPix);
		y = findScrollByBottomStrip(gray1Bottom.data(), gray2Bottom.data(), imgW, stripH);
	}
	if (y == 0) {
		if (framesDiffer(data, img1)) {
			if (settleRecheckCount < maxSettleRecheck) {
				settleRecheckCount++;
				if (!isPaused) win->setTimer(settleRecheckMs, scrollEndMsgId);
				return false;
			}
		}
		settleRecheckCount = 0;
		dismissTime++;
		if (dismissTime > maxDismissTime) { stopCap(); return false; }
		if (!isPaused) win->setTimer(scrollIntervalMs, scrollMsgId);
		return false;
	}
	dismissTime = 0;
	settleRecheckCount = 0;
	int paintStart = resultH - (imgH - y - changeStartY);
	int newResultH = paintStart + (imgH - changeStartY);
	std::vector<BYTE> newResult((size_t)rowPix * newResultH);
	CopyMemory(newResult.data(), imgData.data(), imgData.size());
	for (int row = 0; row < imgH - changeStartY; row++) {
		CopyMemory(newResult.data() + (size_t)(paintStart + row) * rowPix, data.data() + (size_t)(changeStartY + row) * rowPix, rowPix);
	}
	imgData = std::move(newResult);
	img1 = data;
	resultH = newResultH;
	if (resultH > 36000) { stopCap(); return false; }
	if (!isPaused) previewFollowBottom = true;
	makeImgPreview();
	win->refresh();
	if (!isPaused) win->setTimer(scrollIntervalMs, scrollMsgId);
	return true;
}

void CapLong::makeTool()
{
	if (tool) return;
	win->clearMainToolUserPlaced();
	tool = std::make_unique<ToolLong>(win);
	layoutTool();
	tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
	SetWindowLongPtr(tool->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(win->hwnd));
	layoutTool();
	SetWindowPos(tool->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	App::excludeFromCapture(tool->hwnd);
	syncToolBtn();
}

void CapLong::raiseTool()
{
	if (!tool || !tool->hwnd) return;
	SetWindowPos(tool->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CapLong::setToolVisible(bool visible)
{
	if (!tool || !tool->hwnd) return;
	if (visible) {
		layoutTool();
		tool->show();
		raiseTool();
	}
	else {
		tool->hide();
	}
}

void CapLong::layoutTool()
{
	if (!tool) return;
	if (win->isMainToolUserPlaced()) return;
	const int toolW = (int)(tool->w + 0.5f);
	const int toolH = (int)(tool->h + 0.5f);
	const int pad = (int)(ToolbarTheme::shadowPad * tool->dpi + 0.5f);
	const int visualH = (std::max)(1, toolH - pad * 2);

	const int maskLeftScr = win->x + (int)win->cutMask->maskRect.left;
	const int maskTopScr = win->y + (int)win->cutMask->maskRect.top;
	const int maskRightScr = win->x + (int)win->cutMask->maskRect.right;
	const int maskBottomScr = win->y + (int)win->cutMask->maskRect.bottom;

	RECT maskScrRect{ maskLeftScr, maskTopScr, maskRightScr, maskBottomScr };
	HMONITOR hMon = MonitorFromRect(&maskScrRect, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(hMon, &mi);

	const int gap = (int)(win->cutMask->strokeWidth + 2.f * tool->dpi + 0.5f);
	const bool fitBelow = (maskBottomScr + gap + visualH) <= mi.rcWork.bottom;
	const bool fitAbove = (maskTopScr - gap - visualH) >= mi.rcWork.top;

	// 与贴图一致：相对截图视窗右对齐
	int toolX = maskRightScr - toolW + pad;
	int toolY = 0;
	if (fitBelow) {
		toolY = maskBottomScr + gap - pad;
	}
	else if (fitAbove) {
		toolY = maskTopScr - gap - toolH + pad;
	}
	else {
		const int overlapPad = (int)(3.f * tool->dpi + 0.5f);
		toolX = maskRightScr - toolW + pad - overlapPad;
		toolY = maskBottomScr - toolH + pad - overlapPad;
	}

	if (toolX < mi.rcWork.left) toolX = mi.rcWork.left;
	if (toolX + toolW > mi.rcWork.right) toolX = mi.rcWork.right - toolW;
	if (toolY < mi.rcWork.top) toolY = mi.rcWork.top;
	if (toolY + toolH > mi.rcWork.bottom) toolY = mi.rcWork.bottom - toolH;
	if (tool->x == toolX && tool->y == toolY) return;
	tool->setPosition(toolX, toolY);
}

bool CapLong::isEditMode() const
{
	return tool && tool->isEditMode();
}

bool CapLong::isPreviewPanning() const
{
	return previewPanning;
}

void CapLong::setEditMode(bool on)
{
	if (!tool) return;
	if (on) {
		if (isCapturing && !isPaused && !isFinish) setPaused(true);
		win->restoreWin();
		win->ensureNoMousePierce();
		raiseTool();
		if (!imgData.empty()) {
			ensureSelPreviewBmp();
			makeImgPreview();
		}
	}
	else {
		tool->cancelAnnotSelect();
	}
	tool->setEditMode(on);
	raiseTool();
	win->refresh();
}

void CapLong::cancelAnnotSelect()
{
	if (tool) tool->cancelAnnotSelect();
}

void CapLong::notifyHistoryChanged()
{
	if (tool) tool->updateUndoEnabled();
}

HWND CapLong::toolHwnd() const
{
	return tool ? tool->hwnd : nullptr;
}

bool CapLong::queryBarAnchor(float& x, float& y, float& w, float& h, float& btnCenterX, float& dpi) const
{
	if (!tool) return false;
	x = (float)tool->x;
	y = (float)tool->y;
	w = tool->w;
	h = tool->h;
	btnCenterX = tool->getBtnCenterX();
	dpi = tool->dpi;
	return true;
}

void CapLong::stopCap()
{
	isFinish = true;
	isCapturing = false;
	isPaused = false;
	makeStopText();
	win->killTimer(scrollMsgId);
	win->killTimer(scrollEndMsgId);
	win->killTimer(previewDragPanMsgId);
	ensureExcludeFromCapture(false);
	win->restoreWin();
	win->ensureNoMousePierce();
	raiseTool();
	previewFollowBottom = false;
	previewHot = false;
	previewPanning = false;
	previewPanY = 0.f;
	inspectRatio = 0.f; // 结束后从顶部浏览；遮罩仍钉在底部当前截图
	makeImgPreview();
	syncToolBtn();
	win->refresh();
}

void CapLong::makeStopText()
{
	if (resultH > 36000) {
		layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooLong"), 13 * win->dpi);
	}
	else {
		layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.reachedBottom"), 13 * win->dpi);
	}
	if (!layoutTextEnd) return;
	DWRITE_TEXT_METRICS tm = {};
	layoutTextEnd->GetMetrics(&tm);
	auto& maskRect = win->cutMask->maskRect;
	auto halfX = maskRect.left + (maskRect.right - maskRect.left) / 2;
	auto halfW = tm.width / 2;
	float padding{ 8 * win->dpi };
	stopTextRect.left = halfX - halfW - padding;
	stopTextRect.top = maskRect.bottom - 30 * win->dpi - padding;
	stopTextRect.right = halfX + halfW + padding;
	stopTextRect.bottom = maskRect.bottom - padding;
	layoutTextEnd->SetMaxWidth(stopTextRect.right - stopTextRect.left);
	layoutTextEnd->SetMaxHeight(stopTextRect.bottom - stopTextRect.top);
	stopTextPos = { halfX - halfW, stopTextRect.top + (stopTextRect.bottom - stopTextRect.top - tm.height) / 2 };
}

void CapLong::copyToClipboard()
{
	std::vector<BYTE> pixels;
	if (!exportPixels(pixels)) return;
	Util::saveToClipboard(imgW, resultH, pixels.data());
	Setting::get()->addHistoryItem(pixels, imgW, resultH, L"capture");
}

bool CapLong::saveToFile()
{
	std::vector<BYTE> pixels;
	if (!exportPixels(pixels)) return false;
	auto path = Util::getSaveFilePath(win->hwnd);
	if (path.empty()) return false;
	if (!Util::saveToFile(path, imgW, resultH, pixels.data())) return false;
	Setting::get()->addHistoryItem(pixels, imgW, resultH, L"capture");
	return true;
}

void CapLong::pin()
{
	std::vector<BYTE> pixels;
	if (!exportPixels(pixels)) return;
	auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(monitor, &mi);
	auto& workArea = mi.rcWork;
	int screenW = workArea.right - workArea.left;
	int screenH = workArea.bottom - workArea.top;
	int posX = workArea.left + (screenW - imgW) / 2;
	int posY = workArea.top + (screenH - std::min(resultH, screenH)) / 2;
	WinPin::initFromData(posX, posY, imgW, resultH, pixels);
}
