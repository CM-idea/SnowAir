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

// 选中框统一虚线（所有形状共用一份，避免每个形状各建一次 stroke style）
ID2D1StrokeStyle* ShapeBase::selectionStroke()
{
	static Microsoft::WRL::ComPtr<ID2D1StrokeStyle> style;
	if (!style) {
		auto factory = Ling::D2D::get()->d2dFactory.Get();
		if (factory) {
			float dashes[] = { 4.f, 3.f };
			factory->CreateStrokeStyle(
				D2D1::StrokeStyleProperties(
					D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
					D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
				dashes, 2, style.GetAddressOf());
		}
	}
	return style.Get();
}

// 控件相对标注本体四周留 2px 间隔；选框和四角/四边控点都用这个外扩后的 rect
D2D1_RECT_F ShapeBase::selectionRect(const D2D1_RECT_F& r) const
{
	const float g = 2.f * win->dpi + selectionExtra();
	return D2D1::RectF(r.left - g, r.top - g, r.right + g, r.bottom + g);
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

// 记录"定边"锚点：四角 = 对角，四边 = 对边中点；新建时锚点就是按下点。
// 锚点取世界坐标（按当前 angle 反旋转那个角），拖动时它始终不动。
void ShapeBase::beginScale(const D2D1_RECT_F& r, float angleRad, int handle, float pressX, float pressY)
{
	scaleCreating = handle < 0;
	scaleStartAngle = angleRad;
	scaleHandle = scaleCreating ? 4 : handle;
	scaleStartRect = r;
	if (scaleCreating) {
		scaleStartRect = D2D1::RectF(pressX, pressY, pressX, pressY);
		scaleAnchor = D2D1::Point2F(pressX, pressY);
		return;
	}
	const float cx = (r.left + r.right) * 0.5f;
	const float cy = (r.top + r.bottom) * 0.5f;
	D2D1_POINT_2F local{ cx, cy };
	switch (handle) {
	case 0: local = { r.right, r.bottom }; break; // 左上 → 右下
	case 1: local = { cx, r.bottom }; break;      // 上边 → 下边中点
	case 2: local = { r.left, r.bottom }; break;
	case 3: local = { r.left, cy }; break;        // 右边 → 左边中点
	case 4: local = { r.left, r.top }; break;
	case 5: local = { cx, r.top }; break;
	case 6: local = { r.right, r.top }; break;
	case 7: local = { r.right, cy }; break;
	default: break;
	}
	const float ca = std::cos(angleRad), sa = std::sin(angleRad);
	const float dx = local.x - cx, dy = local.y - cy;
	scaleAnchor = D2D1::Point2F(cx + dx * ca - dy * sa, cy + dx * sa + dy * ca);
}

// 按锚点重建 rect：把指针投影到形状自身的两条轴上，固定边（锚点）不动。
// Shift 等比时以"变化更大的那条轴"为准，另一条按起始比例跟上，避免角飞出光标。
D2D1_RECT_F ShapeBase::dragScale(float x, float y, bool keepAspect) const
{
	if (scaleHandle < 0) return scaleStartRect;
	const float ca = std::cos(scaleStartAngle), sa = std::sin(scaleStartAngle);
	const float dx = x - scaleAnchor.x, dy = y - scaleAnchor.y;
	const float du = dx * ca + dy * sa;    // 沿形状自身 x 轴
	const float dv = -dx * sa + dy * ca;   // 沿形状自身 y 轴
	const float sw = std::max(0.f, scaleStartRect.right - scaleStartRect.left);
	const float sh = std::max(0.f, scaleStartRect.bottom - scaleStartRect.top);
	const bool corner = (scaleHandle % 2) == 0;          // 0/2/4/6
	const bool dragW = (scaleHandle == 3 || scaleHandle == 7); // 左右边：宽度跟手

	float w = 0.f, h = 0.f;    // 新宽高
	float ox = 0.f, oy = 0.f;  // 中心相对锚点的偏移（沿两条轴）
	if (corner) {
		if (keepAspect) {
			const float sx = sw > 0.001f ? std::fabs(du) / sw : 0.f;
			const float sy = sh > 0.001f ? std::fabs(dv) / sh : 0.f;
			const float s = std::max(sx, sy);
			if (sw > 0.001f && sh > 0.001f) { w = sw * s; h = sh * s; }
			else { w = h = std::max(std::fabs(du), std::fabs(dv)); } // 新建：正方形/正圆
			ox = std::copysign(w * 0.5f, du);
			oy = std::copysign(h * 0.5f, dv);
		}
		else {
			w = std::fabs(du);
			h = std::fabs(dv);
			ox = du * 0.5f;
			oy = dv * 0.5f;
		}
	}
	else {
		// 边：对边固定；等比时另一轴以对边中点为中心对称展开
		const float driven = dragW ? du : dv;
		const float base = dragW ? sw : sh;
		const float other = dragW ? sh : sw;
		const float s = (keepAspect && base > 0.001f) ? std::fabs(driven) / base : 1.f;
		w = dragW ? std::fabs(driven) : other * s;
		h = dragW ? other * s : std::fabs(driven);
		ox = dragW ? driven * 0.5f : 0.f;
		oy = dragW ? 0.f : driven * 0.5f;
	}
	const D2D1_POINT_2F c{ scaleAnchor.x + ca * ox - sa * oy,
						   scaleAnchor.y + sa * ox + ca * oy };
	return D2D1::RectF(c.x - w * 0.5f, c.y - h * 0.5f, c.x + w * 0.5f, c.y + h * 0.5f);
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
