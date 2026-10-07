#include "pch.h"
#include "App.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "Tool/SerialEmoji.h"
#include "History.h"
#include "ShapeNumber.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float kPi = 3.14159265358979323846f;
}

ShapeNumber::ShapeNumber(AnnotHost* win) :ShapeBase(win),
	r{ win->toolSub->getSliderVal() },
	styleKind{ win->toolSub->numberStyle },
	val{ getNextVal(win, win->toolSub->numberStyle) },
	emoji{ win->toolSub->numberStyle == ToolSub::NumberEmoji
		? win->toolSub->serialEmoji : std::wstring{} }
{
	if (isEmoji())
		draggers.assign(8, D2D1::RectF(0, 0, 0, 0));
	else
		draggers.assign(3, D2D1::RectF(0, 0, 0, 0));
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0XFFFFFF), brushText.GetAddressOf());
	if (emoji.empty()) emoji = SerialEmoji::Default;
	isFill = true;
}

bool ShapeNumber::isEmoji() const
{
	return styleKind == ToolSub::NumberEmoji;
}

int ShapeNumber::getNextVal(AnnotHost* win, int style)
{
	int maxVal{ 0 };
	for (auto& shape : win->history->shapes) {
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && !number->isUndo && number->styleKind == style && number->val > maxVal) {
			maxVal = number->val;
		}
	}
	return maxVal + 1;
}

std::wstring ShapeNumber::formatLabel() const
{
	if (styleKind == ToolSub::NumberLetter) {
		std::wstring s;
		int n = val;
		while (n > 0) {
			n -= 1;
			s.insert(s.begin(), (wchar_t)(L'A' + (n % 26)));
			n /= 26;
		}
		return s.empty() ? L"A" : s;
	}
	if (styleKind == ToolSub::NumberEmoji) {
		return emoji.empty() ? SerialEmoji::Default : emoji;
	}
	return std::to_wstring(val);
}

ShapeNumber::~ShapeNumber()
{
}

void ShapeNumber::makeTextLayout()
{
	auto d2d = Ling::D2D::get();
	auto text = formatLabel();
	float layoutSide = r * 2.f;
	if (isEmoji()) layoutSide = r * 2.4f;
	d2d->dwriteFactory->CreateTextLayout(text.data(), (UINT32)text.length(),
		d2d->baseTextFormat.Get(), layoutSide, layoutSide, layoutText.ReleaseAndGetAddressOf());
	if (!layoutText) return;
	layoutText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	layoutText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	if (isEmoji()) {
		layoutText->SetFontFamilyName(L"Segoe UI Emoji", { 0, (UINT32)text.length() });
		layoutText->SetFontSize(r, { 0, (UINT32)text.length() });
	}
	else {
		layoutText->SetFontSize(r, { 0, (UINT32)text.length() });
	}
}

D2D1_POINT_2F ShapeNumber::boxCenter() const
{
	return { (rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f };
}

void ShapeNumber::syncRectFromCenter()
{
	const float half = r * 0.6f;
	rect = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
}

void ShapeNumber::syncCenterFromRect()
{
	cx = (rect.left + rect.right) * 0.5f;
	cy = (rect.top + rect.bottom) * 0.5f;
	const float side = std::max(rect.right - rect.left, rect.bottom - rect.top);
	r = side / 1.2f;
	const float minR = 8.f * win->dpi;
	if (r < minR) r = minR;
	r = win->toolSub->setShapeSliderVal(L"number", r);
	// Keep square box from size
	const float half = r * 0.6f;
	rect = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
}

void ShapeNumber::toLocal(float x, float y, float& lx, float& ly) const
{
	if (std::fabs(boxAngle) < 1e-6f) {
		lx = x;
		ly = y;
		return;
	}
	const auto c = boxCenter();
	const float dx = x - c.x, dy = y - c.y;
	const float ca = std::cos(-boxAngle), sa = std::sin(-boxAngle);
	lx = c.x + dx * ca - dy * sa;
	ly = c.y + dx * sa + dy * ca;
}

void ShapeNumber::withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	if (std::fabs(boxAngle) < 1e-6f) {
		draw();
		return;
	}
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	const auto c = boxCenter();
	ctx->SetTransform(D2D1::Matrix3x2F::Rotation(boxAngle * 180.f / kPi, c) * old);
	draw();
	ctx->SetTransform(old);
}

void ShapeNumber::paint(ID2D1DeviceContext* ctx)
{
	if (!layoutText) return;
	if (!isEmoji()) {
		if (!path) return;
		ctx->FillGeometry(path.Get(), brush.Get());
		ctx->DrawTextLayout({ cx - r, cy - r }, layoutText.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
		return;
	}
	// Emoji：彩色字形 + 包围盒旋转（与矩形相同）
	withRotation(ctx, [&] {
		const float box = r * 1.2f;
		ctx->DrawTextLayout({ cx - box, cy - box }, layoutText.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
	});
}

void ShapeNumber::paintDragger(ID2D1DeviceContext* ctx)
{
	if (isWheel) return;
	if (!isEmoji()) {
		for (auto& dragger : draggers) paintHandle(ctx, dragger);
		return;
	}
	// 与 ShapeArea / 矩形相同：选框 + 旋转柄 + 四角（无边中点）
	withRotation(ctx, [&] {
		ctx->DrawRectangle(
			D2D1::RectF(rect.left - 1.f, rect.top - 1.f, rect.right + 1.f, rect.bottom + 1.f),
			brushHandleBorder.Get(), win->dpi);
		const float bx = (rect.left + rect.right) * 0.5f;
		ctx->DrawLine(D2D1::Point2F(bx, rect.top),
			D2D1::Point2F(bx, (rotateHandle.top + rotateHandle.bottom) * 0.5f),
			brushHandleBorder.Get(), 2.f * win->dpi);
		static const int corners[] = { 0, 2, 4, 6 };
		for (int i : corners) paintHandle(ctx, draggers[i]);
		paintHandle(ctx, rotateHandle);
	});
}

void ShapeNumber::mouseDrag(const float x, const float y)
{
	if (!isEmoji()) {
		if (hoverDraggerIndex == 0) {
			auto spanX{ x - pressX };
			auto spanY{ y - pressY };
			cx += spanX;
			cy += spanY;
			makePath();
			pressX = x;
			pressY = y;
		}
		else if (hoverDraggerIndex == 1) {
			angle = -atan2f(y - cy, x - cx) * 180.f / kPi;
			makePath();
		}
		else if (hoverDraggerIndex == 2) {
			auto dx{ x - cx };
			auto dy{ y - cy };
			r = sqrtf(dx * dx + dy * dy);
			auto minR{ 8.f * win->dpi };
			if (r < minR) r = minR;
			r = win->toolSub->setShapeSliderVal(L"number", r);
			makePath();
			makeTextLayout();
		}
		updateDraggers();
		return;
	}

	// Emoji：矩形系拖拽
	if (hoverDraggerIndex == 9) {
		const auto c = boxCenter();
		boxAngle = std::atan2(y - c.y, x - c.x) + kPi * 0.5f;
		updateDraggers();
		return;
	}
	float lx = x, ly = y;
	if (hoverDraggerIndex != 8) toLocal(x, y, lx, ly);
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4 || hoverDraggerIndex == 2 || hoverDraggerIndex == 6) {
		auto [left, right] = std::minmax(pressX, lx);
		auto [top, bottom] = std::minmax(pressY, ly);
		rect = D2D1::RectF(left, top, right, bottom);
		// Emoji 保持正方形
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		if (w > h) rect.bottom = rect.top + w;
		else rect.right = rect.left + h;
		syncCenterFromRect();
		makeTextLayout();
	}
	else if (hoverDraggerIndex == 8) {
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		rect.left = x - pressX;
		rect.top = y - pressY;
		rect.right = rect.left + w;
		rect.bottom = rect.top + h;
		cx = (rect.left + rect.right) * 0.5f;
		cy = (rect.top + rect.bottom) * 0.5f;
	}
	updateDraggers();
}

void ShapeNumber::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) {
		cx = x;
		cy = y;
		pressX = cx;
		pressY = cy;
		if (isEmoji()) {
			syncRectFromCenter();
			hoverDraggerIndex = 8;
			// 记的是「抓取偏移」而不是按下点的绝对坐标：mouseDrag 的移动分支算的是
			// rect.left = x - pressX。之前存绝对坐标，只要按下时鼠标动 1px（hasDragged），
			// Emoji 就会被甩到 (x-pressX, y-pressY)（≈ 画布左上角）→ 看着像"点了没落上"
			pressX = x - rect.left;
			pressY = y - rect.top;
			makeTextLayout();
			updateDraggers();
		}
		else {
			hoverDraggerIndex = 0;
			makePath();
			makeTextLayout();
		}
		win->refresh();
		return;
	}
	if (!isEmoji()) {
		pressX = x;
		pressY = y;
		return;
	}
	if (hoverDraggerIndex == 9) return;
	if (hoverDraggerIndex == 8) {
		pressX = x - rect.left;
		pressY = y - rect.top;
		return;
	}
	float lx, ly;
	toLocal(x, y, lx, ly);
	if (hoverDraggerIndex == 0) { pressX = rect.right; pressY = rect.bottom; }
	else if (hoverDraggerIndex == 2) { pressX = rect.left; pressY = rect.bottom; }
	else if (hoverDraggerIndex == 4) { pressX = rect.left; pressY = rect.top; }
	else if (hoverDraggerIndex == 6) { pressX = rect.right; pressY = rect.top; }
}

void ShapeNumber::mouseUp(const float x, const float y)
{
	(void)x; (void)y;
	updateDraggers();
}

void ShapeNumber::updateDraggers()
{
	const float half = draggerSize * 0.5f;
	if (!isEmoji()) {
		draggers[0] = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
		draggers[1] = D2D1::RectF(tip.x - half, tip.y - half, tip.x + half, tip.y + half);
		draggers[2] = D2D1::RectF(mid.x - half, mid.y - half, mid.x + half, mid.y + half);
		return;
	}
	const float w = rect.right - rect.left;
	const float h = rect.bottom - rect.top;
	draggers[0] = D2D1::RectF(rect.left - half, rect.top - half, rect.left + half, rect.top + half);
	draggers[1] = D2D1::RectF(rect.left + w * 0.5f - half, rect.top - half, rect.left + w * 0.5f + half, rect.top + half);
	draggers[2] = D2D1::RectF(rect.right - half, rect.top - half, rect.right + half, rect.top + half);
	draggers[3] = D2D1::RectF(rect.right - half, rect.top + h * 0.5f - half, rect.right + half, rect.top + h * 0.5f + half);
	draggers[4] = D2D1::RectF(rect.right - half, rect.bottom - half, rect.right + half, rect.bottom + half);
	draggers[5] = D2D1::RectF(rect.left + w * 0.5f - half, rect.bottom - half, rect.left + w * 0.5f + half, rect.bottom + half);
	draggers[6] = D2D1::RectF(rect.left - half, rect.bottom - half, rect.left + half, rect.bottom + half);
	draggers[7] = D2D1::RectF(rect.left - half, rect.top + h * 0.5f - half, rect.left + half, rect.top + h * 0.5f + half);
	const float rotY = rect.top - 20.f * win->dpi;
	const float bx = (rect.left + rect.right) * 0.5f;
	rotateHandle = D2D1::RectF(bx - half, rotY - half, bx + half, rotY + half);
}

void ShapeNumber::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	if (!isEmoji()) {
		if (isInRect(draggers[0], x, y))
			hoverDraggerIndex = 0;
		else if (isInRect(draggers[1], x, y))
			hoverDraggerIndex = 1;
		else if (isInRect(draggers[2], x, y))
			hoverDraggerIndex = 2;
		else if (hitErase(x, y))
			hoverDraggerIndex = 0;
	}
	else {
		float lx, ly;
		toLocal(x, y, lx, ly);
		const float rotPad = 8.f * win->dpi;
		const auto rotHit = D2D1::RectF(
			rotateHandle.left - rotPad, rotateHandle.top - rotPad,
			rotateHandle.right + rotPad, rotateHandle.bottom + rotPad);
		if (isInRect(rotHit, lx, ly)) {
			hoverDraggerIndex = 9;
		}
		else {
			const int edge = hitBoxEdgeOrCorner(lx, ly, rect, std::max(8.f * win->dpi, 6.f * win->dpi));
			if (edge == 0 || edge == 2 || edge == 4 || edge == 6)
				hoverDraggerIndex = edge;
			else if (lx >= rect.left && lx <= rect.right && ly >= rect.top && ly <= rect.bottom)
				hoverDraggerIndex = 8;
		}
	}
	if (isWheel) {
		isWheel = false;
		mouseUp(x, y);
		win->refresh();
	}
}

void ShapeNumber::mouseWheel(const float x, const float y, const short delta)
{
	isWheel = true;
	if (delta < 0) {
		if (r <= 6.f * win->dpi) return;
		r--;
	}
	else {
		r++;
	}
	r = win->toolSub->setShapeSliderVal(L"number", r);
	if (isEmoji()) {
		syncRectFromCenter();
		makeTextLayout();
		updateDraggers();
	}
	else {
		makePath();
		makeTextLayout();
	}
	win->refresh();
}

void ShapeNumber::setCursor()
{
	if (applyUnselectedHoverCursor()) return;
	if (!isEmoji()) {
		if (hoverDraggerIndex >= 0)
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		else
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return;
	}
	if (hoverDraggerIndex == 9) SetCursor(LoadCursor(nullptr, IDC_HAND));
	else if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4) SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
	else if (hoverDraggerIndex == 2 || hoverDraggerIndex == 6) SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
	else if (hoverDraggerIndex == 8) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	else SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapeNumber::hitErase(const float x, const float y)
{
	if (isEmoji()) {
		float lx, ly;
		toLocal(x, y, lx, ly);
		return lx >= rect.left && lx <= rect.right && ly >= rect.top && ly <= rect.bottom;
	}
	if (!path) return false;
	BOOL contains = FALSE;
	path->FillContainsPoint({ x, y }, nullptr, &contains);
	if (contains) return true;
	path->StrokeContainsPoint({ x, y }, win->dpi * 2.f, nullptr, nullptr, &contains);
	return contains == TRUE;
}

D2D1_POINT_2F ShapeNumber::localPoint(const float degrees)
{
	float radians = degrees * kPi / 180.f;
	return D2D1::Point2F(r * cosf(radians), -r * sinf(radians));
}

D2D1_POINT_2F ShapeNumber::transformPoint(const D2D1_POINT_2F& point)
{
	float radians = -angle * kPi / 180.f;
	float cosValue = cosf(radians);
	float sinValue = sinf(radians);
	return D2D1::Point2F(
		cx + point.x * cosValue - point.y * sinValue,
		cy + point.x * sinValue + point.y * cosValue
	);
}

void ShapeNumber::makePath()
{
	if (isEmoji()) return;
	auto d2d = Ling::D2D::get();
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	auto start = transformPoint(localPoint(10.f));
	mid = transformPoint(localPoint(180.f));
	auto end = transformPoint(localPoint(350.f));
	tip = transformPoint(D2D1::Point2F(r + r / 3.f, 0.f));
	sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
	sink->AddArc(D2D1::ArcSegment(mid, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	sink->AddLine(tip);
	sink->AddLine(start);
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
}
