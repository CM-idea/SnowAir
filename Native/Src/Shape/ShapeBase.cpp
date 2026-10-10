#include "pch.h"
#include "App.h"
#include "../Win/AnnotHost.h"
#include "ShapeBase.h"
#include "AnnotCursor.h"
#include <cmath>

ShapeBase::ShapeBase(AnnotHost* win) :win{ win },
	// 内圆直径 8、命中约 12
	draggerSize{ 8.f * win->dpi },
	hitPad{ 2.f * win->dpi }
{
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushHandleFill.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x34C759), brushHandleBorder.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 64.f / 255.f), brushHandleShadow.GetAddressOf());
	brushDragger = brushHandleBorder;
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y)
{
	return (x > rect.left - hitPad && x < rect.right + hitPad
		&& y > rect.top - hitPad && y < rect.bottom + hitPad);
}

int ShapeBase::hitBoxEdgeOrCorner(float x, float y, const D2D1_RECT_F& r, float band) const
{
	const float corner = std::max(band * 1.2f, draggerSize);
	const bool nearL = x >= r.left - band && x <= r.left + corner;
	const bool nearR = x >= r.right - corner && x <= r.right + band;
	const bool nearT = y >= r.top - band && y <= r.top + corner;
	const bool nearB = y >= r.bottom - corner && y <= r.bottom + band;
	const bool inX = x >= r.left - band && x <= r.right + band;
	const bool inY = y >= r.top - band && y <= r.bottom + band;
	if (nearL && nearT) return 0;
	if (nearR && nearT) return 2;
	if (nearR && nearB) return 4;
	if (nearL && nearB) return 6;
	if (nearT && inX) return 1;
	if (nearB && inX) return 5;
	if (nearL && inY) return 7;
	if (nearR && inY) return 3;
	return -1;
}

// 角外旋转圈：内侧收到 4（= 缩放柄半径 paintHandle rInner，等于"手一离开角柄就转"），
// 外侧 26。注意内侧不能大于角柄的命中带，否则角缩放会抢手。
static constexpr float kRotateInnerPx = 4.f;
static constexpr float kRotateRingPx = 26.f;

int ShapeBase::hitRotateRing(float lx, float ly, const D2D1_RECT_F& r) const
{
	// 框内不旋转（框内是移动/缩放）
	if (lx >= r.left && lx <= r.right && ly >= r.top && ly <= r.bottom) return -1;
	const float inner = kRotateInnerPx * win->dpi;
	const float outer = kRotateRingPx * win->dpi;
	const D2D1_POINT_2F corners[4]{
		{ r.left, r.top }, { r.right, r.top }, { r.right, r.bottom }, { r.left, r.bottom }
	};
	const int idx[4]{ 0, 2, 4, 6 }; // 与 hitBoxEdgeOrCorner 的角序号一致
	for (int i = 0; i < 4; i++) {
		const float d = std::hypot(lx - corners[i].x, ly - corners[i].y);
		if (d > inner && d <= outer) return idx[i];
	}
	return -1;
}

void ShapeBase::setRotateCursor(int corner, float angleRad)
{
	HCURSOR cur = AnnotCursor::rotate(corner, angleRad);
	if (cur) SetCursor(cur);
	else SetCursor(LoadCursor(nullptr, IDC_HAND));
}

void ShapeBase::setIconCursor(wchar_t code, float angleRad)
{
	HCURSOR cur = AnnotCursor::glyph(code, angleRad);
	if (cur) SetCursor(cur);
	else SetCursor(LoadCursor(nullptr, IDC_HAND));
}

int ShapeBase::dotIndexFromCorner(int corner)
{
	switch (corner) {
	case 0: return 10;
	case 2: return 11;
	case 4: return 12;
	default: return 13;
	}
}

D2D1_POINT_2F ShapeBase::cornerDotPos(int dotIndex, const D2D1_RECT_F& r, float inset)
{
	const float in = std::min(inset, std::min(r.right - r.left, r.bottom - r.top) * 0.28f);
	switch (dotIndex) {
	case 10: return { r.left + in, r.top + in };
	case 11: return { r.right - in, r.top + in };
	case 12: return { r.right - in, r.bottom - in };
	default: return { r.left + in, r.bottom - in };
	}
}

int ShapeBase::hitCornerDot(float lx, float ly, const D2D1_RECT_F& r) const
{
	const float inset = 14.f * win->dpi;
	const float hit = 7.f * win->dpi;
	for (int i = 10; i <= 13; i++) {
		const auto p = cornerDotPos(i, r, inset);
		if (std::hypot(lx - p.x, ly - p.y) <= hit) return i;
	}
	return -1;
}

void ShapeBase::paintCornerDot(ID2D1DeviceContext* ctx, D2D1_POINT_2F p, bool active)
{
	// 与四角缩放柄完全同一套尺寸/配色（rOuter 5 / rInner 4 / 1.5px 描边），
	// 区别只有"该角已选中"时填主题绿
	const float rOuter = 5.f * win->dpi;
	const float rInner = 4.f * win->dpi;
	ctx->FillEllipse(D2D1::Ellipse(p, rOuter, rOuter), brushHandleShadow.Get());
	ctx->FillEllipse(D2D1::Ellipse(p, rInner, rInner),
		active ? brushHandleBorder.Get() : brushHandleFill.Get());
	ctx->DrawEllipse(D2D1::Ellipse(p, rInner, rInner), brushHandleBorder.Get(), 1.5f * win->dpi);
}

void ShapeBase::paintHandle(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect)
{
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;
	const float rOuter = 5.f * win->dpi;
	const float rInner = 4.f * win->dpi;
	ctx->FillEllipse(D2D1::Ellipse({ cx, cy }, rOuter, rOuter), brushHandleShadow.Get());
	ctx->FillEllipse(D2D1::Ellipse({ cx, cy }, rInner, rInner), brushHandleBorder.Get());
	ctx->DrawEllipse(D2D1::Ellipse({ cx, cy }, rInner, rInner), brushHandleFill.Get(), 1.5f * win->dpi);
}

bool ShapeBase::isSelected() const
{
	return win && win->shapeHover == this;
}

bool ShapeBase::applyUnselectedHoverCursor()
{
	// 文本工具停在线上（**选中与否都算**）：一律工字型。
	// 这段必须排在 isSelected() 之前 —— 否则"已选中的线段"会走子类的移动/缩放光标（↔），
	// 二次编辑线上文字时鼠标就变成双向箭头了
	if (win && win->getCurToolId() == L"text") {
		POINT p{};
		if (GetCursorPos(&p) && ScreenToClient(win->hwnd, &p)) {
			const auto img = win->clientToAnnotPt((float)p.x, (float)p.y);
			if (hitErase((float)img.x, (float)img.y)) {
				SetCursor(LoadCursor(nullptr, IDC_IBEAM));
				return true;
			}
		}
	}
	if (isSelected() || hoverDraggerIndex < 0) return false;
	// 未选中标注上提示可点选二次编辑
	SetCursor(LoadCursor(nullptr, IDC_HAND));
	return true;
}
