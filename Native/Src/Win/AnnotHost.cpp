#include "pch.h"
#include "AnnotHost.h"
#include "../Tool/ToolCap.h"
#include "../Tool/ToolSub.h"
#include "../History.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeText.h"
#include "../Shape/ShapeLink.h"
#include "../Lang.h"
#include "../Shape/ShapeArrow.h"
#include "../Shape/ShapeLine.h"
#include "../Shape/ShapeRect.h"
#include "../Shape/ShapeEllipse.h"
#include "../Shape/ShapeNumber.h"
#include "../Shape/ArrowPoly.h"
#include "../Shape/FreehandStroke.h"
#include "../Shape/ShapePen.h"
#include "../Shape/ShapeMosaic.h"
#include "../Shape/ShapePatina.h"
#include "../Shape/ShapeWatermark.h"
#include "../Shape/ShapeHighlight.h"
#include "../Tool/SerialEmoji.h"
#include "../Util.h"

using namespace Microsoft::WRL;

AnnotHost::AnnotHost() = default;

AnnotHost::~AnnotHost() = default;

void AnnotHost::ensureHistory()
{
	if (!history) history = std::make_unique<History>(this);
	if (!toolSub) toolSub = std::make_unique<ToolSub>(this);
}

void AnnotHost::onHistoryChanged()
{
	// 撤销/重做/删除都会改变下层内容：上层滤镜重算一遍
	refreshFiltersAbove(nullptr);
	if (toolCap) toolCap->updateUndoEnabled();
}

// z 序里排在 changed 后面的形状才是"盖在它上面的"，逐个让滤镜重新采样。
// 注意：绝不能放进 paint 里做 —— createEffectBitmap 会 SetTarget，正在 BeginDraw 的帧会被打断。
void AnnotHost::refreshFiltersAbove(ShapeBase* changed)
{
	if (!history) return;
	// changed 已不在列表里（被删/被撤销）→ 找不到"它上面"是谁，干脆全部重算
	if (changed) {
		bool found = false;
		for (auto& s : history->shapes) {
			if (s.get() == changed) { found = true; break; }
		}
		if (!found) changed = nullptr;
	}
	bool past = (changed == nullptr);
	for (auto& s : history->shapes) {
		auto* cur = s.get();
		if (!past) {
			if (cur == changed) past = true;
			continue;
		}
		if (cur == changed || cur->isUndo) continue;
		if (auto* m = dynamic_cast<ShapeMosaic*>(cur)) m->rebuildEffect();
	}
}

void AnnotHost::refreshFiltersAboveThrottled(ShapeBase* changed)
{
	const ULONGLONG now = GetTickCount64();
	if (now - lastFilterRefreshMs < 66) return; // 拖动中约 15fps，抬手时会精确重算
	lastFilterRefreshMs = now;
	refreshFiltersAbove(changed);
}

bool AnnotHost::queryMainBarAnchor(MainBarAnchor& out) const
{
	if (!toolCap) return false;
	out.x = (float)toolCap->x;
	out.y = (float)toolCap->y;
	out.w = toolCap->w;
	out.h = toolCap->h;
	out.btnCenterX = toolCap->getBtnCenterX();
	out.dpi = toolCap->dpi;
	return true;
}

void AnnotHost::requestFadeTick()
{
	setTimer(33, 211);
}

std::wstring AnnotHost::getCurToolId() const
{
	if (!forcedToolId.empty()) return forcedToolId;
	if (!toolCap || toolCap->curId.empty()) return L"";
	return toolCap->resolveToolId(toolCap->curId);
}

void AnnotHost::forwardKey(UINT key)
{
	annotKey(key);
}

POINT AnnotHost::toImgPos(const POINT& pos) const
{
	if (scale == 1.f) return pos;
	return POINT{ static_cast<LONG>(std::lround(pos.x / scale)), static_cast<LONG>(std::lround(pos.y / scale)) };
}

D2D1_POINT_2F AnnotHost::annotToClientPt(float ax, float ay) const
{
	return D2D1::Point2F(ax * scale, ay * scale);
}

D2D1_POINT_2F AnnotHost::clientToAnnotPt(float cx, float cy) const
{
	if (scale == 1.f) return D2D1::Point2F(cx, cy);
	return D2D1::Point2F(cx / scale, cy / scale);
}

ShapeBase* AnnotHost::hitShapeAt(float x, float y)
{
	if (!history) return nullptr;
	// 挂在箭头/线段上的文字是"整体的一部分"：命中它要返回它所挂的那条线，
	// 这样选中的永远是"箭头+文字"这个整体，文字不会被单独选中
	auto groupOf = [this](ShapeBase* s) -> ShapeBase* {
		auto* t = dynamic_cast<ShapeText*>(s);
		if (!t || !t->attachTo) return s;
		for (auto& o : history->shapes) {
			if (o.get() == t->attachTo && !o->isUndo) return t->attachTo;
		}
		return s;
	};
	if (shapeHover && !shapeHover->isUndo && !shapeHover->isViewportFilter()) {
		shapeHover->mouseMove(x, y);
		if (shapeHover->hoverDraggerIndex >= 0) return groupOf(shapeHover);
	}
	for (int i = (int)history->shapes.size() - 1; i >= 0; i--) {
		auto* cur = history->shapes[i].get();
		if (cur->isUndo || cur == shapeHover || cur->isViewportFilter() || cur->isEphemeral()) continue;
		cur->mouseMove(x, y);
		if (cur->hoverDraggerIndex >= 0) return groupOf(cur);
	}
	return nullptr;
}

void AnnotHost::clearAnnotSelection()
{
	if (!shapeHover) return;
	if (auto* a = dynamic_cast<ShapeArrow*>(shapeHover)) a->resetFree();
	else if (auto* l = dynamic_cast<ShapeLine*>(shapeHover)) l->resetFree();
	shapeHover = nullptr;
	refresh();
}

// 清空标注列表的唯一入口：裸指针先归零再释放 shape，避免留下悬垂的 shapeHover。
// （工具条按钮的 onClick 里会读 shapeHover，一旦悬垂就是「点一下工具栏就闪退」）
void AnnotHost::clearAllShapes()
{
	if (editingText) editingText->finishEdit(); // 内部会置空 editingText / shapeHover
	annotMouseDown = false;
	annotDragIndex = -1;
	hasDragged = false;
	newShape = nullptr;
	shapeHover = nullptr;
	linkBtn = nullptr;        // 连线叉指向的 shape 也要先归零，别留悬垂指针
	linkHoverTarget = nullptr;
	linkPressTarget = nullptr;
	if (history) {
		history->shapes.clear();
		onHistoryChanged();
	}
	refresh();
}

bool AnnotHost::hasSelectedAnnot() const
{
	return shapeHover && !shapeHover->isUndo && !shapeHover->isViewportFilter();
}

std::wstring AnnotHost::toolIdOfShape(ShapeBase* s)
{
	if (!s || s->isUndo || s->isViewportFilter()) return {};
	if (dynamic_cast<ShapeRect*>(s) || dynamic_cast<ShapeEllipse*>(s)) return L"rect";
	if (dynamic_cast<ShapeArrow*>(s) || dynamic_cast<ShapeLine*>(s)) return L"arrow";
	if (dynamic_cast<ShapePen*>(s)) return L"pen";
	if (dynamic_cast<ShapeText*>(s)) return L"text";
	if (dynamic_cast<ShapeNumber*>(s)) return L"number";
	if (dynamic_cast<ShapeMosaic*>(s)) return L"mosaic";
	if (dynamic_cast<ShapeHighlight*>(s)) return L"highlight";
	if (dynamic_cast<ShapePatina*>(s)) return L"patina";
	if (dynamic_cast<ShapeWatermark*>(s)) return L"watermark";
	return {};
}

void AnnotHost::presentSelectedAnnotStyle()
{
	if (!hasSelectedAnnot() || !toolSub) return;
	const auto sid = toolIdOfShape(shapeHover);
	if (sid.empty()) return;
	const auto cur = getCurToolId();
	if (cur.empty() || cur != sid) return;
	const float sz = shapeHover->stylePrimarySize();
	if (sz > 0.f) toolSub->setShapeSliderVal(sid, sz);
}

bool AnnotHost::shouldPaintAnnotHandles() const
{
	if (!shapeHover || shapeHover->isUndo || shapeHover->isViewportFilter()) return false;
	if (shapeHover == newShape) return false; // 落笔中
	if (editingText) return false;            // 文字编辑中
	return true;
}

void AnnotHost::lockAnnotDragFromHover()
{
	annotDragIndex = (shapeHover && shapeHover->hoverDraggerIndex >= 0)
		? shapeHover->hoverDraggerIndex : -1;
}

void AnnotHost::applyLockedAnnotDrag()
{
	if (shapeHover && annotDragIndex >= 0)
		shapeHover->hoverDraggerIndex = annotDragIndex;
}

void AnnotHost::paintShapes(ID2D1DeviceContext* ctx)
{
	paintShapes(ctx, true);
}

void AnnotHost::paintShapes(ID2D1DeviceContext* ctx, bool withChrome)
{
	paintShapes(ctx, withChrome, false);
}

// skipEphemeral：离屏导出（截图/长图）时丢掉渐隐画笔这类临时笔迹，
// 只画已提交的 ops（laser 是纯屏上特效）
void AnnotHost::paintShapes(ID2D1DeviceContext* ctx, bool withChrome, bool skipEphemeral)
{
	if (!history) return;
	std::vector<ShapeHighlight*> highlights;
	for (auto& shape : history->shapes) {
		if (shape->isUndo) continue;
		if (skipEphemeral && shape->isEphemeral()) continue;
		if (auto* h = dynamic_cast<ShapeHighlight*>(shape.get())) {
			highlights.push_back(h);
			continue;
		}
		// 「箭头上的文字」：先按所属箭头/线段当前的中点摆一遍 —— 拖箭头、拖端点、
		// 撤销/擦除之后，文字都跟着走或自动脱钩（对象可能已经不在列表里，先验证）。
		// 方向不跟：线上文字一律横排（见 ShapeText::setAttachPose）
		if (auto* t = dynamic_cast<ShapeText*>(shape.get())) {
			auto* host = t->attachTo;
			bool alive = false;
			if (host) {
				for (auto& s : history->shapes) {
					if (s.get() == host && !s->isUndo) { alive = true; break; }
				}
			}
			if (!alive) {
				t->attachTo = nullptr;
			}
			else {
				float mx = 0.f, my = 0.f;
				bool ok = false;
				if (auto* a = dynamic_cast<ShapeArrow*>(host)) { ok = a->shaftMidPoint(mx, my); }
				else if (auto* l = dynamic_cast<ShapeLine*>(host)) { ok = l->shaftMidPoint(mx, my); }
				if (ok) t->setAttachPose(mx, my);
			}
		}
		shape->paint(ctx);
	}
	ShapeHighlight::paintMerged(ctx, this, highlights);
	if (!withChrome) return;
	// 共用：仅「已选中且非落笔、非文字编辑」时画控点
	if (shouldPaintAnnotHandles())
		shapeHover->paintDragger(ctx);
	paintSerialCursor(ctx);
	paintTextMidHint(ctx);
	paintLinkHints(ctx);
}

// 文本工具悬停在线段/箭头中间：在那儿开一块空白（底图抠回来），示意"可以在这儿打字"。
// 大箭头不参与（它是整体填充形状，中间没"线段"可言）。
void AnnotHost::updateTextMidHint(const POINT& imgPos, const std::wstring& tool)
{
	D2D1_RECT_F next{};
	bool on = false;
	bool hasLabel = false;
	if (tool == L"text" && history) {
		// 不需要先选中、也不依赖"悬停命中"（未选中的箭头命不中时，整条链路就断了）：
		// 自己扫一遍标注，取中点离光标最近、且在热区内的那条箭头/线段
		ShapeBase* host = nullptr;
		float bestMx = 0.f, bestMy = 0.f, bestDist = 1e9f;
		for (auto& s : history->shapes) {
			if (!s || s->isUndo || s.get() == newShape) continue;
			float mx = 0.f, my = 0.f;
			bool ok = false, big = false;
			if (auto* a = dynamic_cast<ShapeArrow*>(s.get())) { ok = a->shaftMidPoint(mx, my); big = a->bigHead(); }
			else if (auto* l = dynamic_cast<ShapeLine*>(s.get())) { ok = l->shaftMidPoint(mx, my); }
			if (!ok || big) continue;
			const float tol = 18.f * dpi;          // 光标离中点多近才算"停在中间"
			const float dx = std::abs((float)imgPos.x - mx), dy = std::abs((float)imgPos.y - my);
			if (dx <= tol && dy <= tol && dx + dy < bestDist) {
				bestDist = dx + dy;
				host = s.get();
				bestMx = mx;
				bestMy = my;
			}
		}
		if (host) {
			float fh = toolSub ? toolSub->getSliderVal() : 20.f;
			if (fh < 10.f) fh = 10.f;
			// 和"空标签"用同一套尺寸：一个字宽 + 两侧间距（之前是 2.2 个字宽，太胖）。
			// pad 要和 ShapeText::borderPadding（6*dpi）一致，否则停悬的洞和落笔后的洞大小不一
			const float pad = 6.f * dpi;
			const float h = fh * 1.35f + pad * 2.f;
			const float wBox = fh + pad * 2.f;
			next = D2D1::RectF(bestMx - wBox * 0.5f, bestMy - h * 0.5f, bestMx + wBox * 0.5f, bestMy + h * 0.5f);
			on = true;
			// 线上已经有字：不用再挖空（用户要求），但仍然保持"这儿能打字"的提示状态 → 工字型
			for (auto& s : history->shapes) {
				auto* t = dynamic_cast<ShapeText*>(s.get());
				if (t && t->attachTo == host) { hasLabel = true; break; }
			}
		}
	}
	const bool same = (on == textMidHintActive) && (hasLabel == textMidHasLabel)
		&& std::abs(textMidHint.left - next.left) < 0.5f && std::abs(textMidHint.top - next.top) < 0.5f
		&& std::abs(textMidHint.right - next.right) < 0.5f && std::abs(textMidHint.bottom - next.bottom) < 0.5f;
	if (same) return;
	textMidHintActive = on;
	textMidHasLabel = hasLabel;
	textMidHint = next;
	refresh();
}

void AnnotHost::paintTextMidHint(ID2D1DeviceContext* ctx)
{
	if (!textMidHintActive || textMidHasLabel || !ctx || !screenImg) return;
	// 就是"空白"本身：底图原地抠回来 → 底下的线在这一段等于不存在
	ctx->PushAxisAlignedClip(textMidHint, D2D1_ANTIALIAS_MODE_ALIASED);
	ctx->DrawBitmap(screenImg.Get(), D2D1::RectF(0.f, 0.f, w, h));
	ctx->PopAxisAlignedClip();
}

// 连线的提示（都只在箭头/线段工具或空工具=选择态下出现）：
//   · 悬停形状时：在"朝鼠标方向"的边缘点画一个和控点同款的小圆点 + 一句"拖动可连线"
//     —— 这就是**起点**的提示（这个形状可以按住往外拖出连线）；
//   · 拖动连线中：点**一直待在起点**上（起点贴着边缘随鼠标方向滑，点跟着滑）。
//     拖进目标形状时终点会吸附，但点不会跟到目标上去 —— 用户要的：目标不需要点提示；
//   · 线中点上的**叉**：点它直接删掉这条箭头/线段，只在没拖动时出现。
// 文本工具停在线中间是"这儿能打字"（updateTextMidHint），两个热区都在中点，
// 不能让它们抢同一处，所以这些提示都不在文本工具下出现
void AnnotHost::updateLinkHints(const POINT& imgPos, const std::wstring& tool)
{
	const bool toolOk = (tool == L"arrow" || tool.empty());
	const D2D1_POINT_2F cursor{ (float)imgPos.x, (float)imgPos.y };

	// ① 点：悬停时=该形状朝鼠标方向的边缘点（起点提示）；拖动中=这条线的起点
	ShapeBase* target = nullptr;
	D2D1_POINT_2F dot{ 0.f, 0.f };
	bool showDot = false;
	if (annotMouseDown) {
		if (newShape && newShape->linkStartPoint(dot)) {
			target = newShape;
			showDot = true;
		}
	}
	else if (toolOk) {
		if (ShapeBase* t = ShapeLink::pickTarget(this, cursor.x, cursor.y)) {
			if (ShapeLink::edgePointToward(t, cursor.x, cursor.y, dot)) {
				target = t;
				showDot = true;
			}
		}
	}

	// ② 叉：找离光标最近、且在热区内的连线中点（拖动中不找）
	ShapeBase* btn = nullptr;
	D2D1_POINT_2F btnPos{ 0.f, 0.f };
	if (!annotMouseDown && toolOk && history) {
		float bestDist = 1e9f;
		for (auto& s : history->shapes) {
			if (!s || s->isUndo || s->isEphemeral()) continue;
			s->syncLinks();                 // 位置按最新的形状几何算（形状没了会顺手脱钩）
			if (!s->hasLink()) continue;
			D2D1_POINT_2F pos{};
			if (!s->linkButtonPos(pos)) continue;
			const float d = std::hypot(cursor.x - pos.x, cursor.y - pos.y);
			if (d <= 11.f * dpi && d < bestDist) {
				bestDist = d;
				btn = s.get();
				btnPos = pos;
			}
		}
	}

	const bool same = (btn == linkBtn) && (target == linkHoverTarget) && (showDot == linkShowDot)
		&& (!btn || (std::abs(linkBtnPos.x - btnPos.x) < 0.5f && std::abs(linkBtnPos.y - btnPos.y) < 0.5f))
		&& (!showDot || (std::abs(linkAnchorDot.x - dot.x) < 0.5f && std::abs(linkAnchorDot.y - dot.y) < 0.5f));
	if (same) return;
	linkBtn = btn;
	linkBtnPos = btnPos;
	linkHoverTarget = target;
	linkShowDot = showDot;
	linkAnchorDot = dot;
	refresh();
}

void AnnotHost::paintLinkHints(ID2D1DeviceContext* ctx)
{
	if (!ctx) return;
	auto d2d = Ling::D2D::get();
	if (!brushLinkBtnFill) {
		d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushLinkBtnFill.GetAddressOf());
		d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x34C759), brushLinkBtnBorder.GetAddressOf());
		d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 64.f / 255.f), brushLinkBtnShadow.GetAddressOf());
	}

	// 起点小圆点：**和四周控点一模一样**（同一套半径/配色，中间不加任何符号）。
	// 悬停时 = 该形状上"朝鼠标方向"的边缘点（起点提示）；拖动中 = 这条线的起点（一直跟着它）。
	// 目标形状上不会画 —— 拖进目标时 linkShowDot 跟的是起点那头
	if (linkShowDot && history) {
		bool targetAlive = false;
		for (auto& s : history->shapes) {
			if (s.get() == linkHoverTarget && !s->isUndo) { targetAlive = true; break; }
		}
		if (!targetAlive) linkHoverTarget = nullptr;
	}
	if (linkShowDot && linkHoverTarget) {
		// 半径/描边和 ShapeBase::paintHandle 一致（阴影 5 / 绿底 4 / 白圈 1.5）
		const float rOuter = 5.f * dpi;
		const float rInner = 4.f * dpi;
		ctx->FillEllipse(D2D1::Ellipse(linkAnchorDot, rOuter, rOuter), brushLinkBtnShadow.Get());
		ctx->FillEllipse(D2D1::Ellipse(linkAnchorDot, rInner, rInner), brushLinkBtnBorder.Get());
		ctx->DrawEllipse(D2D1::Ellipse(linkAnchorDot, rInner, rInner), brushLinkBtnFill.Get(), 1.5f * dpi);
		// 文字提示挂在起点旁边（"拖动可连线"）；拖动中收掉 —— 线本身已经把结果画出来了
		if (!annotMouseDown) paintLinkHintText(ctx, linkAnchorDot);
	}

	// 叉：先说清它还在不在（删除/撤销之后指针可能已经失效）
	if (!linkBtn || !history) return;
	bool alive = false;
	for (auto& s : history->shapes) {
		if (s.get() == linkBtn && !s->isUndo) { alive = true; break; }
	}
	if (!alive) { linkBtn = nullptr; return; }
	D2D1_POINT_2F pos{};
	if (!linkBtn->linkButtonPos(pos)) { linkBtn = nullptr; return; }
	const float r = 9.f * dpi;
	const float arm = r * 0.45f;
	ctx->FillEllipse(D2D1::Ellipse(pos, r, r), brushLinkBtnFill.Get());
	ctx->DrawEllipse(D2D1::Ellipse(pos, r, r), brushLinkBtnBorder.Get(), 1.5f * dpi);
	ctx->DrawLine(D2D1::Point2F(pos.x - arm, pos.y - arm), D2D1::Point2F(pos.x + arm, pos.y + arm),
		brushLinkBtnBorder.Get(), 1.6f * dpi);
	ctx->DrawLine(D2D1::Point2F(pos.x + arm, pos.y - arm), D2D1::Point2F(pos.x - arm, pos.y + arm),
		brushLinkBtnBorder.Get(), 1.6f * dpi);
}

// 起点旁边那句提示（深色小胶囊 + 白字），贴着"＋"柄往右下摆、夹在窗口里。
// 文案走 Lang（cap.dragToLink），和别处的提示一致
void AnnotHost::paintLinkHintText(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& anchor)
{
	if (!ctx) return;
	auto d2d = Ling::D2D::get();
	auto layout = d2d->makeTextLayout(Lang::get(L"cap.dragToLink"), 12.f * dpi);
	if (!layout) return;
	layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
	DWRITE_TEXT_METRICS tm{};
	layout->GetMetrics(&tm);
	const float padX = 8.f * dpi, padY = 5.f * dpi;
	float left = anchor.x + 10.f * dpi;
	float top = anchor.y + 10.f * dpi;
	const float boxW = tm.width + padX * 2.f;
	const float boxH = tm.height + padY * 2.f;
	if (left + boxW > w) left = anchor.x - 10.f * dpi - boxW;
	if (top + boxH > h) top = anchor.y - 10.f * dpi - boxH;
	left = std::clamp(left, 0.f, std::max(0.f, w - boxW));
	top = std::clamp(top, 0.f, std::max(0.f, h - boxH));

	if (!brushLinkHintBg) {
		d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.72f), brushLinkHintBg.GetAddressOf());
		d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushLinkHintFg.GetAddressOf());
	}
	const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
		D2D1::RectF(left, top, left + boxW, top + boxH), 6.f * dpi, 6.f * dpi);
	ctx->FillRoundedRectangle(pill, brushLinkHintBg.Get());
	ctx->DrawTextLayout({ left + padX, top + padY }, layout.Get(), brushLinkHintFg.Get(),
		D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void AnnotHost::paintSerialCursor(ID2D1DeviceContext* ctx)
{
	if (!numberCursorFollow || !toolSub || !ctx) return;
	if (annotMouseDown) return; // 落笔中由 shape 自身绘制
	if (getCurToolId() != L"number") return;

	POINT screen{};
	GetCursorPos(&screen);
	POINT client = screen;
	ScreenToClient(hwnd, &client);
	if (client.x < 0 || client.y < 0 || client.x >= (int)w || client.y >= (int)h) return;

	const auto img = toImgPos(client);
	const float mx = (float)img.x;
	const float my = (float)img.y;
	float size = toolSub->getSliderVal();
	if (size < 10.f) size = 10.f;

	if (toolSub->numberStyle == ToolSub::NumberEmoji) {
		// 与 ShapeNumber::paint 一致：字号=r，布局盒=r*1.2
		const float draw = std::max(10.f, size);
		const float box = draw * 1.2f;
		auto emoji = toolSub->serialEmoji.empty() ? SerialEmoji::Default : toolSub->serialEmoji;
		ComPtr<IDWriteTextLayout> layout;
		auto d2d = Ling::D2D::get();
		d2d->dwriteFactory->CreateTextLayout(emoji.data(), (UINT32)emoji.size(),
			d2d->baseTextFormat.Get(), box * 2.f, box * 2.f, layout.GetAddressOf());
		if (!layout) return;
		layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		layout->SetFontFamilyName(L"Segoe UI Emoji", { 0, (UINT32)emoji.size() });
		layout->SetFontSize(draw, { 0, (UINT32)emoji.size() });
		ComPtr<ID2D1SolidColorBrush> brush;
		d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.9f), brush.GetAddressOf());
		ctx->DrawTextLayout({ mx - box, my - box }, layout.Get(), brush.Get(),
			D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
		return;
	}

	// 数字/字母：半透明空心圆；半径=滑条值，与 ShapeNumber::r 一致
	auto color = toolSub->getSelectedColor();
	ComPtr<ID2D1SolidColorBrush> fill;
	ComPtr<ID2D1SolidColorBrush> border;
	auto fillC = color; fillC.a = 0.28f;
	auto borderC = color; borderC.a = 0.55f;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(fillC, fill.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(borderC, border.GetAddressOf());
	const float rad = size;
	const D2D1_ELLIPSE el{ { mx, my }, rad, rad };
	ctx->FillEllipse(el, fill.Get());
	ctx->DrawEllipse(el, border.Get(), 1.5f);
}

Ling::TextBox* AnnotHost::getTextBox()
{
	if (textBox) return textBox;
	textBox = body->makeChild<Ling::TextBox>();
	textBox->setPositionType(Ling::Position::Absolute);
	textBox->setAutoSize(true);
	textBox->hide();
	return textBox;
}

void AnnotHost::setEditingText(ShapeText* shape)
{
	editingText = shape;
}

void AnnotHost::syncViewportFilters()
{
	ensureHistory();
	history->syncViewportFilters(getCurToolId());
}

void AnnotHost::onToolStyleChanged()
{
	if (editingText) editingText->applyStyle();
	else if (auto* t = dynamic_cast<ShapeText*>(shapeHover)) t->applyStyle();
	if (history) {
		for (auto& s : history->shapes) {
			if (s->isUndo) continue;
			if (auto* m = dynamic_cast<ShapeMosaic*>(s.get())) {
				if (s.get() == shapeHover) m->applyStyle();
			}
			else if (auto* p = dynamic_cast<ShapePatina*>(s.get())) p->applyStyle();
			else if (auto* w = dynamic_cast<ShapeWatermark*>(s.get())) w->applyStyle();
			else if (auto* h = dynamic_cast<ShapeHighlight*>(s.get())) {
				if (s.get() == shapeHover) h->applyStyle();
			}
		}
	}
	// 改属性的若是下层标注，上层的模糊/马赛克也要重采样
	if (shapeHover) refreshFiltersAbove(shapeHover);
	refresh();
}

void AnnotHost::applyEmptyToolCursor()
{
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void AnnotHost::applyToolDrawCursor()
{
	const auto tool = getCurToolId();
	if (tool.empty()) {
		applyEmptyToolCursor();
		return;
	}
	if (tool == L"text") SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	else if (tool == L"eraser") SetCursor(LoadCursor(nullptr, IDC_HAND)); // 对齐 QT PointingHand
	else if (tool == L"watermark" || tool == L"patina") SetCursor(LoadCursor(nullptr, IDC_ARROW));
	else if (tool == L"number") {
		numberCursorFollow = true;
		SetCursor(nullptr); // 对齐 QT BlankCursor：跟手圆/表情即光标
	}
	else SetCursor(LoadCursor(nullptr, IDC_CROSS));
}

ShapeBase* AnnotHost::probeShapeUnder(float x, float y, ShapeBase* preferFirst)
{
	if (!history) return nullptr;
	if (preferFirst && !preferFirst->isUndo && !preferFirst->isViewportFilter() && !preferFirst->isEphemeral()) {
		preferFirst->mouseMove(x, y);
		if (preferFirst->hoverDraggerIndex >= 0) return preferFirst;
	}
	for (int i = (int)history->shapes.size() - 1; i >= 0; i--) {
		auto* cur = history->shapes[i].get();
		if (cur->isUndo || cur->isViewportFilter() || cur->isEphemeral() || cur == preferFirst) continue;
		cur->mouseMove(x, y);
		if (cur->hoverDraggerIndex >= 0) return cur;
	}
	return nullptr;
}

void AnnotHost::applyAnnotCursor()
{
	numberCursorFollow = false;
	// 文本工具停在线段中间那块"空白"里：一律工字型（比"悬停到已有图形"的移动/缩放光标优先，
	// 不然在箭头上悬停时会被箭头自己的光标盖掉，看着就不像能打字）
	if (textMidHintActive && !annotMouseDown) {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
		return;
	}
	if (editingText) {
		bool handled{ false };
		onCursor(&handled);
		if (handled) return;
	}
	// 正在落笔：保持工具光标
	if (annotMouseDown && newShape) {
		applyToolDrawCursor();
		return;
	}
	// 拖动中锁定按下时的操作模式，禁止重探命中（否则旋转时指针离开柄会变成移动）
	if (annotMouseDown && shapeHover && shapeHover != newShape) {
		shapeHover->setCursor();
		return;
	}

	POINT pos{};
	GetCursorPos(&pos);
	ScreenToClient(hwnd, &pos);
	const auto img = toImgPos(pos);
	const float ix = (float)img.x, iy = (float)img.y;

	// 已选中：边缘缩放 / 区内移动
	if (shapeHover && !shapeHover->isUndo && shapeHover != newShape
		&& !shapeHover->isViewportFilter()) {
		shapeHover->mouseMove(ix, iy);
		if (shapeHover->hoverDraggerIndex >= 0) {
			shapeHover->setCursor();
			return;
		}
	}

	// 未选中标注上：手型，提示可点选（录屏工具启用时跳过，只显示画笔光标）。
	// 空工具时也**保留手型** —— 用户确认过：悬停在标注上给手型是对的（提示"能点它"）；
	// "没选工具 = 普通箭头"说的是**选框里的空白处**（那条走 applyEmptyToolCursor）
	if (!annotSkipHitSelect()) {
		if (auto* under = probeShapeUnder(ix, iy, nullptr)) {
			if (under != shapeHover) {
				under->setCursor();
				return;
			}
		}
	}

	applyToolDrawCursor();
}

void AnnotHost::setSizePreviewHold(bool hold)
{
	if (sizePreviewHold == hold) return;
	sizePreviewHold = hold;
	refresh();
}

void AnnotHost::pulseSizePreview()
{
	sizePreviewPulseUntil = GetTickCount64() + 700;
	setTimer(700, 210);
	refresh();
}

void AnnotHost::annotTimer(UINT id)
{
	if (id == 210) {
		killTimer(210);
		sizePreviewPulseUntil = 0;
		refresh();
		return;
	}
	if (id == 211) {
		// 渐隐画笔：按点寿命重建轮廓，整段过寿命后移除；绘制中也持续 tick
		if (!history) {
			killTimer(211);
			return;
		}
		bool still = false;
		std::vector<ShapeBase*> gone;
		for (auto& s : history->shapes) {
			auto* pen = dynamic_cast<ShapePen*>(s.get());
			if (!pen || !pen->needsFadeTick()) continue;
			if (pen->tickFade()) gone.push_back(pen);
			else still = true;
		}
		for (auto* p : gone) history->removeShape(p);
		refresh();
		if (still) setTimer(33, 211);
		else killTimer(211);
		return;
	}
	if (id != 100) return;
	if (!shapeHover) {
		refresh();
		killTimer(100);
	}
}

namespace {
	constexpr float kPiPrev = 3.14159265358979323846f;
}

void AnnotHost::paintSizePreview(ID2D1DeviceContext* ctx)
{
	if (!toolSub || !ctx) return;
	if (!sizePreviewHold && GetTickCount64() >= sizePreviewPulseUntil) return;
	auto id = getCurToolId();
	if (id.empty() || id == L"eraser" || id == L"mosaic" || id == L"patina"
		|| id == L"watermark" || id == L"highlight" || id == L"number") return;

	float cx = 0.f, cy = 0.f, half = 40.f;
	if (!sizePreviewOrigin(cx, cy, half)) return;
	const float x1 = cx - half;
	const float x2 = cx + half;

	// 整段预览用层透明度，避免杆身与箭头交接处叠色发暗（对齐 QT setOpacity）
	ComPtr<ID2D1Layer> layer;
	ctx->CreateLayer(nullptr, layer.GetAddressOf());
	if (layer) {
		ctx->PushLayer(D2D1::LayerParameters(
			D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
			D2D1::IdentityMatrix(), 0.55f), layer.Get());
	}
	auto color = toolSub->getSelectedColor();
	color.a = 1.f;
	ComPtr<ID2D1SolidColorBrush> brush;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());

	auto pop = [&] {
		if (layer) ctx->PopLayer();
	};

	if (id == L"text") {
		const float px = std::max(8.f, toolSub->getSliderVal());
		const std::wstring sample = L"SnowAir 轻雪";
		// 用 makeTextLayout（左对齐定宽），避免 CENTER+超宽 layout 把字画飞到屏外
		auto layout = Ling::D2D::get()->makeTextLayout(sample, px);
		if (!layout) { pop(); return; }
		layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		if (toolSub->isTextBold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, { 0, (UINT32)sample.size() });
		if (toolSub->isTextItalic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, { 0, (UINT32)sample.size() });
		DWRITE_TEXT_METRICS tm{};
		layout->GetMetrics(&tm);
		ctx->DrawTextLayout(
			{ cx - tm.width * 0.5f, cy - tm.height * 0.5f },
			layout.Get(), brush.Get());
		pop();
		return;
	}

	if (id == L"pen") {
		const float stroke = toolSub->getSliderVal();
		if (toolSub->isPenFade) {
			// 渐隐画笔：预览是一条半透明线段示意笔迹大小
			auto style = ArrowParts::makeShaftStyle(
				Ling::D2D::get()->d2dFactory.Get(), stroke, true, false);
			ctx->DrawLine({ x1, cy }, { x2, cy }, brush.Get(), stroke, style.Get());
			pop();
			return;
		}
		const float amp = std::min(22.f, half * 0.28f);
		std::vector<D2D1_POINT_2F> pts;
		std::vector<int64_t> times;
		static const float kT[] = { 0, 0.04f, 0.09f, 0.16f, 0.28f, 0.45f, 0.62f, 0.76f, 0.86f, 0.93f, 0.97f, 1.f };
		for (float t : kT) {
			pts.push_back({
				x1 + (x2 - x1) * t,
				cy + std::sin(t * kPiPrev * 2.f) * amp
			});
			// 假时间：两端慢、中间快，软笔预览能看出粗细变化
			const float speed = 0.25f + 1.2f * std::sin(t * kPiPrev);
			times.push_back(times.empty() ? 0 : times.back() + (int64_t)std::max(4.f, 18.f / speed));
		}
		if (!toolSub->isPenDash) {
			auto geo = FreehandStroke::outline(
				Ling::D2D::get()->d2dFactory.Get(), pts, stroke, toolSub->isPenSoft, times);
			if (geo) ctx->FillGeometry(geo.Get(), brush.Get());
		}
		else {
			ComPtr<ID2D1PathGeometry> path;
			Ling::D2D::get()->d2dFactory->CreatePathGeometry(path.GetAddressOf());
			ComPtr<ID2D1GeometrySink> sink;
			path->Open(sink.GetAddressOf());
			sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
			for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(pts[i]);
			sink->EndFigure(D2D1_FIGURE_END_OPEN);
			sink->Close();
			ComPtr<ID2D1StrokeStyle> style;
			const float sw = std::max(stroke, 1.f);
			float dashes[] = { 8.f / sw, 6.f / sw };
			Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
				D2D1::StrokeStyleProperties(
					D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
					D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
				dashes, ARRAYSIZE(dashes), style.GetAddressOf());
			ctx->DrawGeometry(path.Get(), brush.Get(), stroke, style.Get());
		}
		pop();
		return;
	}

	if (id == L"arrow") {
		const float stroke = toolSub->getSliderVal();
		const bool roundCap = toolSub->isArrowRound;
		const std::vector<D2D1_POINT_2F> pts{ { x1, cy }, { x2, cy } };
		auto factory = Ling::D2D::get()->d2dFactory.Get();
		if (toolSub->isLine) {
			// 线段 = 只有轴线：虚线作用于它，cap 跟「圆头」
			auto style = ArrowParts::makeShaftStyle(factory, stroke, roundCap, toolSub->isArrowDash);
			ctx->DrawLine({ x1, cy }, { x2, cy }, brush.Get(), stroke, style.Get());
			pop();
			return;
		}
		if (toolSub->arrowHead == ToolSub::ArrowBig) {
			const auto outline = ArrowPolyState::buildBigArrowPolygon(pts, std::max(1.f, stroke) * 2.5f);
			if (outline.size() >= 3) {
				ComPtr<ID2D1PathGeometry> path;
				Ling::D2D::get()->d2dFactory->CreatePathGeometry(path.GetAddressOf());
				ComPtr<ID2D1GeometrySink> sink;
				path->Open(sink.GetAddressOf());
				sink->SetFillMode(D2D1_FILL_MODE_WINDING);
				sink->BeginFigure(outline[0], D2D1_FIGURE_BEGIN_FILLED);
				for (size_t i = 1; i < outline.size(); i++) sink->AddLine(outline[i]);
				sink->EndFigure(D2D1_FIGURE_END_CLOSED);
				sink->Close();
				ctx->FillGeometry(path.Get(), brush.Get());
			}
			pop();
			return;
		}

		// 与 ShapeArrow::paint 同一套部件：杆身画到尖端（勿缩短，否则平头会卡在三角头里），
		// 虚线只作用于杆身，圆头同时作用于杆身与箭头头部
		auto bodyStyle = ArrowParts::makeShaftStyle(factory, stroke, roundCap, toolSub->isArrowDash);
		auto shaft = ArrowParts::shaftPath(factory, pts);
		if (shaft) ctx->DrawGeometry(shaft.Get(), brush.Get(), stroke, bodyStyle.Get());

		auto headStyle = ArrowParts::makeHeadStyle(factory, roundCap);
		if (toolSub->arrowHead == ToolSub::ArrowAnnot) {
			auto bars = ArrowParts::annotBarsPath(factory, pts, stroke);
			if (bars) ctx->DrawGeometry(bars.Get(), brush.Get(), stroke, headStyle.Get());
		}
		else if (toolSub->arrowHead == ToolSub::ArrowBoth) {
			if (auto head = ArrowParts::openHeadPath(factory, { x2, cy }, { x1, cy }, stroke))
				ctx->DrawGeometry(head.Get(), brush.Get(), stroke, headStyle.Get());
			if (auto head = ArrowParts::openHeadPath(factory, { x1, cy }, { x2, cy }, stroke))
				ctx->DrawGeometry(head.Get(), brush.Get(), stroke, headStyle.Get());
		}
		else {
			if (auto head = ArrowParts::openHeadPath(factory, { x1, cy }, { x2, cy }, stroke))
				ctx->DrawGeometry(head.Get(), brush.Get(), stroke, headStyle.Get());
		}
		pop();
		return;
	}

	// rect 等：半透明线段示意粗细
	const float stroke = toolSub->getSliderVal();
	ctx->DrawLine({ x1, cy }, { x2, cy }, brush.Get(), stroke);
	pop();
}

bool AnnotHost::annotDown(POINT pos, BOOL isRight)
{
	ensureHistory();
	// 对齐 Tauri：编辑中点输入框外只提交退出，同一次点击不点选、不落笔
	if (editingText) {
		if (textBox && textBox->isPosIn(pos)) return true;
		editingText->finishEdit();
		clearAnnotSelection();
		return true;
	}
	// finishEdit 可能因失焦先于本事件完成；吞掉紧随其后的点选/新建
	if (GetTickCount64() < annotSuppressUntil) {
		annotSuppressUntil = 0;
		clearAnnotSelection();
		return true;
	}
	if (isRight) return annotRightClick();

	// 连线：点中线上的叉 —— 直接删掉这条箭头/线段。
	// 优先于其它命中，否则这一下会被当成拖动这条线
	if (linkBtn) {
		const auto ipBtn = toImgPos(pos);
		if (linkBtn->hitLinkButton((float)ipBtn.x, (float)ipBtn.y, dpi)) {
			ShapeBase* doomed = linkBtn;
			linkBtn = nullptr;
			// 整体删除：线中点上挂着"线上文字"的话，文字跟着一起走
			history->removeShapeGroup(doomed);
			return true;
		}
	}

	POINT screenPos{};
	GetCursorPos(&screenPos);
	auto now = GetTickCount64();
	bool isDblClick = (now - annotLastDownTime <= GetDoubleClickTime())
		&& std::abs(screenPos.x - annotLastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
		&& std::abs(screenPos.y - annotLastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
	annotLastDownTime = now;
	annotLastDownPos = screenPos;
	// 双击落在线段"中间热区"（提示挖空那块）时，语义是"在这条线上打字"，不能走下面的
	// "双击切端点 free" —— 它会把这个双击吃掉（日志里 shaft=1 shaftDist=3.5 dbl=1 却
	// 插不进光标就是这个）。用悬停提示标志判断，它在按下前已经算好了；跳过这里之后，
	// 下面通用路径会自己处理"已有文字就编辑、没有就落笔"
	if (isDblClick && !editingText && !textMidHintActive) {
		// 先判断"这一下双击是不是落在线上文字上"再决定撤不撤上一次落笔 ——
		// 顺序反了的话，双击箭头里的文字会先把箭头（或文字）undo 掉，表现就是
		// "重复编辑箭头里的文字，箭头整个不见了，只剩文字"
		bool dblLabel = false;
		if (shapeHover && history) {
			const auto ip = toImgPos(pos);
			const bool isLine = dynamic_cast<ShapeArrow*>(shapeHover) != nullptr
				|| dynamic_cast<ShapeLine*>(shapeHover) != nullptr;
			if (isLine) {
				for (auto& s : history->shapes) {
					auto* t = dynamic_cast<ShapeText*>(s.get());
					if (!t || t->attachTo != shapeHover) continue;
					// 文字框是转过角度的（竖排/斜排），命中要按文字自己的朝向判
					dblLabel = t->hitLabelBox((float)ip.x, (float)ip.y);
					break;
				}
			}
		}
		if (prevPressCreatedShape && history && !dblLabel) history->undo();
		auto imgDbl = toImgPos(pos);
		const float dx = (float)imgDbl.x, dy = (float)imgDbl.y;
		// 双击线段上的文字（文字是线段的一部分，命中的是线段本身）：直接进文字编辑，
		// 不用先单独选中文字
		if (shapeHover && history) {
			bool isLine = dynamic_cast<ShapeArrow*>(shapeHover) != nullptr
				|| dynamic_cast<ShapeLine*>(shapeHover) != nullptr;
			if (isLine) {
				for (auto& s : history->shapes) {
					auto* t = dynamic_cast<ShapeText*>(s.get());
					if (!t || t->attachTo != shapeHover) continue;
					// 双击点落在文字框（= 洞）里才进编辑，双击线的别处仍然按原来的端点逻辑走
					if (t->hitLabelBox(dx, dy)) {
						shapeHover = t;
						setEditingText(t);
						t->startEdit();
						annotMouseDown = false;
						return true;
					}
					break;
				}
			}
		}
		// 对齐 QT：双击箭头/线段端点切换 free（绕对端旋转 ↔ 自由拖点）
		if (shapeHover) {
			if (auto* a = dynamic_cast<ShapeArrow*>(shapeHover)) {
				if (a->toggleFreeEndpoint(dx, dy)) return true;
			}
			else if (auto* l = dynamic_cast<ShapeLine*>(shapeHover)) {
				if (l->toggleFreeEndpoint(dx, dy)) return true;
			}
		}
		if (auto* hit = hitShapeAt(dx, dy)) {
			if (auto* txt = dynamic_cast<ShapeText*>(hit)) {
				shapeHover = txt;
				txt->startEdit();
				annotMouseDown = false;
				return true;
			}
			if (auto* a = dynamic_cast<ShapeArrow*>(hit)) {
				shapeHover = a;
				if (a->toggleFreeEndpoint(dx, dy)) return true;
			}
			else if (auto* l = dynamic_cast<ShapeLine*>(hit)) {
				shapeHover = l;
				if (l->toggleFreeEndpoint(dx, dy)) return true;
			}
		}
		if (annotDoubleClick()) return true;
	}

	annotPressPos = pos;
	annotMouseDown = true;
	hasDragged = false;
	annotDragIndex = -1;
	SetCapture(hwnd);

	const auto tool = getCurToolId();
	auto imgPos = toImgPos(pos);

	if (tool == L"eraser") {
		clearAnnotSelection();
		newShape = nullptr;
		history->eraseAt((float)imgPos.x, (float)imgPos.y);
		return true;
	}

	// 水印/包浆：选工具已 sync，画布点击不新建
	if (tool == L"watermark" || tool == L"patina") {
		if (auto* hit = hitShapeAt((float)imgPos.x, (float)imgPos.y)) {
			newShape = nullptr;
			shapeHover = hit;
			shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
			lockAnnotDragFromHover();
			refresh();
			return true;
		}
		clearAnnotSelection();
		newShape = nullptr;
		annotMouseDown = false;
		ReleaseCapture();
		return true;
	}

	// 文本工具按下点是否落在某条箭头/线段的中间？这里**现场算**，不看悬停提示的标志位 ——
	// 换到文本工具后第一下点击往往没经过一次 mousemove，标志位还是 false，于是那一下就漏了
	// （既没跳过"命中已有标注"，也没把文字挂到线上，看着就像这功能根本没做）
	ShapeBase* shaftHost = nullptr;
	float shaftMx = 0.f, shaftMy = 0.f;   // 命中线段的中点（文字就用它当落笔点）
	float shaftBest = 1e9f;
	if (tool == L"text" && history) {
		for (auto& s : history->shapes) {
			if (!s || s->isUndo) continue;
			float mx = 0.f, my = 0.f;
			bool ok = false, big = false;
			if (auto* a = dynamic_cast<ShapeArrow*>(s.get())) { ok = a->shaftMidPoint(mx, my); big = a->bigHead(); }
			else if (auto* l = dynamic_cast<ShapeLine*>(s.get())) { ok = l->shaftMidPoint(mx, my); }
			if (!ok || big) continue;
			const float tol = 18.f * dpi;
			const float dx = std::abs((float)imgPos.x - mx), dy = std::abs((float)imgPos.y - my);
			// 取"最近"的一条，不能 break：多条箭头时先命中的那条会把后面那条的点击吃掉
			// （表现就是"第二个箭头点了没用、插不进光标"）
			if (dx <= tol && dy <= tol && dx + dy < shaftBest) {
				shaftBest = dx + dy;
				shaftHost = s.get();
				shaftMx = mx;
				shaftMy = my;
			}
		}
	}

	// 这条线上已经有字了：点一下直接进编辑（不再造第二条文字）
	if (shaftHost && history) {
		for (auto& s : history->shapes) {
			auto* t = dynamic_cast<ShapeText*>(s.get());
			if (t && t->attachTo == shaftHost) {
				// 不要动 shapeHover：它得继续是那条线（线条的 hoverDraggerIndex 才不会失配，
				// 否则下一次点它会被当成"新箭头落笔"→ 线被 setTwo(点,点) 缩成一团）
				annotMouseDown = false;
				ReleaseCapture();
				setEditingText(t);
				t->startEdit();
				return true;
			}
		}
	}

	// 对齐 QT/Tauri：按下时先命中已有标注（空工具也能点选），未命中才落笔
	// 录屏工具启用时跳过点选，直接落笔，避免标注过程中拖到旧图形
	// 文本工具落在线段中间时要例外：那儿就是要打字，不能被"命中已有箭头"抢走
	const bool textOnShaft = (shaftHost != nullptr);
	// 连线：箭头/线段工具按在矩形/椭圆里 —— **一律起笔画连线**（选没选中都一样，用户定的：
	// 箭头工具下矩形不该被拖走，要移动它请换矩形工具/空工具）。
	// 没拖就抬手（就是单击）见 annotUp：空线删掉、改成选中那个形状，
	// 于是箭头工具下"点一下=选中、按住拖=连线"
	bool linkFromShape = false;
	linkPressTarget = nullptr;
	if (tool == L"arrow" && !annotSkipHitSelect() && !textOnShaft) {
		if (ShapeBase* t = ShapeLink::pickTarget(this, (float)imgPos.x, (float)imgPos.y)) {
			linkFromShape = true;
			linkPressTarget = t;
		}
	}
	if (!annotSkipHitSelect() && !textOnShaft && !linkFromShape) {
		if (auto* hit = hitShapeAt((float)imgPos.x, (float)imgPos.y)) {
			newShape = nullptr;
			if (shapeHover != hit) {
				if (auto* a = dynamic_cast<ShapeArrow*>(shapeHover)) a->resetFree();
				else if (auto* l = dynamic_cast<ShapeLine*>(shapeHover)) l->resetFree();
				shapeHover = hit;
				refresh();
			}
			shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
			lockAnnotDragFromHover();
			return true;
		}
	}

	clearAnnotSelection();
	newShape = nullptr;

	if (tool.empty()) {
		if (!annotEmptyToolDrag(pos)) {
			annotMouseDown = false;
			ReleaseCapture();
		}
		return true;
	}

	// 文本工具落在线段中间：把文字挂到这条箭头/线段上（之后跟着它移动，见 paintShapes）
	// 落笔点仍用鼠标位置（普通文字工具就是这么定位的，所见即所得）：
	// 上一版改成"以线段中点当落笔点"，结果输入框跑到线段上方，反而更糟，回退
	shapeHover = history->createShape(tool, imgPos.x, imgPos.y);
	newShape = shapeHover;
	// 落笔位置正好在线段中间：给文字开洞（线断在字两边）+ 挂到线上（始终中心对齐、一律横排）
	if (shapeHover && shaftHost) {
		if (auto* t = dynamic_cast<ShapeText*>(shapeHover)) {
			t->punchBg = true;
			t->attachTo = shaftHost;
			float mx = shaftMx, my = shaftMy;   // 用扫描时记下的中点（不再重算）
			if (auto* a = dynamic_cast<ShapeArrow*>(shaftHost)) { a->shaftMidPoint(mx, my); }
			else if (auto* l = dynamic_cast<ShapeLine*>(shaftHost)) { l->shaftMidPoint(mx, my); }
			t->setAttachPose(mx, my);   // 立刻摆正，别等下一帧
			// createShape 可能已经把编辑开起来了（那时输入框还在鼠标落笔的位置）。
			// 这一步才是"热区里任意位置落笔，都从线段中间开始输入"：把输入框挪到锚点
			t->syncEditorGeometry();
		}
		textMidHintActive = false;
		refresh();
	}

	if (!shapeHover) {
		annotMouseDown = false;
		ReleaseCapture();
	}
	else {
		lockAnnotDragFromHover();
	}
	return true;
}

void AnnotHost::annotMove(POINT pos)
{
	if (!history) return;
	if (editingText && textBox && textBox->isPosIn(pos)) return;
	auto imgPos = toImgPos(pos);
	const auto tool = getCurToolId();

	if (annotMouseDown) {
		if (tool.empty() && !shapeHover) {
			annotEmptyToolDrag(pos);
			return;
		}
		if (tool == L"eraser") {
			if (pos.x != annotPressPos.x || pos.y != annotPressPos.y) hasDragged = true;
			history->eraseAt((float)imgPos.x, (float)imgPos.y);
			return;
		}
		if (shapeHover) {
			if (pos.x != annotPressPos.x || pos.y != annotPressPos.y) hasDragged = true;
			applyLockedAnnotDrag();
			shapeHover->mouseDrag((float)imgPos.x, (float)imgPos.y);
			// 拖动中也要刷起点提示点（连线时它跟着箭头起点沿边缘滑）
			updateLinkHints(imgPos, tool);
			// 拖的若是下层标注，上层盖着的模糊/马赛克要跟着重新取样（实时，限流）
			refreshFiltersAboveThrottled(shapeHover);
			refresh();
			applyAnnotCursor();
		}
		return;
	}

	// 未按下：刷新命中以驱动光标（选中态控点 + 未选中悬停手型）
	updateTextMidHint(imgPos, tool);
	updateLinkHints(imgPos, tool);
	if (shapeHover && !shapeHover->isUndo) {
		const int prev = shapeHover->hoverDraggerIndex;
		shapeHover->mouseMove((float)imgPos.x, (float)imgPos.y);
		if (prev != shapeHover->hoverDraggerIndex) setTimer(800, 100);
	}
	applyAnnotCursor();
	// 文本工具停在"线段中间"（含线上已经有字）：语义就是打字，最后再压一次工字型，别被悬停手型盖掉
	if (textMidHintActive) SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	// 停在连线的"断开叉"上：手型，提示可点
	if (linkBtn) SetCursor(LoadCursor(nullptr, IDC_HAND));
	// 停在可连形状上（起点"＋"柄）：手型，提示"按住就能拖出连线"
	else if (linkHoverTarget) SetCursor(LoadCursor(nullptr, IDC_HAND));
	// 序号跟手预览需每帧重绘
	if (numberCursorFollow || getCurToolId() == L"number") refresh();
}

void AnnotHost::annotUp(POINT pos, BOOL isRight)
{
	if (isRight) return;
	annotMouseDown = false;
	annotDragIndex = -1;
	ReleaseCapture();
	if (!history) return;
	auto justCreated = newShape;
	newShape = nullptr;
	prevPressCreatedShape = false;
	// 松手即"停悬"：拖动中提示点跟着起点，松手后要立刻按"鼠标底下是谁"重算 ——
	// 从 A 连到 B、在 B 上松手，这时 B 就该显示起点了（不用等鼠标再动一下）
	updateLinkHints(toImgPos(pos), getCurToolId());

	if (getCurToolId().empty() && !shapeHover) {
		annotAfterEmptyToolUp();
		return;
	}
	if (!shapeHover) return;
	// "没拖"的判定给一点手抖余量：1~2px 的抖动不该被当成"画了一条"（否则箭头会被撑到最短长度，
	// 而"点一下选中形状"也就失灵了）
	const float jitter = 3.f * dpi;
	const bool tinyMove = std::abs((float)(pos.x - annotPressPos.x)) <= jitter
		&& std::abs((float)(pos.y - annotPressPos.y)) <= jitter;
	if (shapeHover == justCreated && (tinyMove || !hasDragged) && !shapeHover->isValidWithoutDrag()) {
		history->removeShape(shapeHover);
		// 这一下本来是"从形状里起笔连线"但没拖（就是单击）：那条空线删掉，改成**选中那个形状**。
		// 于是箭头工具下"点一下=选中、按住拖=连线"，选中之后再按住拖就是移动它
		if (linkPressTarget && history) {
			shapeHover = linkPressTarget;
			refresh();
			applyAnnotCursor();
		}
		linkPressTarget = nullptr;
		return;
	}
	linkPressTarget = nullptr;
	prevPressCreatedShape = (shapeHover == justCreated);
	auto imgPos = toImgPos(pos);
	shapeHover->mouseUp((float)imgPos.x, (float)imgPos.y);
	// 抬手后按最终几何再采样一次（拖拽中已经跟手重算，这里收个尾更稳）
	refreshFiltersAbove(shapeHover);

	// 共用：画完一律不立刻出控点；再次点击才选中（文字编辑靠 editingText，不靠选中态）
	if (justCreated)
		shapeHover = nullptr;
	refresh();
	applyAnnotCursor();
}

void AnnotHost::annotKey(UINT key)
{
	if (editingText) {
		if (key == VK_ESCAPE) editingText->finishEdit();
		return;
	}
	if (!history) return;
	bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	if (ctrl && key == 'Z') history->undo();
	else if (ctrl && key == 'Y') history->redo();
	else if (key == VK_DELETE || key == VK_BACK) history->removeHoverShape();
	else if (key == VK_ESCAPE) clearAnnotSelection();
}

bool AnnotHost::getAnnotPixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size, const D2D1_RECT_F* crop)
{
	if (!screenImg) return false;
	if (editingText) editingText->finishEdit();
	auto imgSize = screenImg->GetPixelSize();
	UINT32 outW = imgSize.width, outH = imgSize.height;
	UINT32 srcX = 0, srcY = 0;
	if (crop) {
		srcX = (UINT32)std::max(0.f, std::floor(crop->left));
		srcY = (UINT32)std::max(0.f, std::floor(crop->top));
		outW = (UINT32)std::max(1.f, std::floor(crop->right - crop->left));
		outH = (UINT32)std::max(1.f, std::floor(crop->bottom - crop->top));
		if (srcX + outW > imgSize.width) outW = imgSize.width - srcX;
		if (srcY + outH > imgSize.height) outH = imgSize.height - srcY;
	}
	if (outW == 0 || outH == 0) return false;

	auto d2d = Ling::D2D::get();
	D2D1_BITMAP_PROPERTIES1 props{
		.pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> target;
	if (FAILED(d2d->deviceContext->CreateBitmap(D2D1::SizeU(outW, outH), nullptr, 0, &props, target.GetAddressOf())))
		return false;
	d2d->deviceContext->SetTarget(target.Get());
	d2d->deviceContext->BeginDraw();
	d2d->deviceContext->Clear(D2D1::ColorF(0, 0.f));
	const float ox = -(float)srcX, oy = -(float)srcY;
	d2d->deviceContext->SetTransform(D2D1::Matrix3x2F::Translation(ox, oy));
	d2d->deviceContext->DrawBitmap(screenImg.Get(),
		D2D1::RectF(0, 0, (float)imgSize.width, (float)imgSize.height));
	paintShapes(d2d->deviceContext.Get(), false, true);
	d2d->deviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
	if (FAILED(d2d->deviceContext->EndDraw())) {
		d2d->deviceContext->SetTarget(nullptr);
		return false;
	}
	d2d->deviceContext->SetTarget(nullptr);

	D2D1_BITMAP_PROPERTIES1 cpuProp{
		.pixelFormat{ target->GetPixelFormat() },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBmp;
	if (FAILED(d2d->deviceContext->CreateBitmap(D2D1::SizeU(outW, outH), nullptr, 0, &cpuProp, cpuBmp.GetAddressOf())))
		return false;
	if (FAILED(cpuBmp->CopyFromBitmap(nullptr, target.Get(), nullptr))) return false;
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
	const UINT32 rowBytes = outW * 4;
	pixels.resize((size_t)rowBytes * outH);
	for (UINT32 row = 0; row < outH; row++)
		CopyMemory(pixels.data() + (size_t)row * rowBytes, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
	cpuBmp->Unmap();
	size = D2D1::SizeU(outW, outH);
	return true;
}

bool AnnotHost::composeAnnotOnImage(const BYTE* bgra, int imgW, int imgH, std::vector<BYTE>& out)
{
	if (!bgra || imgW <= 0 || imgH <= 0) return false;
	if (!history) {
		out.assign(bgra, bgra + (size_t)imgW * imgH * 4);
		return true;
	}
	bool any = false;
	for (auto& s : history->shapes) {
		if (s && !s->isUndo) { any = true; break; }
	}
	if (!any) {
		out.assign(bgra, bgra + (size_t)imgW * imgH * 4);
		return true;
	}
	if (editingText) editingText->finishEdit();
	auto d2d = Ling::D2D::get();
	D2D1_BITMAP_PROPERTIES1 props{
		.pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> target;
	if (FAILED(d2d->deviceContext->CreateBitmap(D2D1::SizeU(imgW, imgH), nullptr, 0, &props, target.GetAddressOf())))
		return false;
	ComPtr<ID2D1Bitmap1> srcBmp;
	D2D1_BITMAP_PROPERTIES1 srcProps{
		.pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_NONE }
	};
	if (FAILED(d2d->deviceContext->CreateBitmap(D2D1::SizeU(imgW, imgH), bgra, imgW * 4, &srcProps, srcBmp.GetAddressOf())))
		return false;
	d2d->deviceContext->SetTarget(target.Get());
	d2d->deviceContext->BeginDraw();
	d2d->deviceContext->Clear(D2D1::ColorF(0, 0.f));
	d2d->deviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
	d2d->deviceContext->DrawBitmap(srcBmp.Get(), D2D1::RectF(0, 0, (float)imgW, (float)imgH));
	paintShapes(d2d->deviceContext.Get(), false, true);
	if (FAILED(d2d->deviceContext->EndDraw())) {
		d2d->deviceContext->SetTarget(nullptr);
		return false;
	}
	d2d->deviceContext->SetTarget(nullptr);

	D2D1_BITMAP_PROPERTIES1 cpuProp{
		.pixelFormat{ target->GetPixelFormat() },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBmp;
	if (FAILED(d2d->deviceContext->CreateBitmap(D2D1::SizeU(imgW, imgH), nullptr, 0, &cpuProp, cpuBmp.GetAddressOf())))
		return false;
	if (FAILED(cpuBmp->CopyFromBitmap(nullptr, target.Get(), nullptr))) return false;
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
	const UINT32 rowBytes = (UINT32)imgW * 4;
	out.resize((size_t)rowBytes * imgH);
	for (int row = 0; row < imgH; row++)
		CopyMemory(out.data() + (size_t)row * rowBytes, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
	cpuBmp->Unmap();
	return true;
}
