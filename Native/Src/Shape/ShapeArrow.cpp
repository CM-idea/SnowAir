#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeArrow.h"
#include "ShapeLink.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

ShapeArrow::ShapeArrow(AnnotHost* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	headStyle = toolSub->arrowHead;
	isDash = toolSub->isArrowDash;
	isRound = toolSub->isArrowRound;
	ensureStrokeStyle();
}

ShapeArrow::~ShapeArrow()
{
}

bool ShapeArrow::isBig() const
{
	return headStyle == ToolSub::ArrowBig;
}

float ShapeArrow::arrowSize() const
{
	// 箭头身宽缩放 2.5（传入 buildBigArrow* 的身宽）
	return std::max(1.f, strokeWidth) * 2.5f;
}

void ShapeArrow::resetFree()
{
	poly.freeStart = false;
	poly.freeEnd = false;
	poly.breakHoverKey = -1;
}

// —— 连线（箭头这一侧）：全部委托给 ShapeLink ——
bool ShapeArrow::hasLink() const { return ShapeLink::hasLink(poly); }
// 拖动中提示点**只跟着起点**（起点吸附在起笔那个形状上，沿边缘随鼠标方向滑）。
// 拖进目标形状时终点也会吸附，但那时点不能跟着终点跑到目标上 —— 目标不需要点提示
bool ShapeArrow::linkStartPoint(D2D1_POINT_2F& out) const
{
	if (!poly.linkStart.shape || poly.pts.size() < 2) return false;
	out = poly.pts.front();
	return true;
}
bool ShapeArrow::linkButtonPos(D2D1_POINT_2F& out) const { return ShapeLink::linkButtonPos(poly, out); }
bool ShapeArrow::hitLinkButton(const float x, const float y, const float dpi) const
{
	return ShapeLink::hitLinkButton(poly, x, y, dpi);
}
void ShapeArrow::syncLinks() { ShapeLink::sync(win, poly); }

bool ShapeArrow::bigHead() const
{
	return isBig();
}

// 折线长度中点（图像坐标）：文本工具悬停在线段中间时在那儿显示"可以打字"的空白
bool ShapeArrow::shaftMidPoint(float& mx, float& my) const
{
	const auto& pts = poly.pts;
	if (pts.size() < 2) return false;
	float total = 0.f;
	for (size_t i = 1; i < pts.size(); i++)
		total += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
	if (total < 2.f) return false;
	const float half = total * 0.5f;
	float walk = 0.f;
	for (size_t i = 1; i < pts.size(); i++) {
		const float seg = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
		if (walk + seg >= half) {
			const float t = seg > 0.f ? (half - walk) / seg : 0.f;
			mx = pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t;
			my = pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t;
			return true;
		}
		walk += seg;
	}
	mx = pts.back().x;
	my = pts.back().y;
	return true;
}

float ShapeArrow::shaftAngle() const
{
	const auto& pts = poly.pts;
	if (pts.size() < 2) return 0.f;
	return std::atan2(pts.back().y - pts.front().y, pts.back().x - pts.front().x);
}

bool ShapeArrow::toggleFreeEndpoint(float x, float y)
{
	if (!isSelected()) return false;
	if (!poly.toggleFreeAt(x, y, win->dpi)) return false;
	win->refresh();
	return true;
}

void ShapeArrow::ensureStrokeStyle()
{
	strokeStyle.Reset();
	headStrokeStyle.Reset();
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	// 轴线：虚线只作用于它；头部：永远实线。两者 cap/join 都跟「圆头」
	strokeStyle = ArrowParts::makeShaftStyle(factory, strokeWidth, isRound, isDash);
	headStrokeStyle = ArrowParts::makeHeadStyle(factory, isRound);
}

void ShapeArrow::makeBigPath()
{
	bigPath.Reset();
	if (poly.pts.size() < 2) return;
	const auto outline = ArrowPolyState::buildBigArrowPolygon(poly.pts, arrowSize());
	if (outline.size() < 3) return;
	auto d2d = Ling::D2D::get();
	d2d->d2dFactory->CreatePathGeometry(bigPath.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	bigPath->Open(sink.GetAddressOf());
	// 折角自交时 ALTERNATE 会挖出缺口，用 WINDING 填实
	sink->SetFillMode(D2D1_FILL_MODE_WINDING);
	sink->BeginFigure(outline[0], D2D1_FIGURE_BEGIN_FILLED);
	for (size_t i = 1; i < outline.size(); i++)
		sink->AddLine(outline[i]);
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
}

void ShapeArrow::ensureMinLength()
{
	if (poly.pts.size() < 2) return;
	float length = 0.f;
	for (size_t i = 1; i < poly.pts.size(); i++)
		length += std::hypot(poly.pts[i].x - poly.pts[i - 1].x, poly.pts[i].y - poly.pts[i - 1].y);
	const float minLen = isBig()
		? arrowSize() + 8.f * win->dpi
		: std::max(24.f * win->dpi, strokeWidth * 6.f);
	if (length >= minLen) return;
	auto& a = poly.pts[poly.pts.size() - 2];
	auto& b = poly.pts.back();
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float cur = std::hypot(dx, dy);
	float ux = 1.f, uy = 0.f;
	if (cur >= 1.f) { ux = dx / cur; uy = dy / cur; }
	const float need = minLen - (length - cur);
	b.x = a.x + ux * std::max(need, minLen * 0.25f);
	b.y = a.y + uy * std::max(need, minLen * 0.25f);
	rebuild();
}

void ShapeArrow::rebuild()
{
	if (isBig()) makeBigPath();
	else bigPath.Reset();
}

void ShapeArrow::drawPolyline(ID2D1DeviceContext* ctx)
{
	// 轴线（杆身）：虚线只作用于这条路径
	auto path = ArrowParts::shaftPath(Ling::D2D::get()->d2dFactory.Get(), poly.pts);
	if (path) ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, strokeStyle.Get());
}

void ShapeArrow::paint(ID2D1DeviceContext* ctx)
{
	if (poly.pts.size() < 2) return;
	// 连线端点先贴回形状边缘（形状可能刚被拖动/缩放，甚至已经被删掉）
	ShapeLink::sync(win, poly);
	if (isBig()) {
		if (!bigPath) makeBigPath();
		if (bigPath) ctx->FillGeometry(bigPath.Get(), brush.Get());
		return;
	}
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	drawPolyline(ctx);
	const auto start = poly.front();
	const auto startNext = poly.pts[1];
	const auto end = poly.back();
	const auto endPrev = poly.pts[poly.pts.size() - 2];
	// 箭头头部：圆头同样作用到它，且永远实线
	if (headStyle == ToolSub::ArrowAnnot) {
		auto bars = ArrowParts::annotBarsPath(factory, poly.pts, strokeWidth);
		if (bars) ctx->DrawGeometry(bars.Get(), brush.Get(), strokeWidth, headStrokeStyle.Get());
	}
	else if (headStyle == ToolSub::ArrowBoth) {
		if (auto head = ArrowParts::openHeadPath(factory, startNext, start, strokeWidth))
			ctx->DrawGeometry(head.Get(), brush.Get(), strokeWidth, headStrokeStyle.Get());
		if (auto head = ArrowParts::openHeadPath(factory, endPrev, end, strokeWidth))
			ctx->DrawGeometry(head.Get(), brush.Get(), strokeWidth, headStrokeStyle.Get());
	}
	else {
		if (auto head = ArrowParts::openHeadPath(factory, endPrev, end, strokeWidth))
			ctx->DrawGeometry(head.Get(), brush.Get(), strokeWidth, headStrokeStyle.Get());
	}
}

void ShapeArrow::paintDragger(ID2D1DeviceContext* ctx)
{
	const bool dragging = poly.drag != ArrowPolyState::DragKind::None;
	// 大箭头与普通箭头同一套控点（端点/断点/虚中点）
	paintArrowPolyHandles(ctx, poly, win->dpi,
		brushHandleFill.Get(), brushHandleBorder.Get(), brushHandleShadow.Get(),
		dragging, true);
}

int ShapeArrow::hoverFromHit(const ArrowHitInfo& h) const
{
	if (h.kind == ArrowHitInfo::Start) return 0;
	if (h.kind == ArrowHitInfo::End) return 1;
	if (h.kind == ArrowHitInfo::Break && h.insertAfter >= 0) return 100 + h.insertAfter;
	if (h.kind == ArrowHitInfo::Break) return 10 + std::max(0, h.pointIndex);
	if (h.kind == ArrowHitInfo::Move) return 8;
	return -1;
}

void ShapeArrow::mouseDrag(const float x, const float y)
{
	if (poly.drag == ArrowPolyState::DragKind::None) return;
	float mx = x, my = y;
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (shiftDown && poly.dragFree
		&& (poly.drag == ArrowPolyState::DragKind::Start || poly.drag == ArrowPolyState::DragKind::End)
		&& poly.dragOrigin.size() >= 2) {
		const auto pivot = (poly.drag == ArrowPolyState::DragKind::Start)
			? poly.dragOrigin.back() : poly.dragOrigin.front();
		constrainToEightDirections(pivot.x, pivot.y, x, y, mx, my);
	}
	poly.dragTo(mx, my);
	// 连线：端点落进形状就吸附；已连上的端点拖不断，只在边缘上滑
	ShapeLink::dragEndpoint(win, poly, creating, mx, my);
	rebuild();
}

void ShapeArrow::mouseDown(const float x, const float y)
{
	// 只有"新建箭头"这一步才允许把折线重置成一个点。已选中的箭头被再点一下时，
	// 若它的 hoverDraggerIndex 恰好是 -1（比如刚编辑过线上文字、没跑过 mouseMove），
	// 旧代码会 setTwo(点,点) —— 整条箭头被缩成一团（只剩三个控点挤在一起）
	if ((hoverDraggerIndex == -1 && !isSelected()) || poly.pts.size() < 2) {
		poly.setTwo(x, y, x, y);
		creating = true;
		// 落点落在矩形/椭圆里就从那儿起笔（端点吸附到形状边缘）
		ShapeLink::bindAtEndpoint(win, poly, true, x, y);
		lastHit.kind = ArrowHitInfo::End;
		lastHit.pointIndex = 1;
		lastHit.insertAfter = -1;
		hoverDraggerIndex = 1;
		poly.beginDrag(lastHit, x, y);
		poly.dragFree = true; // 落笔始终自由拖终点
		rebuild();
		return;
	}
	creating = false;
	poly.beginDrag(lastHit, x, y);
	rebuild();
}

void ShapeArrow::mouseUp(const float x, const float y)
{
	(void)x; (void)y;
	// 点击虚中点后几乎没拖：去掉共线折点，避免无意义拐点造成外形毛刺/缺口
	if (poly.drag == ArrowPolyState::DragKind::Break && poly.dragOrigin.size() == poly.pts.size()
		&& poly.dragPointIndex > 0 && poly.dragPointIndex + 1 < (int)poly.pts.size()) {
		const auto& a = poly.pts[poly.dragPointIndex - 1];
		const auto& b = poly.pts[poly.dragPointIndex];
		const auto& c = poly.pts[poly.dragPointIndex + 1];
		const float move = std::hypot(b.x - poly.dragOrigin[poly.dragPointIndex].x,
			b.y - poly.dragOrigin[poly.dragPointIndex].y);
		const float ab = std::hypot(b.x - a.x, b.y - a.y);
		const float bc = std::hypot(c.x - b.x, c.y - b.y);
		const float ac = std::hypot(c.x - a.x, c.y - a.y);
		if (move < 3.f * win->dpi && ab + bc <= ac + 1.5f * win->dpi)
			poly.pts.erase(poly.pts.begin() + poly.dragPointIndex);
	}
	poly.endDrag();
	creating = false;
	ensureMinLength();
	ShapeLink::sync(win, poly);   // 收尾：端点再贴一次边缘（ensureMinLength 可能动过它）
	rebuild();
}

bool ShapeArrow::hitBody(float x, float y)
{
	return poly.hit(x, y, win->dpi, false, false).kind != ArrowHitInfo::None;
}

void ShapeArrow::mouseMove(const float x, const float y)
{
	ShapeLink::sync(win, poly);   // 命中/悬停都按"贴回边缘之后"的端点算
	const bool dragging = poly.drag != ArrowPolyState::DragKind::None;
	// 大箭头与普通箭头同一套命中/断点逻辑
	const bool allowVirtual = isSelected();
	lastHit = poly.hit(x, y, win->dpi, dragging, allowVirtual);
	if (lastHit.kind == ArrowHitInfo::Break && lastHit.insertAfter >= 0)
		poly.breakHoverKey = lastHit.insertAfter + 1000;
	else if (lastHit.kind == ArrowHitInfo::Break)
		poly.breakHoverKey = lastHit.pointIndex;
	else
		poly.breakHoverKey = -1;
	hoverDraggerIndex = hoverFromHit(lastHit);
	// 禁止在 mouseMove 里同步 refresh：会与 WM_SETCURSOR 重入卡死
}

void ShapeArrow::mouseWheel(const float x, const float y, const short delta)
{
	(void)x; (void)y;
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"arrow", next);
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	ensureStrokeStyle();
	rebuild();
	win->refresh();
}

void ShapeArrow::setCursor()
{
	if (applyUnselectedHoverCursor()) return;
	// 控点（端点/断点）：点按手型；杆身：移动
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 1 || hoverDraggerIndex >= 10)
		SetCursor(LoadCursor(nullptr, IDC_HAND));
	else if (hoverDraggerIndex == 8)
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	else
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapeArrow::hitErase(const float x, const float y)
{
	return hitBody(x, y);
}

void ShapeArrow::constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY)
{
	float dx = mouseX - anchorX;
	float dy = mouseY - anchorY;
	float absX = fabsf(dx);
	float absY = fabsf(dy);
	if (absY <= absX * 0.41421356237f) {
		targetX = anchorX + dx;
		targetY = anchorY;
	}
	else if (absX <= absY * 0.41421356237f) {
		targetX = anchorX;
		targetY = anchorY + dy;
	}
	else {
		float span = absX > absY ? absX : absY;
		targetX = anchorX + (dx >= 0.f ? span : -span);
		targetY = anchorY + (dy >= 0.f ? span : -span);
	}
}
