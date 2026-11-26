#include "pch.h"
#include "App.h"
#include "Win/AnnotHost.h"
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
	// 0..7 = 矩形系八向控件；圆形控件另用 1（尾巴尖）/ 3（旋转柄）
	draggers.assign(8, D2D1::RectF(0, 0, 0, 0));
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0XFFFFFF), brushText.GetAddressOf());
	if (emoji.empty()) emoji = SerialEmoji::Default;
	if (!isEmoji()) noTail_ = win->toolSub->isNumberNoTail;
	if (noTail_) syncRectFromCenter();
	// 属性栏「当前序号」输入框：手动指定的数字或文字优先于自动递增
	{
		std::wstring label;
		val = win->toolSub->takeNumberValue(styleKind, val, label);
		if (!label.empty()) labelText = label;
	}
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
	// 属性栏手动输入的序号（例如中文）直接显示
	if (!labelText.empty()) return labelText;
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

void ShapeNumber::setDisplayLabel(const std::wstring& text)
{
	labelText = text;
	makeTextLayout();
	win->refresh();
}

void ShapeNumber::applyStyle()
{
	auto* ts = win->toolSub.get();
	if (!ts) return;
	r = std::max(6.f, ts->getSliderVal());
	isFill = ts->isNumberFill;
	if (!isEmoji()) noTail_ = ts->isNumberNoTail;
	brush->SetColor(ts->getSelectedColor());
	tailAnchored_ = false;
	makePath();
	makeTextLayout();
	if (isEmoji() || noTail_) syncRectFromCenter();
	updateDraggers();
	win->refresh();
}

void ShapeNumber::syncToolFromShape() const
{
	auto* ts = win->toolSub.get();
	if (!ts) return;
	ts->numberStyle = styleKind;
	ts->isNumberFill = isFill;
	if (!isEmoji()) ts->isNumberNoTail = noTail_;
	ts->setShapeSliderVal(L"number", r);
}

bool ShapeNumber::retractTail()
{
	if (noTail_) return false;
	// 初始状态：尾巴正对文字下方，长度就是默认短尾巴
	const float homeAngle = 270.f - badgeAngle * 180.f / kPi;
	// 角度按 360 取模比较：-90 与 270 是同一个方向，别被当成“还没复位”
	const float diff = std::fmod(std::fmod(angle - homeAngle, 360.f) + 540.f, 360.f) - 180.f;
	const bool atHome = std::fabs(tailFactor_ - 4.f / 3.f) < 1e-3f && std::fabs(diff) < 0.5f;
	if (atHome) return false; // 已经复位，这一下按键才是删除
	// Delete / Backspace 先把尾巴复位到初始位置和初始长度；再按一次才删标注
	tailFactor_ = 4.f / 3.f;
	angle = homeAngle;
	tailAnchored_ = false;
	makePath();
	updateDraggers();
	win->refresh();
	return true;
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
	return ShapeBase::boxCenter(rect);
}

void ShapeNumber::syncRectFromCenter()
{
	// Emoji：包围盒按 1.2r（字形可视范围）；停用尾巴：正方形正好框住圆形徽章
	const float half = isEmoji() ? r * 0.6f : r;
	rect = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
}

void ShapeNumber::syncCenterFromRect()
{
	cx = (rect.left + rect.right) * 0.5f;
	cy = (rect.top + rect.bottom) * 0.5f;
	const float side = std::max(rect.right - rect.left, rect.bottom - rect.top);
	r = isEmoji() ? side / 1.2f : side * 0.5f;
	const float minR = 8.f * win->dpi;
	if (r < minR) r = minR;
	r = win->toolSub->setShapeSliderVal(L"number", r);
	// Keep square box from size
	const float half = isEmoji() ? r * 0.6f : r;
	rect = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
}

void ShapeNumber::toLocal(float x, float y, float& lx, float& ly) const
{
	ShapeBase::boxToLocal(rect, boxAngle, x, y, lx, ly);
}

void ShapeNumber::withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	ShapeBase::withBoxRotation(ctx, rect, boxAngle, draw);
}

void ShapeNumber::withBadgeRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	if (std::fabs(badgeAngle) < 1e-6f) {
		draw();
		return;
	}
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	ctx->SetTransform(D2D1::Matrix3x2F::Rotation(badgeAngle * 180.f / kPi,
		D2D1::Point2F(cx, cy)) * old);
	draw();
	ctx->SetTransform(old);
}

D2D1_POINT_2F ShapeNumber::rotateHandlePos() const
{
	// 局部顶部 (0,-R) 绕徽章中心顺时针转 badgeAngle
	const float R = controlRadius();
	return D2D1::Point2F(cx + R * sinf(badgeAngle), cy - R * cosf(badgeAngle));
}

void ShapeNumber::paint(ID2D1DeviceContext* ctx)
{
	if (!layoutText) return;
	if (!isEmoji()) {
		if (!path) return;
		if (noTail_) {
			// 停用尾巴：与矩形一致的可旋转徽章，圆本身转不转一样，文字跟着转
			withRotation(ctx, [&] {
				ctx->FillGeometry(path.Get(), brush.Get());
				ctx->DrawTextLayout({ cx - r, cy - r }, layoutText.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
			});
			return;
		}
		ctx->FillGeometry(path.Get(), brush.Get());
		// 数字随旋转柄一起倾斜；圆本身对称，转的是文字
		withBadgeRotation(ctx, [&] {
			ctx->DrawTextLayout({ cx - r, cy - r }, layoutText.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
		});
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
	if (boxMode()) {
		// 与矩形一致：虚线选框 + 四角，不再有单独的旋转柄
		withRotation(ctx, [&] {
			ctx->DrawRectangle(selectionRect(rect), brushHandleBorder.Get(), win->dpi, selectionStroke());
			static const int corners[] = { 0, 2, 4, 6 };
			for (int i : corners) paintHandle(ctx, draggers[i]);
		});
		return;
	}
	// 圆形控件：大同心圆虚线 + 尾巴尖控制点 + 顶部旋转柄
	const float R = controlRadius();
	ctx->DrawEllipse(D2D1::Ellipse({ cx, cy }, R, R),
		brushHandleBorder.Get(), win->dpi, selectionStroke());
	paintHandle(ctx, draggers[1]);   // 尾巴尖控制点
	paintHandle(ctx, draggers[3]);   // 顶部旋转柄
}

std::vector<float> ShapeNumber::editSnapshot() const
{
	return { cx, cy, r, angle, boxAngle, rect.left, rect.top, rect.right, rect.bottom, tailFactor_, badgeAngle };
}

void ShapeNumber::applyEditSnapshot(const std::vector<float>& s)
{
	if (s.size() < 9) return;
	cx = s[0];
	cy = s[1];
	r = s[2];
	angle = s[3];
	boxAngle = s[4];
	rect = D2D1::RectF(s[5], s[6], s[7], s[8]);
	tailFactor_ = (s.size() >= 10) ? std::clamp(s[9], 4.f / 3.f, 64.f) : 4.f / 3.f;
	badgeAngle = (s.size() >= 11) ? s[10] : 0.f;
	tailAnchored_ = false;
	makePath();
	makeTextLayout();
	updateDraggers();
}

bool ShapeNumber::translateBy(float dx, float dy)
{
	cx += dx;
	cy += dy;
	rect = offsetRect(rect, dx, dy);
	syncTailFromAnchor();
	makePath();
	updateDraggers();
	return true;
}

D2D1_RECT_F ShapeNumber::bounds() const
{
	// 数字/字母徽章是"圆 + 指向尾巴"，rect 只是内部的小方框；尾巴可能被拉长，按最远点放宽
	const float pad = r * (std::max)(1.6f, tailFactor_) + 2.f * win->dpi;
	return D2D1::RectF(cx - pad, cy - pad, cx + pad, cy + pad);
}

bool ShapeNumber::scaleBy(float pivotX, float pivotY, float angleRad, float sx, float sy)
{
	const auto nc = scalePoint(cx, cy, pivotX, pivotY, angleRad, sx, sy);
	const float dx = nc.x - cx, dy = nc.y - cy;
	cx = nc.x;
	cy = nc.y;
	rect = offsetRect(rect, dx, dy);
	r = std::max(4.f, r * (sx + sy) * 0.5f);
	if (isEmoji() || noTail_) syncRectFromCenter();
	tailAnchored_ = false; // 缩放：尾巴随半径等比缩放，不再钉住
	makePath();
	makeTextLayout();
	updateDraggers();
	return true;
}

bool ShapeNumber::rotateBy(float pivotX, float pivotY, float deltaRad)
{
	const auto nc = rotatePoint(cx, cy, pivotX, pivotY, deltaRad);
	const float dx = nc.x - cx, dy = nc.y - cy;
	cx = nc.x;
	cy = nc.y;
	rect = offsetRect(rect, dx, dy);
	boxAngle += deltaRad;                       // Emoji / 无尾巴：包围盒旋转
	badgeAngle += deltaRad;                     // 带尾巴：徽章（数字）一起转
	if (!tailExtended()) angle -= deltaRad * 180.f / 3.14159265f; // 尾巴缩回圈内才跟着转；钉住时只转徽章
	makePath();
	updateDraggers();
	return true;
}

void ShapeNumber::mouseDrag(const float x, const float y)
{
	if (boxMode()) {
		// Emoji /「停用尾巴」共用矩形系交互：角/边等比缩放、框内移动、角外一圈旋转
		if (hoverDraggerIndex == 9) {
			const auto c = boxCenter();
			float next = rotateStartAngle + (std::atan2(y - c.y, x - c.x) - rotateStartAtan);
			if (GetKeyState(VK_SHIFT) & 0x8000) next = snapRotation8(next);
			boxAngle = next;
			updateDraggers();
			return;
		}
		if (hoverDraggerIndex >= 0 && hoverDraggerIndex <= 7) {
			rect = dragScale(x, y, true);
			syncCenterFromRect();
			makePath();
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
			makePath();
			makeTextLayout();
		}
		updateDraggers();
		return;
	}
	if (hoverDraggerIndex == 0) {
			auto spanX{ x - pressX };
			auto spanY{ y - pressY };
			cx += spanX;
			cy += spanY;
			// 尾巴已拖出圈：尖钉在原位，靠角度/长度重接回新中心；
			// 尾巴还在圈内：作为本体的一部分整体平移
			syncTailFromAnchor();
			makePath();
			pressX = x;
			pressY = y;
		}
		else if (hoverDraggerIndex == 1) {
			// 尾巴控制点：方向跟着光标；长度只能往外拉——最短就是初始短尾巴，
			// 再往里拉不再变短（去掉尾巴用属性栏的「停用尾巴」）。
			// 长度按“相对按下点的径向位移”算，横着绕圈只改方向不改长度。
			angle = -atan2f(y - cy, x - cx) * 180.f / kPi;
			const float d = std::hypot(x - cx, y - cy);
			const float delta = (d - tailDragStartDist_) / (std::max)(1.f, r);
			tailFactor_ = std::clamp(tailDragStartLen_ + delta, 4.f / 3.f, 64.f);
			// 拖出控制圈就记下尾巴尖的世界坐标；缩回圈内自动失效
			updateTailAnchor();
			makePath();
		}
		else if (hoverDraggerIndex == 3) {
			// 顶部旋转柄：只转徽章本身（数字倾斜、柄绕圈）。
			// 尾巴已经拖出圈、钉在某个位置上时保持不动；缩回圈内才跟着一起转。
			float next = rotateStartAngle + (atan2f(y - cy, x - cx) - rotateStartAtan);
			if (GetKeyState(VK_SHIFT) & 0x8000) next = snapRotation8(next);
			badgeAngle = next;
			if (!tailExtended()) angle = tailStartAngle - (next - rotateStartAngle) * 180.f / kPi;
			makePath();
		}
		else if (hoverDraggerIndex == 2) {
			// 圆边任意位置拖动：半径 = 光标到圆心距离 ÷ 大圆倍率，圆边始终跟着光标
			const float d = std::hypot(x - cx, y - cy);
			r = (std::max)(8.f * win->dpi, d / 1.7f);
			r = win->toolSub->setShapeSliderVal(L"number", r);
			tailAnchored_ = false; // 改大小：尾巴长度按倍率缩放，不再钉住
			makePath();
			makeTextLayout();
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
		if (boxMode()) {
			syncRectFromCenter();
			hoverDraggerIndex = 8;
			// 记录的是相对形状的抓取偏移，而非按下点的绝对坐标：mouseDrag 按
			// rect.left = x - pressX 计算。若存绝对坐标，按下后移动 1px 即触发拖动，
			// 形状会被平移到画布左上角附近
			pressX = x - rect.left;
			pressY = y - rect.top;
			makePath();
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
	if (boxMode()) {
		if (hoverDraggerIndex == 9) {
			const auto c = boxCenter();
			rotateStartAngle = boxAngle;
			rotateStartAtan = std::atan2(y - c.y, x - c.x);
			return;
		}
		if (hoverDraggerIndex == 8) {
			pressX = x - rect.left;
			pressY = y - rect.top;
			return;
		}
		if (hoverDraggerIndex >= 0) beginScale(rect, boxAngle, hoverDraggerIndex, x, y);
		return;
	}
	if (hoverDraggerIndex == 3) {
		rotateStartAngle = badgeAngle;
		rotateStartAtan = atan2f(y - cy, x - cx);
		tailStartAngle = angle;
		return;
	}
	if (hoverDraggerIndex == 1) {
		// 尾巴控制点：记下按下时的长度和径向距离，拖拽按“相对位移”改长度
		tailDragStartLen_ = tailFactor_;
		tailDragStartDist_ = std::hypot(x - cx, y - cy);
		updateDraggers();
		return;
	}
	pressX = x;
	pressY = y;
}

void ShapeNumber::mouseUp(const float x, const float y)
{
	(void)x; (void)y;
	updateDraggers();
}

void ShapeNumber::updateDraggers()
{
	const float half = draggerSize * 0.5f;
	if (boxMode()) {
		ShapeBase::layoutBoxDraggers(draggers, selectionRect(rect), half);
		return;
	}
	draggers[0] = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
	// 尾巴控制点：默认贴在大圆虚线上；尾巴被拖出圈后，点跟着尾巴尖走
	const auto dot = transformPoint(D2D1::Point2F(tailDotRadius(), 0.f));
	draggers[1] = D2D1::RectF(dot.x - half, dot.y - half, dot.x + half, dot.y + half);
	draggers[2] = D2D1::RectF(mid.x - half, mid.y - half, mid.x + half, mid.y + half);
	// 旋转柄绕徽章转：位置跟着 badgeAngle 走，拖尾巴点不会带动它
	const auto handle = rotateHandlePos();
	draggers[3] = D2D1::RectF(handle.x - half, handle.y - half, handle.x + half, handle.y + half);
}

void ShapeNumber::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	if (boxMode()) {
		// 与矩形一致：角/边缩放优先；角外一圈旋转
		float lx, ly;
		toLocal(x, y, lx, ly);
		const float band = 8.f * win->dpi;
		const int edge = hitBoxEdgeOrCorner(lx, ly, rect, band);
		if (edge >= 0) {
			hoverDraggerIndex = edge;
		}
		else if (isSelected()) {
			const int rot = hitRotateRing(lx, ly, rect);
			if (rot >= 0) {
				hoverDraggerIndex = 9;
				rotateCorner = rot;
			}
			else if (lx >= rect.left && lx <= rect.right && ly >= rect.top && ly <= rect.bottom)
				hoverDraggerIndex = 8;
		}
		else if (lx >= rect.left && lx <= rect.right && ly >= rect.top && ly <= rect.bottom)
			hoverDraggerIndex = 8;
		// 未选中时点在徽章上按"移动"处理
		if (hoverDraggerIndex == -1 && hitErase(x, y)) hoverDraggerIndex = 8;
	}
	else {
		const float d = std::hypot(x - cx, y - cy);
		const float R = controlRadius();
		const float band = 8.f * win->dpi;
		const float dotHit = 9.f * win->dpi;
		// 命中优先级：顶部旋转柄 → 尾巴控制点 → 大圆边（整圈缩放）→ 圆内（整体移动）
		const auto handle = rotateHandlePos();
		if (std::hypot(x - handle.x, y - handle.y) <= dotHit)
			hoverDraggerIndex = 3;
		else {
			const auto dot = transformPoint(D2D1::Point2F(tailDotRadius(), 0.f));
			if (std::hypot(x - dot.x, y - dot.y) <= dotHit)
				hoverDraggerIndex = 1;
			else if (isSelected() && std::fabs(d - R) <= band)
				hoverDraggerIndex = 2;
			else if (isSelected() && d < R - band)
				hoverDraggerIndex = 0;
			else if (hitErase(x, y))
				hoverDraggerIndex = 0;
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
	}
	else {
		tailAnchored_ = false;
		makePath();
		makeTextLayout();
		if (noTail_) syncRectFromCenter();
	}
	updateDraggers();
	win->refresh();
}

void ShapeNumber::setCursor()
{
	if (applyUnselectedHoverCursor()) return;
	if (boxMode()) {
		// 与矩形一致的光标
		if (hoverDraggerIndex == 9) setRotateCursor(rotateCorner, boxAngle);
		else if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4) SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
		else if (hoverDraggerIndex == 1 || hoverDraggerIndex == 5) SetCursor(LoadCursor(nullptr, IDC_SIZENS));
		else if (hoverDraggerIndex == 2 || hoverDraggerIndex == 6) SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
		else if (hoverDraggerIndex == 3 || hoverDraggerIndex == 7) SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
		else if (hoverDraggerIndex == 8) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		else SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return;
	}
	if (hoverDraggerIndex == 1 || hoverDraggerIndex == 3) {
		SetCursor(LoadCursor(nullptr, IDC_HAND));   // 尾巴尖 / 顶部旋转柄
	}
	else if (hoverDraggerIndex == 2) {
		// 圆边缩放：按当前方位给对角箭头
		POINT p{};
		GetCursorPos(&p);
		ScreenToClient(win->hwnd, &p);
		const auto a = win->clientToAnnotPt((float)p.x, (float)p.y);
		const float ang = std::atan2(a.y - cy, a.x - cx);
		const bool nwse = std::cos(ang) * std::sin(ang) >= 0.f;
		SetCursor(LoadCursor(nullptr, nwse ? IDC_SIZENWSE : IDC_SIZENESW));
	}
	else if (hoverDraggerIndex == 0) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
	else {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
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

void ShapeNumber::updateTailAnchor()
{
	// 只有尾巴拖出了控制圈、并且没停用尾巴，才有值得钉住的尾巴尖
	if (noTail_ || !tailExtended()) {
		tailAnchored_ = false;
		return;
	}
	tailAnchor_ = transformPoint(D2D1::Point2F(r * tailFactor_, 0.f));
	tailAnchored_ = true;
}

// 移动本体时把尾巴重新接到钉住的尖上：角度和长度按“中心 → 尖”重算，尖保持不动。
// 本体挪到尖附近、尾巴已经收不回来时，钉子就此失效（长度不再强行改变，避免跳变）。
bool ShapeNumber::syncTailFromAnchor()
{
	if (!tailAnchored_) return false;
	const float dx = tailAnchor_.x - cx, dy = tailAnchor_.y - cy;
	const float d = std::hypot(dx, dy);
	const float f = d / (std::max)(1.f, r);
	if (f < 1.7f) {
		tailAnchored_ = false;
		return false;
	}
	angle = -atan2f(dy, dx) * 180.f / kPi;
	tailFactor_ = f;
	return true;
}

void ShapeNumber::makePath()
{
	if (isEmoji()) return;
	auto d2d = Ling::D2D::get();
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	mid = transformPoint(localPoint(180.f));
	if (noTail_) {
		// 停用尾巴：整圆徽章（两段半圆拼成闭合圆，无凸出的尾巴尖）
		const auto circleStart = transformPoint(localPoint(90.f));
		const auto circleOpposite = transformPoint(localPoint(270.f));
		tip = transformPoint(localPoint(0.f));
		sink->BeginFigure(circleStart, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddArc(D2D1::ArcSegment(circleOpposite, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
		sink->AddArc(D2D1::ArcSegment(circleStart, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		sink->Close();
		return;
	}
	auto start = transformPoint(localPoint(10.f));
	auto end = transformPoint(localPoint(350.f));
	tip = transformPoint(D2D1::Point2F(r * tailFactor_, 0.f));
	sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
	sink->AddArc(D2D1::ArcSegment(mid, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	sink->AddLine(tip);
	sink->AddLine(start);
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
}
