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
#include "../Shape/AnnotCursor.h"
#include "../Tool/SerialEmoji.h"
#include "../Util.h"
#include <imm.h>
#include <cstdio>

using namespace Microsoft::WRL;

namespace {
	// 文本工具下，马赛克这类"整块铺满的背景"不参与命中判定：否则点它会先被当成选中/拖动，
	// 文字落不下去。马赛克与普通标注是同一套按创建顺序叠加的模型，后画的盖住先画的，
	// 所以光标停在这儿应当能直接打字（文字创建后就压在它上面）。
	bool textToolIgnoresShape(const std::wstring& tool, ShapeBase* shape)
	{
		return tool == L"text" && shape && dynamic_cast<ShapeMosaic*>(shape) != nullptr;
	}
}

AnnotHost::AnnotHost()
{
	// 标注文字是画布内自绘的，没有系统插入符；输入法的组字窗/候选框位置只能由宿主
	// 通过 IMM 告知。不订阅这个事件，输入法就按窗口默认原点摆 —— 截图窗口是全屏的，
	// 表现就是「打字时输入法候选框跑到屏幕左上角」。
	onIME.add([this] { positionImeWindow(); });
}

AnnotHost::~AnnotHost()
{
	// WinBase 析构不销毁 HWND：不 close 就析构会留下"幽灵窗"，WndProc 重入已释放对象
	if (hwnd) { close(); hwnd = nullptr; }
}

void AnnotHost::ensureHistory()
{
	if (!history) history = std::make_unique<History>(this);
	if (!toolSub) toolSub = std::make_unique<ToolSub>(this);
}

void AnnotHost::onHistoryUndoRedo()
{
	// 组框缓存的是变换前的几何：撤销/重做把标注换回去后必须重新包一遍
	if (hasMultiSelection()) syncGroupBoxFromSelection();
	refresh();
}

void AnnotHost::onHistoryChanged()
{
	// 撤销/重做/删除都会改变下层内容：上层滤镜重算一遍
	refreshFiltersAbove(nullptr);
	pruneAnnotSelection();
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

void AnnotHost::setMainBarAnchorId(const std::wstring& id)
{
	if (toolCap) toolCap->setAnchorId(id);
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
	// 15ms 与系统时钟节拍对齐，实际约 64Hz
	setTimer(15, 211);
}

std::wstring AnnotHost::getCurToolId() const
{
	if (!forcedToolId.empty()) return forcedToolId;
	if (!toolCap || toolCap->curId.empty()) return L"";
	const auto eff = toolCap->resolveToolId(toolCap->curId);
	// 「标注」是箭头槽位里的线段子工具：底层仍按 arrow 路由，画箭头还是线段由 toolSub->isLine 决定
	return eff == L"line" ? L"arrow" : eff;
}

std::wstring AnnotHost::styleToolId() const
{
	const auto id = getCurToolId();
	// 图片编辑器的「尺寸」是独立工具 id（保证落笔一定是尺寸），
	// 但属性栏/选中样式仍归 arrow 家族，这样线型、颜色、粗细的交互保持一致。
	return id == L"dimension" ? L"arrow" : id;
}

// 线上文字开洞：默认用屏幕捕获图把文字区域原样盖回去。
// 宿主可以重写（图片编辑器的画板可能是透明的，需要先铺画布底色）。
void AnnotHost::paintPunchBackground(ID2D1DeviceContext* ctx, const D2D1_RECT_F&)
{
	ID2D1Bitmap1* src = punchSource_ ? punchSource_ : screenImg.Get();
	if (!ctx || !src) return;
	// 底图按自身像素尺寸铺（贴图缩放时客户区尺寸 ≠ 图像尺寸，用 w/h 会抠错位置）；
	// 尺寸对不上时仍按窗口尺寸铺，保持白板/演示的原行为
	const D2D1_SIZE_U srcPx = src->GetPixelSize();
	const float sc = scale > 0.001f ? scale : 1.f;
	const bool sameSpace = std::fabs((float)srcPx.width - (float)w / sc) <= 2.f
		&& std::fabs((float)srcPx.height - (float)h / sc) <= 2.f;
	ctx->DrawBitmap(src, sameSpace
		? D2D1::RectF(0.f, 0.f, (float)srcPx.width, (float)srcPx.height)
		: D2D1::RectF(0.f, 0.f, (float)w, (float)h));
}

// 按住 Alt = 强制落笔：跳过"按下先命中已有标注"，让当前工具直接压在旧标注上作画。
// 只在"手里有工具"时生效 —— 空工具（选择态）下 Alt 不改变"点谁选谁"。
bool AnnotHost::altForceDraw() const
{
	// GetKeyState 读的是本线程消息队列里的键状态：处理这条鼠标消息时，Alt 的状态就是
	// 按下/抬起这一下的真实状态（比 GetAsyncKeyState 更适合配合鼠标事件判断）。
	if (getCurToolId().empty()) return false;
	return (GetKeyState(VK_MENU) & 0x8000) != 0;
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

// 尺寸/角度：给线挂一句自动文字。拖拽过程中就调用，文字会跟着线实时出现；
// 已经挂过（含用户手动改过内容）的直接返回，不重复创建。
// 尺寸挂的是标记箭头（ShapeArrow），角度挂的是 ShapeLine，这里两种都认。
ShapeText* AnnotHost::ensureShaftText(ShapeBase* host, const std::wstring& text, float px)
{
	if (!host || !history) return nullptr;
	for (auto& s : history->shapes) {
		auto* t = dynamic_cast<ShapeText*>(s.get());
		if (t && t->attachTo == host && !s->isUndo) return t;
	}
	auto txt = std::make_unique<ShapeText>(this);
	// 尺寸的文字压在线上，需要挖洞把线断开；角度的数字在角的外侧，不挖洞
	//（否则会把底下的线/标注剪出缺口）
	auto* ln = dynamic_cast<ShapeLine*>(host);
	txt->punchBg = !(ln && ln->isAngle());
	txt->attachTo = host;
	txt->initAttachedText(text, px);
	float mx = 0.f, my = 0.f;
	bool ok = false;
	if (auto* a = dynamic_cast<ShapeArrow*>(host)) ok = a->shaftMidPoint(mx, my);
	else if (auto* l = dynamic_cast<ShapeLine*>(host)) ok = l->shaftMidPoint(mx, my);
	if (ok) txt->setAttachPose(mx, my);
	auto* raw = txt.get();
	history->shapes.push_back(std::move(txt));
	return raw;
}

namespace {
	// 显示器物理像素密度（每英寸像素）。系统 DPI 只是逻辑值，与物理密度无关，
	// 不能直接用于厘米换算。
	float physicalDpi()
	{
		static const float cached = [] {
			float dpi = 96.f;
			if (HDC hdc = GetDC(nullptr)) {
				const int px = GetDeviceCaps(hdc, HORZRES);
				const int mm = GetDeviceCaps(hdc, HORZSIZE);
				ReleaseDC(nullptr, hdc);
				if (px > 0 && mm > 0) {
					const float v = (float)px * 25.4f / (float)mm;
					if (v >= 40.f && v <= 1000.f) dpi = v;
				}
			}
			return dpi;
		}();
		return cached;
	}
}

// 尺寸标注文字：线长的物理尺寸。图像像素与屏幕物理像素一致，
// 按显示器物理密度换算厘米。
std::wstring AnnotHost::dimensionTextFor(ShapeBase* host) const
{
	float px = 0.f;
	if (auto* a = dynamic_cast<ShapeArrow*>(host)) px = a->shaftLength();
	const float cm = px * 2.54f / physicalDpi();
	wchar_t buf[32]{};
	swprintf_s(buf, L"%.1f cm", cm);
	return buf;
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
	if (shapeHover && !shapeHover->isUndo && !shapeHover->isViewportFilter()
		&& (!shapeHover->backgroundLayer || boardBackgroundEditing())) {
		shapeHover->mouseMove(x, y);
		if (shapeHover->hoverDraggerIndex >= 0) return groupOf(shapeHover);
	}
	const bool bgEdit = boardBackgroundEditing();
	for (int i = (int)history->shapes.size() - 1; i >= 0; i--) {
		auto* cur = history->shapes[i].get();
		if (cur->isUndo || cur == shapeHover || cur->isViewportFilter() || cur->isEphemeral()) continue;
		// 背景层标注只有在「编辑背景」打开时才可点选；平时它属于背景，不单独选中
		if (cur->backgroundLayer && !bgEdit) continue;
		cur->mouseMove(x, y);
		if (cur->hoverDraggerIndex >= 0) return groupOf(cur);
	}
	return nullptr;
}

bool AnnotHost::nudgeSelectedAnnot(float dx, float dy)
{
	if (!history) return false;
	// 落笔中/拖动中/编辑文字时不存在"选中标注微调"
	if (annotMouseDown || newShape || editingText) return false;
	const auto targets = selectedShapes();
	if (targets.empty()) return false;
	std::vector<std::vector<float>> before;
	before.reserve(targets.size());
	for (auto* s : targets) before.push_back(s->editSnapshot());
	bool moved = false;
	for (auto* s : targets) {
		if (s->translateBy(dx, dy)) moved = true;
	}
	if (!moved) return false;
	// 连线跟着贴回新位置（与鼠标拖动同一口径）
	for (auto& o : history->shapes) {
		if (o && !o->isUndo) o->syncLinks();
	}
	refreshFiltersAbove(nullptr);
	std::vector<std::vector<float>> after;
	after.reserve(targets.size());
	for (auto* s : targets) after.push_back(s->editSnapshot());
	history->addEditSteps(targets, before, after);
	refresh();
	return true;
}

void AnnotHost::clearAnnotSelection(bool keepStylePreview)
{
	if (!shapeHover && extraSelection_.empty()) return;
	if (auto* a = dynamic_cast<ShapeArrow*>(shapeHover)) a->resetFree();
	else if (auto* l = dynamic_cast<ShapeLine*>(shapeHover)) l->resetFree();
	shapeHover = nullptr;
	extraSelection_.clear();
	groupBox_ = D2D1::RectF(0.f, 0.f, -1.f, -1.f);
	groupAngle_ = 0.f;
	groupScaleActive_ = false;
	styleLoadCandidate = nullptr;
	marqueeActive_ = false;
	groupMoveActive_ = false;
	groupRotateActive_ = false;
	groupTargets_.clear();
	groupBefore_.clear();
	if (toolSub) {
		toolSub->endMultiStyle();
		toolSub->clearSelectedNumberValue();
	// 取消选中：属性栏还原为工具默认值；开始绘制新标注时传入 true，保留已载入的样式。
		if (!keepStylePreview) toolSub->endShapeStylePreview();
	}
	refresh();
}

bool AnnotHost::isShapeSelected(ShapeBase* s) const
{
	if (!s) return false;
	if (s == shapeHover) return true;
	return std::find(extraSelection_.begin(), extraSelection_.end(), s) != extraSelection_.end();
}

bool AnnotHost::hasAnyAnnotSelection() const
{
	return shapeHover != nullptr || !extraSelection_.empty();
}

void AnnotHost::selectShapesInRect(const D2D1_RECT_F& rect, bool additive)
{
	if (!history) return;
	if (!additive) clearAnnotSelection(true);
	if (ShapeBase::rectEmpty(rect)) { refresh(); return; }
	for (auto& sp : history->shapes) {
		ShapeBase* s = sp.get();
		if (!s || s->isUndo) continue;
		auto b = s->bounds();
		if (ShapeBase::rectEmpty(b)) continue;
		if (b.right < rect.left || b.left > rect.right || b.bottom < rect.top || b.top > rect.bottom) continue;
		if (isShapeSelected(s)) continue;
		if (!shapeHover) shapeHover = s;
		else extraSelection_.push_back(s);
	}
	syncGroupBoxFromSelection();
	if (toolSub && hasMultiSelection()) toolSub->beginMultiStyle(selectedShapes());
	refresh();
}

std::vector<ShapeBase*> AnnotHost::selectedShapes() const
{
	std::vector<ShapeBase*> out;
	if (!history) return out;
	auto alive = [this](ShapeBase* s) {
		if (!s) return false;
		for (auto& o : history->shapes) {
			if (o.get() == s && !o->isUndo) return true;
		}
		return false;
	};
	if (alive(shapeHover)) out.push_back(shapeHover);
	for (auto* s : extraSelection_) {
		if (s == shapeHover || !alive(s)) continue;
		if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
	}
	return out;
}

std::vector<ShapeBase*> AnnotHost::selectedShapesForTool(const std::wstring& toolId) const
{
	std::vector<ShapeBase*> out;
	if (toolId.empty() || !toolSub) return out;
	for (auto* s : selectedShapes()) {
		if (!s || toolIdOfShape(s) != toolId) continue;
		if (toolId == L"arrow") {
			// 「标注」栏：按当前形态只取同一类（大箭头/普通箭头/线段/气泡/爆裂/尺寸/角度）
			if (toolSub->isLine) {
				const int want = toolSub->lineKind;
				if (auto* t = dynamic_cast<ShapeText*>(s)) {
					if (!t->isFrame()) continue;
					if (want == ToolSub::LineBubble && !t->isBubble()) continue;
					if (want == ToolSub::LineBurst && !t->isBurst()) continue;
					if (want != ToolSub::LineBubble && want != ToolSub::LineBurst) continue;
				}
				else if (auto* l = dynamic_cast<ShapeLine*>(s)) {
					if (want == ToolSub::LineStraight && l->lineKind() != ShapeLine::Straight) continue;
					if (want == ToolSub::LineWave && l->lineKind() != ShapeLine::Wave) continue;
					if (want == ToolSub::LineAngle && l->lineKind() != ShapeLine::Angle) continue;
					if (want == ToolSub::LineBubble || want == ToolSub::LineBurst
						|| want == ToolSub::LineDimension) continue;
				}
				else if (auto* a = dynamic_cast<ShapeArrow*>(s)) {
					if (!a->isDimension() || want != ToolSub::LineDimension) continue;
				}
				else continue;
			}
			else {
				auto* a = dynamic_cast<ShapeArrow*>(s);
				if (!a || a->isDimension()) continue;
				if (a->arrowHeadStyle() != toolSub->arrowHead) continue;
			}
		}
		else if (toolId == L"rect") {
			if (toolSub->isEllipse) {
				if (!dynamic_cast<ShapeEllipse*>(s)) continue;
			}
			else {
				auto* r = dynamic_cast<ShapeRect*>(s);
				if (!r) continue;
				const int want = toolSub->rectKind == ToolSub::PolygonShape ? ShapeRect::Polygon
					: toolSub->rectKind == ToolSub::StarShape ? ShapeRect::Star : ShapeRect::Rect;
				if (r->kind() != want) continue;
			}
		}
		else if (toolId == L"text") {
			auto* t = dynamic_cast<ShapeText*>(s);
			if (!t || t->isFrame()) continue;
		}
		else if (toolId == L"number") {
			// 数字/字母/Emoji 是不同形态：只改当前形态，避免改样式时把别的形态也拢进来
			auto* n = dynamic_cast<ShapeNumber*>(s);
			if (!n || n->style() != toolSub->numberStyle) continue;
		}
		out.push_back(s);
	}
	return out;
}

void AnnotHost::pruneAnnotSelection()
{
	if (!history) {
		shapeHover = nullptr;
		pendingPen = nullptr;
		extraSelection_.clear();
		return;
	}
	auto alive = [this](ShapeBase* s) {
		if (!s) return false;
		for (auto& o : history->shapes) {
			if (o.get() == s && !o->isUndo) return true;
		}
		return false;
	};
	bool changed = false;
	if (pendingPen && !alive(pendingPen)) { pendingPen = nullptr; changed = true; }
	if (shapeHover && !alive(shapeHover)) { shapeHover = nullptr; changed = true; }
	std::vector<ShapeBase*> keep;
	for (auto* s : extraSelection_) {
		if (s == shapeHover || !alive(s)) { changed = true; continue; }
		if (std::find(keep.begin(), keep.end(), s) == keep.end()) keep.push_back(s);
	}
	extraSelection_.swap(keep);
	// 主选中没了但还有其它选中：提一个当主选中，属性栏/控点才有落点
	if (!shapeHover && !extraSelection_.empty()) {
		shapeHover = extraSelection_.back();
		extraSelection_.pop_back();
		changed = true;
	}
	if (changed) syncGroupBoxFromSelection();
}

bool AnnotHost::hasSelectableShapes() const
{
	if (!history) return false;
	const bool bg = boardBackgroundEditing();
	for (auto& s : history->shapes) {
		if (!s || s->isUndo || s->isViewportFilter() || s->isEphemeral()) continue;
		if (s->backgroundLayer && !bg) continue;
		if (auto* t = dynamic_cast<ShapeText*>(s.get()); t && t->attachTo) continue;
		if (!ShapeBase::rectEmpty(s->bounds())) return true;
	}
	return false;
}

D2D1_RECT_F AnnotHost::selectionBounds() const
{
	D2D1_RECT_F out = D2D1::RectF(0.f, 0.f, -1.f, -1.f);
	for (auto* s : selectedShapes()) out = ShapeBase::unionRect(out, s->bounds());
	if (ShapeBase::rectEmpty(out)) return out;
	// 与 ShapeBase::selectionRect 同口径：控件相对本体外扩 2px
	const float pad = 2.f * dpi;
	return D2D1::RectF(out.left - pad, out.top - pad, out.right + pad, out.bottom + pad);
}

void AnnotHost::syncGroupBoxFromSelection()
{
	if (!hasMultiSelection()) {
		groupBox_ = D2D1::RectF(0.f, 0.f, -1.f, -1.f);
		groupAngle_ = 0.f;
		return;
	}
	groupBox_ = selectionBounds();
	groupAngle_ = 0.f;
}

bool AnnotHost::groupBoxContains(float x, float y) const
{
	if (!hasMultiSelection() || ShapeBase::rectEmpty(groupBox_)) return false;
	const auto p = ShapeBase::boxToLocalPoint(groupBox_, groupAngle_, x, y);
	return p.x >= groupBox_.left && p.x <= groupBox_.right
		&& p.y >= groupBox_.top && p.y <= groupBox_.bottom;
}

int AnnotHost::hitGroupHandle(float x, float y) const
{
	if (!hasMultiSelection() || ShapeBase::rectEmpty(groupBox_)) return -1;
	const auto p = ShapeBase::boxToLocalPoint(groupBox_, groupAngle_, x, y);
	const auto& r = groupBox_;
	const float band = 8.f * dpi;
	const float corner = (std::max)(band * 1.2f, 7.f * dpi);
	const bool nearL = p.x >= r.left - band && p.x <= r.left + corner;
	const bool nearR = p.x >= r.right - corner && p.x <= r.right + band;
	const bool nearT = p.y >= r.top - band && p.y <= r.top + corner;
	const bool nearB = p.y >= r.bottom - corner && p.y <= r.bottom + band;
	const bool inX = p.x >= r.left - band && p.x <= r.right + band;
	const bool inY = p.y >= r.top - band && p.y <= r.bottom + band;
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

int AnnotHost::hitGroupRotateCorner(float x, float y) const
{
	if (!hasMultiSelection() || ShapeBase::rectEmpty(groupBox_)) return -1;
	const auto p = ShapeBase::boxToLocalPoint(groupBox_, groupAngle_, x, y);
	const auto& r = groupBox_;
	if (p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom) return -1;
	const float inner = 4.f * dpi, outer = 26.f * dpi;
	const D2D1_POINT_2F corners[4]{
		{ r.left, r.top }, { r.right, r.top }, { r.right, r.bottom }, { r.left, r.bottom }
	};
	const int idx[4]{ 0, 2, 4, 6 };
	for (int i = 0; i < 4; ++i) {
		const float d = std::hypot(p.x - corners[i].x, p.y - corners[i].y);
		if (d > inner && d <= outer) return idx[i];
	}
	return -1;
}

void AnnotHost::toggleShapeInSelection(ShapeBase* s)
{
	if (!s) return;
	if (s == shapeHover) {
		if (extraSelection_.empty()) clearAnnotSelection();
		else {
			shapeHover = extraSelection_.back();
			extraSelection_.pop_back();
			syncGroupBoxFromSelection();
			refresh();
		}
		return;
	}
	auto it = std::find(extraSelection_.begin(), extraSelection_.end(), s);
	if (it != extraSelection_.end()) extraSelection_.erase(it);
	else if (!shapeHover) shapeHover = s;
	else extraSelection_.push_back(s);
	syncGroupBoxFromSelection();
	refresh();
}

void AnnotHost::beginMarquee(const POINT& imgPos)
{
	marqueeActive_ = true;
	marqueeStart_ = D2D1::Point2F((float)imgPos.x, (float)imgPos.y);
	marqueeRect_ = D2D1::RectF(marqueeStart_.x, marqueeStart_.y, marqueeStart_.x, marqueeStart_.y);
	hasDragged = false;
	refresh();
}

void AnnotHost::updateMarquee(const POINT& imgPos)
{
	if (!marqueeActive_) return;
	const float x = (float)imgPos.x, y = (float)imgPos.y;
	marqueeRect_ = D2D1::RectF(
		(std::min)(marqueeStart_.x, x), (std::min)(marqueeStart_.y, y),
		(std::max)(marqueeStart_.x, x), (std::max)(marqueeStart_.y, y));
	if (std::abs(x - marqueeStart_.x) > 2.f || std::abs(y - marqueeStart_.y) > 2.f) hasDragged = true;
}

void AnnotHost::finishMarquee()
{
	const auto r = marqueeRect_;
	marqueeActive_ = false;
	if (ShapeBase::rectEmpty(r) || r.right - r.left < 3.f * dpi || r.bottom - r.top < 3.f * dpi) {
		clearAnnotSelection();
		return;
	}
	std::vector<ShapeBase*> picked;
	const bool bg = boardBackgroundEditing();
	for (auto& s : history->shapes) {
		if (!s || s->isUndo || s->isViewportFilter() || s->isEphemeral()) continue;
		if (s->backgroundLayer && !bg) continue;
		// 线上文字跟着宿主走，不单独进多选（宿主会一起被框进来）
		if (auto* t = dynamic_cast<ShapeText*>(s.get()); t && t->attachTo) continue;
		const auto b = s->bounds();
		if (ShapeBase::rectEmpty(b)) continue;
		if (b.right < r.left || b.left > r.right || b.bottom < r.top || b.top > r.bottom) continue;
		picked.push_back(s.get());
	}
	clearAnnotSelection();
	if (picked.empty()) {
		refresh();
		return;
	}
	shapeHover = picked.back();   // 最上层那个当主选中
	for (size_t i = 0; i + 1 < picked.size(); ++i) extraSelection_.push_back(picked[i]);
	syncGroupBoxFromSelection();
	refresh();
}

void AnnotHost::beginGroupTransform(bool rotate, const POINT& imgPos)
{
	if (ShapeBase::rectEmpty(groupBox_)) syncGroupBoxFromSelection();
	groupTargets_ = selectedShapes();
	if (groupTargets_.empty()) return;
	groupBefore_.clear();
	for (auto* s : groupTargets_) groupBefore_.push_back(s->editSnapshot());
	hasDragged = false;
	annotDragIndex = -1;
	if (rotate) {
		groupPivot_ = D2D1::Point2F((groupBox_.left + groupBox_.right) * 0.5f,
			(groupBox_.top + groupBox_.bottom) * 0.5f);
		groupRotateStartAngle_ = std::atan2((float)imgPos.y - groupPivot_.y, (float)imgPos.x - groupPivot_.x);
		groupRotateLastDelta_ = 0.f;
		groupRotateActive_ = true;
	}
	else {
		groupTransformStart_ = D2D1::Point2F((float)imgPos.x, (float)imgPos.y);
		groupLastDelta_ = D2D1::Point2F(0.f, 0.f);
		groupMoveActive_ = true;
	}
}

void AnnotHost::updateGroupTransform(const POINT& imgPos, bool rotate)
{
	if (groupTargets_.empty()) return;
	if (rotate && groupRotateActive_) {
		const float a = std::atan2((float)imgPos.y - groupPivot_.y, (float)imgPos.x - groupPivot_.x);
		float delta = a - groupRotateStartAngle_;
		if (GetKeyState(VK_SHIFT) & 0x8000) {
			constexpr float kStep = 3.14159265358979323846f / 4.f;
			delta = std::round(delta / kStep) * kStep;
		}
		const float step = delta - groupRotateLastDelta_;
		if (std::fabs(step) < 1e-5f) return;
		for (auto* s : groupTargets_) s->rotateBy(groupPivot_.x, groupPivot_.y, step);
		groupAngle_ += step;
		groupRotateLastDelta_ = delta;
		hasDragged = true;
		for (auto& s : history->shapes) if (s && !s->isUndo) s->syncLinks();
		refreshFiltersAbove(nullptr);
		refresh();
		return;
	}
	if (groupMoveActive_) {
		const D2D1_POINT_2F delta{ (float)imgPos.x - groupTransformStart_.x,
			(float)imgPos.y - groupTransformStart_.y };
		const D2D1_POINT_2F step{ delta.x - groupLastDelta_.x, delta.y - groupLastDelta_.y };
		if (std::fabs(step.x) < 0.01f && std::fabs(step.y) < 0.01f) return;
		for (auto* s : groupTargets_) s->translateBy(step.x, step.y);
		groupBox_ = D2D1::RectF(groupBox_.left + step.x, groupBox_.top + step.y,
			groupBox_.right + step.x, groupBox_.bottom + step.y);
		groupLastDelta_ = delta;
		if (std::fabs(delta.x) > 2.f || std::fabs(delta.y) > 2.f) hasDragged = true;
		for (auto& s : history->shapes) if (s && !s->isUndo) s->syncLinks();
		refreshFiltersAbove(nullptr);
		refresh();
	}
}

void AnnotHost::beginGroupScale(int handle, const POINT& imgPos)
{
	(void)imgPos;
	if (ShapeBase::rectEmpty(groupBox_)) syncGroupBoxFromSelection();
	groupTargets_ = selectedShapes();
	if (groupTargets_.empty()) return;
	groupBefore_.clear();
	for (auto* s : groupTargets_) groupBefore_.push_back(s->editSnapshot());
	groupScaleStartRect_ = groupBox_;
	groupScaleHandle_ = handle;
	groupScaleStartCenter_ = D2D1::Point2F(
		(groupBox_.left + groupBox_.right) * 0.5f,
		(groupBox_.top + groupBox_.bottom) * 0.5f);
	groupScaleLastCenter_ = groupScaleStartCenter_;
	groupScaleLastSx_ = 1.f;
	groupScaleLastSy_ = 1.f;
	groupScaleActive_ = true;
	hasDragged = false;
	annotDragIndex = -1;
}

void AnnotHost::updateGroupScale(const POINT& imgPos)
{
	if (!groupScaleActive_ || groupTargets_.empty()) return;
	const auto start = groupScaleStartRect_;
	if (ShapeBase::rectEmpty(start)) return;
	const auto p = ShapeBase::boxToLocalPoint(start, groupAngle_, (float)imgPos.x, (float)imgPos.y);
	auto r = start;
	const float minSize = 10.f * dpi;
	switch (groupScaleHandle_) {
	case 0: r.left = (std::min)(p.x, r.right - minSize); r.top = (std::min)(p.y, r.bottom - minSize); break;
	case 1: r.top = (std::min)(p.y, r.bottom - minSize); break;
	case 2: r.right = (std::max)(p.x, r.left + minSize); r.top = (std::min)(p.y, r.bottom - minSize); break;
	case 3: r.right = (std::max)(p.x, r.left + minSize); break;
	case 4: r.right = (std::max)(p.x, r.left + minSize); r.bottom = (std::max)(p.y, r.top + minSize); break;
	case 5: r.bottom = (std::max)(p.y, r.top + minSize); break;
	case 6: r.left = (std::min)(p.x, r.right - minSize); r.bottom = (std::max)(p.y, r.top + minSize); break;
	case 7: r.left = (std::min)(p.x, r.right - minSize); break;
	default: return;
	}
	const float oldW = start.right - start.left, oldH = start.bottom - start.top;
	const float newW = r.right - r.left, newH = r.bottom - r.top;
	if (oldW < 1.f || oldH < 1.f) return;
	const float sx = std::max(0.01f, newW / oldW);
	const float sy = std::max(0.01f, newH / oldH);
	// 新组框中心：局部偏移绕 groupAngle 转回世界坐标
	const float scx = (start.left + start.right) * 0.5f, scy = (start.top + start.bottom) * 0.5f;
	const float mcx = (r.left + r.right) * 0.5f, mcy = (r.top + r.bottom) * 0.5f;
	const float ca = std::cos(groupAngle_), sa = std::sin(groupAngle_);
	const float offx = mcx - scx, offy = mcy - scy;
	const D2D1_POINT_2F newCenter{
		groupScaleStartCenter_.x + offx * ca - offy * sa,
		groupScaleStartCenter_.y + offx * sa + offy * ca };
	// 每次从按下前的几何重算：先按累计倍率以起始中心缩放，再整体平移到新中心。
	// 避免"围绕已移动过的中心做增量缩放"在非等比时产生漂移。
	for (size_t i = 0; i < groupTargets_.size() && i < groupBefore_.size(); ++i) {
		if (!groupBefore_[i].empty()) groupTargets_[i]->applyEditSnapshot(groupBefore_[i]);
	}
	for (auto* s : groupTargets_) {
		s->scaleBy(groupScaleStartCenter_.x, groupScaleStartCenter_.y, groupAngle_, sx, sy);
	}
	const D2D1_POINT_2F move{ newCenter.x - groupScaleStartCenter_.x,
		newCenter.y - groupScaleStartCenter_.y };
	for (auto* s : groupTargets_) s->translateBy(move.x, move.y);
	groupBox_ = D2D1::RectF(newCenter.x - newW * 0.5f, newCenter.y - newH * 0.5f,
		newCenter.x + newW * 0.5f, newCenter.y + newH * 0.5f);
	groupScaleLastCenter_ = newCenter;
	groupScaleLastSx_ = sx;
	groupScaleLastSy_ = sy;
	hasDragged = true;
	for (auto& s : history->shapes) if (s && !s->isUndo) s->syncLinks();
	refreshFiltersAbove(nullptr);
	refresh();
}

void AnnotHost::finishGroupScale()
{
	finishGroupTransform();
}

void AnnotHost::finishGroupTransform()
{
	if (!groupMoveActive_ && !groupRotateActive_ && !groupScaleActive_) return;
	std::vector<std::vector<float>> after;
	after.reserve(groupTargets_.size());
	for (auto* s : groupTargets_) after.push_back(s->editSnapshot());
	if (history) history->addEditSteps(groupTargets_, groupBefore_, after);
	for (auto* s : groupTargets_) s->transformSettled();
	groupTargets_.clear();
	groupBefore_.clear();
	groupMoveActive_ = false;
	groupRotateActive_ = false;
	groupScaleActive_ = false;
	groupScaleHandle_ = -1;
	annotDragIndex = -1;
	refreshFiltersAbove(nullptr);
	refresh();
}

// 清空标注列表的唯一入口：裸指针先归零再释放 shape，避免留下悬垂的 shapeHover。
// （工具条按钮的 onClick 里会读 shapeHover，一旦悬垂就是「点一下工具栏就闪退」）
void AnnotHost::clearAllShapes(bool keepBackground)
{
	if (editingText) editingText->finishEdit(); // 内部会置空 editingText / shapeHover
	annotMouseDown = false;
	annotDragIndex = -1;
	hasDragged = false;
	newShape = nullptr;
	pendingEditText = nullptr;
	pendingAngle = nullptr;
	pendingPen = nullptr;
	shapeHover = nullptr;
	extraSelection_.clear();
	groupBox_ = D2D1::RectF(0.f, 0.f, -1.f, -1.f);
	groupAngle_ = 0.f;
	groupScaleActive_ = false;
	marqueeActive_ = false;
	groupMoveActive_ = false;
	groupRotateActive_ = false;
	groupTargets_.clear();
	groupBefore_.clear();
	if (toolSub) toolSub->endMultiStyle();
	lastCreatedShape_ = nullptr;
	styleLoadCandidate = nullptr;
	linkBtn = nullptr;        // 连线叉指向的 shape 也要先归零，别留悬垂指针
	linkHoverTarget = nullptr;
	linkPressTarget = nullptr;
	if (history) {
		if (keepBackground) {
			auto& v = history->shapes;
			v.erase(std::remove_if(v.begin(), v.end(),
				[](const std::unique_ptr<ShapeBase>& s) {
					return !s || !s->backgroundLayer;
				}), v.end());
		}
		else {
			history->shapes.clear();
		}
		onHistoryChanged();
	}
	refresh();
}

bool AnnotHost::hasSelectedAnnot() const
{
	return shapeHover && !shapeHover->isUndo && !shapeHover->isViewportFilter();
}

ShapeBase* AnnotHost::liveStyleShape()
{
	if (shapeHover && !shapeHover->isUndo && !shapeHover->isViewportFilter())
		return shapeHover;
	if (!lastCreatedShape_ || !history) {
		lastCreatedShape_ = nullptr;
		return nullptr;
	}
	// 刚画完的形状可能已被撤销/删除：先在历史里确认指针仍有效，再判断是否仍属于当前工具
	bool alive = false;
	for (auto& s : history->shapes) {
		if (s.get() == lastCreatedShape_) { alive = true; break; }
	}
	if (!alive || lastCreatedShape_->isUndo
		|| toolIdOfShape(lastCreatedShape_) != styleToolId()) {
		lastCreatedShape_ = nullptr;
		return nullptr;
	}
	return lastCreatedShape_;
}

std::wstring AnnotHost::toolIdOfShape(ShapeBase* s)
{
	if (!s || s->isUndo || s->isViewportFilter()) return {};
	if (dynamic_cast<ShapeRect*>(s) || dynamic_cast<ShapeEllipse*>(s)) return L"rect";
	if (auto* line = dynamic_cast<ShapeLine*>(s))
		return line->isHighlighter() ? std::wstring{} : L"arrow";   // 荧光笔不回填滑条，避免宽度按倍数叠加
	if (dynamic_cast<ShapeArrow*>(s)) return L"arrow";
	if (dynamic_cast<ShapePen*>(s)) return L"pen";
	if (auto* t = dynamic_cast<ShapeText*>(s))
		return t->isFrame() ? L"arrow" : L"text";   // 气泡/爆裂属于「标注」栏
	if (dynamic_cast<ShapeNumber*>(s)) return L"number";
	if (dynamic_cast<ShapeMosaic*>(s)) return L"mosaic";
	if (dynamic_cast<ShapeHighlight*>(s)) return L"highlight";
	if (dynamic_cast<ShapePatina*>(s)) return L"patina";
	if (dynamic_cast<ShapeWatermark*>(s)) return L"watermark";
	return {};
}

void AnnotHost::presentSelectedAnnotStyle()
{
	if (!toolSub) return;
	if (!hasSelectedAnnot() || !shapeHover) {
		toolSub->clearSelectedNumberValue();
		toolSub->endMultiStyle();
		toolSub->endShapeStylePreview();
		return;
	}
	const auto cur = styleToolId();
	const auto sid = toolIdOfShape(shapeHover);
	// 多选：属性栏按"当前工具 + 形态"筛出目标集合，逐项比较后统一展示/置灰。
	if (hasMultiSelection()) {
		if (cur.empty() || !toolSub->chromeVisible()) {
			toolSub->clearSelectedNumberValue();
			toolSub->endMultiStyle();
			toolSub->endShapeStylePreview();
			return;
		}
		const auto targets = selectedShapesForTool(cur);
		if (targets.empty()) {
			toolSub->clearSelectedNumberValue();
			toolSub->endMultiStyle();
			toolSub->endShapeStylePreview();
			return;
		}
		toolSub->beginShapeStylePreview();
		toolSub->beginMultiStyle(targets);
		toolSub->clearSelectedNumberValue();
		return;
	}
	toolSub->endMultiStyle();
	// 属性栏未打开，或当前工具与该标注不匹配：不展示，也不修改工具状态。
	if (sid.empty() || cur.empty() || cur != sid || !toolSub->chromeVisible()) {
		toolSub->clearSelectedNumberValue();
		toolSub->endShapeStylePreview();
		return;
	}
	// 展示的是该标注的属性：先快照工具状态，取消选中时整体还原，
	// 确保标注自身的属性不会写入下一次落笔的默认值。
	toolSub->beginShapeStylePreview();
	shapeHover->syncToolFromShape();
	toolSub->refreshStyleToggles();
	toolSub->selectColorByValue(shapeHover->styleColorValue());
	// 序号：属性栏输入框显示选中标注的实际文字，编辑时回写它
	if (auto* num = dynamic_cast<ShapeNumber*>(shapeHover))
		toolSub->showSelectedNumberValue(num->displayLabel());
	else
		toolSub->clearSelectedNumberValue();
}

// 二次选中：将该标注的属性载入工具（下一个标注沿用这些属性），属性栏打开时同步展示。
// 仅由定时器 214 调用，避免在鼠标按下过程中重建属性栏。
void AnnotHost::applySelectedAnnotStyleToTool()
{
	if (!toolSub || !shapeHover) return;
	// 多选由 presentSelectedAnnotStyle 统一处理，不把某一个标注的样式载入工具默认值
	if (hasMultiSelection()) return;
	const auto sid = toolIdOfShape(shapeHover);
	if (sid.empty() || getCurToolId() != sid) {
		toolSub->endShapeStylePreview();
		return;
	}
	toolSub->beginShapeStylePreview();   // 取消选中时还原成载入前的默认值
	shapeHover->syncToolFromShape();
	if (!toolSub->chromeVisible()) return;
	toolSub->refreshStyleToggles();
	toolSub->selectColorByValue(shapeHover->styleColorValue());
	if (auto* num = dynamic_cast<ShapeNumber*>(shapeHover))
		toolSub->showSelectedNumberValue(num->displayLabel());
	else
		toolSub->clearSelectedNumberValue();
}

bool AnnotHost::shouldPaintAnnotHandles() const
{
	if (!shapeHover || shapeHover->isUndo || shapeHover->isViewportFilter()) return false;
	if (shapeHover == newShape) return false; // 落笔中
	// 编辑文字时也照常画控点：用户要求"选中即带控件"，编辑和选中是同一个视觉状态
	return true;
}

// editingText 是裸指针：撤销 / 重做等路径可能让它停留在已退出编辑状态的形状上。
// 该残留会持续抑制控点绘制（选框消失），并导致样式按钮继续修改该形状。
void AnnotHost::dropStaleEditingText()
{
	if (!editingText || editingText->isEditingNow()) return;
	editingText = nullptr;
	endTextEdit();
}

void AnnotHost::discardDraft()
{
	if (newShape && history) history->removeShape(newShape);
	newShape = nullptr;
	shapeHover = nullptr;
	extraSelection_.clear();
	groupBox_ = D2D1::RectF(0.f, 0.f, -1.f, -1.f);
	groupAngle_ = 0.f;
	groupScaleActive_ = false;
	marqueeActive_ = false;
	groupMoveActive_ = false;
	groupRotateActive_ = false;
	groupTargets_.clear();
	groupBefore_.clear();
	if (toolSub) toolSub->endMultiStyle();
	annotMouseDown = false;
	annotDragIndex = -1;
	ReleaseCapture();
	refresh();
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
	dropStaleEditingText();
	pruneAnnotSelection();
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
	// 临时范围提示属于控件层：画在标注之上，导出（withChrome=false）时不带
	if (shapeHover) shapeHover->paintHint(ctx);
	// 共用：仅「已选中且非落笔、非文字编辑」时画控点。
	// 多选时不再画单个标注的缩放控点，改画每个标注的虚线框 + 一个组选框/旋转柄。
	if (extraSelection_.empty()) {
		if (shouldPaintAnnotHandles())
			shapeHover->paintDragger(ctx);
	}
	else {
		paintSelectionChrome(ctx);
	}
	if (marqueeActive_) paintMarquee(ctx);
	paintSerialCursor(ctx);
	paintTextMidHint(ctx);
	paintLinkHints(ctx);
}

void AnnotHost::paintSelectionChrome(ID2D1DeviceContext* ctx)
{
	if (!ctx || !hasMultiSelection()) return;
	if (ShapeBase::rectEmpty(groupBox_)) syncGroupBoxFromSelection();
	if (ShapeBase::rectEmpty(groupBox_)) return;
	if (!brushGroupStroke) {
		ctx->CreateSolidColorBrush(D2D1::ColorF(0x34C759u), brushGroupStroke.GetAddressOf());
	}
	if (!brushGroupHandleFill) {
		ctx->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFFFFu), brushGroupHandleFill.GetAddressOf());
	}
	if (!brushGroupHandleShadow) {
		ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 64.f / 255.f),
			brushGroupHandleShadow.GetAddressOf());
	}
	if (!groupDashStyle) {
		float dashes[] = { 4.f, 3.f };
		auto factory = Ling::D2D::get()->d2dFactory;
		if (factory) {
			factory->CreateStrokeStyle(
				D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
					D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
				dashes, ARRAYSIZE(dashes), groupDashStyle.GetAddressOf());
		}
	}
	if (!brushGroupStroke) return;
	brushGroupStroke->SetColor(D2D1::ColorF(0x34C759u));
	const float cx = (groupBox_.left + groupBox_.right) * 0.5f;
	const float cy = (groupBox_.top + groupBox_.bottom) * 0.5f;
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	if (std::fabs(groupAngle_) > 1e-6f)
		ctx->SetTransform(D2D1::Matrix3x2F::Rotation(groupAngle_ * 180.f / 3.14159265f, { cx, cy }) * old);
	ctx->DrawRectangle(groupBox_, brushGroupStroke.Get(), 1.f * dpi, groupDashStyle.Get());
	ctx->SetTransform(old);

	// 严格跟随矩形控件：只画 4 个角柄（四边中点不画、但仍可拖），
	// 样式用和 ShapeBase::paintHandle 完全相同的"黑阴影 + 绿底 + 白描边"。
	const float l = groupBox_.left, t = groupBox_.top, r = groupBox_.right, b = groupBox_.bottom;
	const D2D1_POINT_2F corners[4]{ { l, t }, { r, t }, { r, b }, { l, b } };
	const float rOuter = 5.f * dpi;
	const float rInner = 4.f * dpi;
	for (const auto& lp : corners) {
		const auto p = ShapeBase::rotatePoint(lp.x, lp.y, cx, cy, groupAngle_);
		if (brushGroupHandleShadow)
			ctx->FillEllipse(D2D1::Ellipse(p, rOuter, rOuter), brushGroupHandleShadow.Get());
		ctx->FillEllipse(D2D1::Ellipse(p, rInner, rInner), brushGroupStroke.Get());
		if (brushGroupHandleFill)
			ctx->DrawEllipse(D2D1::Ellipse(p, rInner, rInner), brushGroupHandleFill.Get(), 1.5f * dpi);
	}
}

void AnnotHost::paintMarquee(ID2D1DeviceContext* ctx)
{
	if (!ctx || !marqueeActive_ || ShapeBase::rectEmpty(marqueeRect_)) return;
	if (!brushGroupStroke) {
		ctx->CreateSolidColorBrush(D2D1::ColorF(0x34C759u), brushGroupStroke.GetAddressOf());
	}
	if (!groupDashStyle) {
		float dashes[] = { 4.f, 3.f };
		auto factory = Ling::D2D::get()->d2dFactory;
		if (factory) {
			factory->CreateStrokeStyle(
				D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
					D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
				dashes, ARRAYSIZE(dashes), groupDashStyle.GetAddressOf());
		}
	}
	if (!brushGroupStroke) return;
	// 批量拖选框与标注选中框统一：1px 主题绿虚线，不加蓝色底/蓝边
	brushGroupStroke->SetColor(D2D1::ColorF(0x34C759u));
	ctx->DrawRectangle(marqueeRect_, brushGroupStroke.Get(), 1.f * dpi, groupDashStyle.Get());
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
	// 按"当前工具"校验一次：右键层级退出、切到别的工具这两条路径只 refresh、不重算提示，
	// 状态会一直挂着（表现为工具都取消了，挖空那块提示还留在画面上）
	if (textMidHintActive && getCurToolId() != L"text") {
		textMidHintActive = false;
		textMidHasLabel = false;
	}
	if (!textMidHintActive || textMidHasLabel || !ctx || !screenImg) return;
	// 就是"空白"本身：底图原地抠回来 → 底下的线在这一段等于不存在
	ctx->PushAxisAlignedClip(textMidHint, D2D1_ANTIALIAS_MODE_ALIASED);
	ctx->DrawBitmap(screenImg.Get(), D2D1::RectF(0.f, 0.f, w, h));
	ctx->PopAxisAlignedClip();
}

// 连线的提示（只在箭头工具下出现）：
//   · 悬停形状时：在"朝鼠标方向"的边缘点画一个和控点同款的小圆点 + 一句"拖动可连线"
//     —— 这就是**起点**提示：必须从这个圆点上起笔才会联动，从形状内部别处起笔是普通操作；
//   · 拖动连线中：点**一直待在起点**上（起点贴着边缘随鼠标方向滑，点跟着滑）。
//     拖进目标形状时终点会吸附，但点不会跟到目标上去 —— 用户要的：目标不需要点提示；
//   · 线中点上的**叉**：点它直接删掉这条箭头/线段，只在没拖动时出现。
// 文本工具停在线中间是"这儿能打字"（updateTextMidHint），两个热区都在中点，
// 不能让它们抢同一处，所以这些提示都不在文本工具下出现
void AnnotHost::updateLinkHints(const POINT& imgPos, const std::wstring& tool)
{
	// 叉的显示范围：箭头工具 + 空工具（选择态也能点掉一条连线）
	const bool toolOk = (tool == L"arrow" || tool.empty());
	// 起点"＋"提示只属于箭头工具：空工具（选择态）里按住矩形是把它拖走，拖不出连线，
	// 再显示"拖动可连线"就是误导
	const bool dotOk = (tool == L"arrow");
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
	else if (dotOk) {
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
			s->syncLinks();                 // 位置按最新的形状几何计算（形状消失时自动解除关联）
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
	// 再按"当前工具"校验一次：右键层级退出、切到别的工具都只 refresh、不重算提示，
	// 状态会一直挂着（表现为工具都取消了，"＋"和"拖动可连线"还留在画面上）
	if (linkShowDot && getCurToolId() != L"arrow") {
		linkShowDot = false;
		linkHoverTarget = nullptr;
	}
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
	ScreenToClient(annotInputHwnd(), &client);
	// 用宿主窗口的客户区判断，不能用 w/h：
	// 图片编辑器的 w/h 是画板像素、客户区还包含左右栏和缩放偏移；空画板时 w/h 甚至为 0，
	// 会把跟手预览整个裁掉，而系统光标已经被隐藏 → 看起来就是"鼠标不见了"。
	RECT clientRect{};
	if (!GetClientRect(annotInputHwnd(), &clientRect)) return;
	if (client.x < 0 || client.y < 0 || client.x >= clientRect.right || client.y >= clientRect.bottom) return;

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
		ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.9f), brush.GetAddressOf());
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
	ctx->CreateSolidColorBrush(fillC, fill.GetAddressOf());
	ctx->CreateSolidColorBrush(borderC, border.GetAddressOf());
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

void AnnotHost::requestEditText(ShapeText* shape)
{
	pendingEditText = shape;
	// 1ms：排在当前 mouseUp/annotUp 消息之后，让新建形状先按普通标注完成这一帧的绘制
	setTimer(1, 213);
}

void AnnotHost::beginTextEdit()
{
	// TSF 输入法（Win11 微软拼音）不一定会发 WM_IME_STARTCOMPOSITION：
	// 进编辑时先把组字/候选框摆到插入符，别等组字开始才摆。
	positionImeWindow();
	// 插入符闪烁：定时刷新画布
	setTimer(500, 212);
}

void AnnotHost::endTextEdit()
{
	killTimer(212);
}

void AnnotHost::annotChar(UINT ch)
{
	if (!editingText) return;
	// WM_CHAR 已经过键盘布局/IME 转换；控制字符由 annotKey 处理
	if (ch >= 32 && ch != 0x7F) {
		const wchar_t buf[2]{ (wchar_t)ch, 0 };
		editingText->insertText(buf);
		positionImeWindow();
	}
}

void AnnotHost::positionImeWindow()
{
	if (!editingText || !hwnd || !editingText->isEditingNow()) return;
	HIMC himc = ImmGetContext(annotInputHwnd());
	if (!himc) return;
	float lineH = 0.f;
	const auto p = editingText->imeCaretPos(lineH);
	const auto c = annotToClientPt(p.x, p.y);
	const float sc = scale > 0.001f ? scale : 1.f;
	const float caretH = (std::max)(1.f, lineH * sc);
	const float caretW = (std::max)(1.f, 2.f * dpi);
	const float maxX = (std::max)(0.f, w - 1.f);
	const float maxY = (std::max)(0.f, h - 1.f);
	// 微软拼音的组字窗内部自带约 8~10px 左内边距：向左让出这段，组字文字才和插入符对齐。
	// 自绘文字没有"行内组字"能力，组字窗只能作为一个悬浮框：放在**整行下面**，
	// 不能盖住正在编辑的标注文字（用户要的是"候选栏在文字下面"）。
	const LONG compX = (LONG)std::lround(std::clamp(c.x - 2.f * dpi, 0.f, maxX));
	const LONG compY = (LONG)std::lround(std::clamp(c.y + caretH - 12.f * dpi, 0.f, maxY));
	COMPOSITIONFORM comp{};
	comp.dwStyle = CFS_FORCE_POSITION;
	comp.ptCurrentPos = { compX, compY };
	ImmSetCompositionWindow(himc, &comp);
	CANDIDATEFORM cand{};
	cand.dwStyle = CFS_EXCLUDE;
	cand.ptCurrentPos = { compX, compY };
	cand.rcArea = { compX, compY, compX + (LONG)std::lround(caretW), compY + (LONG)std::lround(caretH) };
	ImmSetCandidateWindow(himc, &cand);
	ImmReleaseContext(annotInputHwnd(), himc);
}

void AnnotHost::syncViewportFilters()
{
	ensureHistory();
	history->syncViewportFilters(getCurToolId());
}

void AnnotHost::onToolStyleChanged()
{
	onToolChanged();
	dropStaleEditingText();
	// 多选：按"一致属性才下发"的规则批量应用，混合属性保留各标注自己的值
	if (!editingText && toolSub && toolSub->multiStyleActive()) {
		toolSub->applyMultiStyle();
		refreshFiltersAbove(nullptr);
		refresh();
		return;
	}
	// 二次选中或编辑中的标注：只有「选中的标注与当前工具同类」时才把属性栏样式下发。
	// 否则会串台：例如选中序号时点矩形工具的虚线，矩形线宽会被当成序号字号写进去。
	const auto curTool = getCurToolId();
	if (editingText) {
		if (toolIdOfShape(editingText) == curTool) editingText->applyStyle();
	}
	else if (shapeHover && !shapeHover->isViewportFilter() && !curTool.empty()
		&& toolIdOfShape(shapeHover) == curTool) {
		shapeHover->applyStyle();
	}
	// 视口滤镜（包浆/水印）没有选中态：样式一变就整体重刷
	if (history) {
		for (auto& s : history->shapes) {
			if (s->isUndo) continue;
			if (auto* p = dynamic_cast<ShapePatina*>(s.get())) p->applyStyle();
			else if (auto* w = dynamic_cast<ShapeWatermark*>(s.get())) w->applyStyle();
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
	else if (tool == L"eraser") SetCursor(LoadCursor(nullptr, IDC_HAND)); // 橡皮用抓手光标
	else if (tool == L"watermark" || tool == L"patina" || tool == L"opacity") SetCursor(LoadCursor(nullptr, IDC_ARROW));
	else if (tool == L"number") {
		numberCursorFollow = true;
		SetCursor(nullptr); // 跟手圆/表情即光标
	}
	else SetCursor(LoadCursor(nullptr, IDC_CROSS));
}

ShapeBase* AnnotHost::probeShapeUnder(float x, float y, ShapeBase* preferFirst)
{
	if (!history) return nullptr;
	const bool bgEdit = boardBackgroundEditing();
	if (preferFirst && !preferFirst->isUndo && !preferFirst->isViewportFilter()
		&& !preferFirst->isEphemeral()
		&& (!preferFirst->backgroundLayer || bgEdit)) {
		preferFirst->mouseMove(x, y);
		if (preferFirst->hoverDraggerIndex >= 0) return preferFirst;
	}
	for (int i = (int)history->shapes.size() - 1; i >= 0; i--) {
		auto* cur = history->shapes[i].get();
		if (cur->isUndo || cur->isViewportFilter() || cur->isEphemeral() || cur == preferFirst) continue;
		// 背景层标注在关闭「编辑背景」时不可编辑：不参与悬停命中，也就不会变成手型
		if (cur->backgroundLayer && !bgEdit) continue;
		cur->mouseMove(x, y);
		if (cur->hoverDraggerIndex >= 0) return cur;
	}
	return nullptr;
}

void AnnotHost::applyAnnotCursor()
{
	numberCursorFollow = false;
	auto groupHandleCursor = [this](int h) {
		// 与矩形控件统一：上下边给上下箭头、左右边给左右箭头，只有四角给对角箭头。
		switch (h) {
		case 0: case 4: SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); break;
		case 2: case 6: SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); break;
		case 1: case 5: SetCursor(LoadCursor(nullptr, IDC_SIZENS)); break;
		case 3: case 7: SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); break;
		default: SetCursor(LoadCursor(nullptr, IDC_ARROW)); break;
		}
	};
	if (groupRotateActive_) {
		if (HCURSOR c = AnnotCursor::rotate(0, groupAngle_)) SetCursor(c);
		else SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		return;
	}
	if (groupScaleActive_) { groupHandleCursor(groupScaleHandle_); return; }
	if (groupMoveActive_) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return; }
	if (marqueeActive_) { SetCursor(LoadCursor(nullptr, IDC_CROSS)); return; }
	// 文本工具停在线段中间那块"空白"里：一律工字型（比"悬停到已有图形"的移动/缩放光标优先，
	// 不然在箭头上悬停时会被箭头自己的光标盖掉，看着就不像能打字）
	if (textMidHintActive && !annotMouseDown) {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
		return;
	}
	// 光标必须在这里统一给出（而不是只在 annotMove 末尾压一次）：光标有 WM_SETCURSOR 与
	// WM_MOUSEMOVE 两条设置路径，两处给的不一致时就会轮流 SetCursor，光标来回闪 ——
	// 箭头工具在**空心矩形内部**移动时闪，就是这个原因。
	if (!annotMouseDown) {
		// 停在连线的"断开叉"上：手型，提示可点（那个是真能点掉的）
		if (linkBtn) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return;
		}
		// 停在可连形状的"＋"柄范围内：给**工具光标**（箭头工具=十字），不是手型 ——
		// 按住那里＝起笔画连线，连线也是"画标注"，跟别的工具一样是十字（用户定的）。
		// 只对箭头工具生效：空工具（选择态）停在标注上仍给手型，那是"能点它"的提示。
		// 排在下面"未选中标注手型"之前，形状边缘那圈才不会被手型抢走
		if (linkHoverTarget && getCurToolId() == L"arrow") {
			applyToolDrawCursor();
			return;
		}
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
	ScreenToClient(annotInputHwnd(), &pos);
	const auto img = toImgPos(pos);
	const float ix = (float)img.x, iy = (float)img.y;

	// 多选：和普通矩形控件一样 —— 8 个柄给缩放光标、角外圈给旋转、框内给移动
	if (!annotMouseDown && hasMultiSelection()) {
		if (ShapeBase::rectEmpty(groupBox_)) syncGroupBoxFromSelection();
		const int handle = hitGroupHandle(ix, iy);
		if (handle >= 0) { groupHandleCursor(handle); return; }
		const int rot = hitGroupRotateCorner(ix, iy);
		if (rot >= 0) {
			if (HCURSOR c = AnnotCursor::rotate(rot, groupAngle_)) SetCursor(c);
			else SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
			return;
		}
		if (groupBoxContains(ix, iy)) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return; }
	}

	// 已选中：边缘缩放 / 区内移动（按住 Alt 时让位给落笔，见 altForceDraw）
	if (!altForceDraw() && shapeHover && !shapeHover->isUndo && shapeHover != newShape
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
	if (!annotSkipHitSelect() && !altForceDraw()) {
		if (auto* under = probeShapeUnder(ix, iy, nullptr)) {
			// 尺寸工具：矩形仅作为底图，不参与悬停与点选交互。
			const bool rectIgnored = dimensionToolActive() && dynamic_cast<ShapeRect*>(under);
			if (under != shapeHover && !textToolIgnoresShape(getCurToolId(), under) && !rectIgnored) {
				under->setCursor();
				return;
			}
		}
	}

	applyToolDrawCursor();
}

void AnnotHost::setSizePreviewHold(bool hold, bool shapePreview)
{
	if (hold) {
		const bool kindChanged = (sizePreviewShape != shapePreview);
		sizePreviewShape = shapePreview;
		if (sizePreviewHold && kindChanged) refresh(); // 两个属性栏之间移动时预览类型要当场切换
	}
	if (sizePreviewHold == hold) return;
	sizePreviewHold = hold;
	refresh();
}

// 二次选中"当前工具"的标注时，滑条/滚轮改的就是本体（粗细分、边数、圆角、字号…），
// 画布上再画一个独立预览是多余的 —— 统一在这里判定，主栏与二级栏共用
bool AnnotHost::sizePreviewSuppressed() const
{
	// 气泡/爆裂正在编辑文字时，滚轮/滑条改的是它自己的描边，直接看本体即可
	if (editingText && editingText->isFrame()) return true;
	return hasSelectedAnnot() && toolIdOfShape(shapeHover) == styleToolId();
}

void AnnotHost::pulseSizePreview(bool shapePreview)
{
	sizePreviewShape = shapePreview;
	sizePreviewPulseUntil = GetTickCount64() + 700;
	setTimer(700, 210);
	refresh();
}

void AnnotHost::annotTimer(UINT id)
{
	if (id == 214) {
		killTimer(214);
		applySelectedAnnotStyleToTool();
		return;
	}
	if (id == 212) {
		if (editingText) {
			refresh();
			setTimer(500, 212);
		}
		else killTimer(212);
		return;
	}
	if (id == 213) {
		killTimer(213);
		ShapeText* s = pendingEditText;
		pendingEditText = nullptr;
		if (!s || !history) return;
		bool alive = false;
		for (auto& sp : history->shapes) {
			if (sp.get() == s && !sp->isUndo) { alive = true; break; }
		}
		if (alive) s->startEdit();
		return;
	}
	if (id == 210) {
		killTimer(210);
		sizePreviewPulseUntil = 0;
		refresh();
		return;
	}
	if (id == 211) {
		// 渐隐画笔：按点寿命重绘，整段过期后移除；绘制中同样需要刷新
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
		if (still) setTimer(15, 211);
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
		|| id == L"watermark" || id == L"highlight" || id == L"number"
		|| id == L"opacity") return;
	// 二次选中本工具的标注：变化直接出现在本体上，不再叠预览
	if (sizePreviewSuppressed()) return;

	float cx = 0.f, cy = 0.f, half = 40.f;
	if (!sizePreviewOrigin(cx, cy, half)) return;
	const float x1 = cx - half;
	const float x2 = cx + half;

	// 整段预览用层透明度，避免杆身与箭头交接处叠色发暗
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

	// 矩形系：调整边数、角数、圆角时画形状本身；调粗细仍用统一的笔迹预览
	if (id == L"rect" && sizePreviewShape) {
		// 刚画完的星形/多边形也在实时跟着滑条变，画布上不再叠一个重复预览
		if (auto* live = dynamic_cast<ShapeRect*>(liveStyleShape())) {
			const int want = toolSub->rectKind == ToolSub::StarShape ? ShapeRect::Star
				: toolSub->rectKind == ToolSub::PolygonShape ? ShapeRect::Polygon
				: ShapeRect::Rect;
			if (live->kind() == want) { pop(); return; }
		}
		const float polyStroke = toolSub->getSliderVal();
		const D2D1_RECT_F pr{ cx - half * 0.86f, cy - half * 0.86f, cx + half * 0.86f, cy + half * 0.86f };
		if (toolSub->rectKind == ToolSub::RectShape) {
			const float rr = toolSub->cornerRadius / 100.f
				* std::min(pr.right - pr.left, pr.bottom - pr.top) * 0.5f;
			const auto rounded = D2D1::RoundedRect(pr, rr, rr);
			if (toolSub->isRectFill) {
				ctx->FillRoundedRectangle(rounded, brush.Get());
			}
			else {
				auto dash = toolSub->isRectDash ? ShapeRect::makeDashStroke(polyStroke) : nullptr;
				ctx->DrawRoundedRectangle(rounded, brush.Get(), polyStroke, dash.Get());
			}
		}
		else {
			const bool star = toolSub->rectKind == ToolSub::StarShape;
			const int sides = star ? toolSub->starPoints : toolSub->polygonSides;
			auto geo = ShapeRect::buildPolyGeometry(pr,
				star ? ShapeRect::Star : ShapeRect::Polygon, sides, toolSub->cornerRadius);
			if (geo) {
				if (toolSub->isRectFill) {
					ctx->FillGeometry(geo.Get(), brush.Get());
				}
				else {
					auto dash = toolSub->isRectDash ? ShapeRect::makeDashStroke(polyStroke) : nullptr;
					ctx->DrawGeometry(geo.Get(), brush.Get(), polyStroke, dash.Get());
				}
			}
		}
		pop();
		return;
	}

	// 所有工具的粗细预览统一成一条手绘风格的 S 形曲线：
	// 比直线更容易看出笔迹的粗细、平滑与端头形态
	float stroke = std::max(1.f, toolSub->getSliderVal());
	bool dash = false;
	bool soft = false;
	if (id == L"pen") {
		if (toolSub->isPenMarker) stroke *= ShapeLine::HighlighterWidthScale;
		soft = toolSub->isPenSoft && !toolSub->isPenFade;
		dash = toolSub->isPenDash;
	}
	else if (id == L"arrow") {
		dash = toolSub->isArrowDash;
	}
	else if (id == L"rect") {
		dash = toolSub->isRectDash;
	}
	// 端头跟随实际工具：画笔本身是圆头，箭头/线段看「圆头」开关，闭合形状按平头
	const bool roundCap = (id == L"pen") || (id == L"arrow" && toolSub->isArrowRound);

	// 幅度控制得小一些：跨度够长、起伏平缓，接近手写的一笔
	const float amp = std::clamp(std::max(half * 0.18f, stroke * 0.6f), 6.f, half * 0.55f);
	std::vector<D2D1_POINT_2F> pts;
	std::vector<int64_t> times;
	constexpr int kSamples = 32;
	pts.reserve(kSamples + 1);
	times.reserve(kSamples + 1);
	for (int i = 0; i <= kSamples; ++i) {
		const float t = (float)i / (float)kSamples;
		pts.push_back({ x1 + (x2 - x1) * t, cy - std::sin(t * kPiPrev * 2.f) * amp });
		// 假时间：两端慢、中间快，软笔预览能看出粗细变化
		const float speed = 0.25f + 1.2f * std::sin(t * kPiPrev);
		times.push_back(times.empty() ? 0 : times.back() + (int64_t)std::max(4.f, 18.f / speed));
	}

	ComPtr<ID2D1Factory> factory = Ling::D2D::get()->d2dFactory;
	ComPtr<ID2D1StrokeStyle> style;
	const float sw = std::max(stroke, 1.f);
	const auto cap = roundCap ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT;
	if (dash) {
		float dashes[] = { 8.f / sw, 6.f / sw };
		factory->CreateStrokeStyle(
			D2D1::StrokeStyleProperties(cap, cap, cap,
				D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
			dashes, ARRAYSIZE(dashes), style.GetAddressOf());
	}
	else {
		factory->CreateStrokeStyle(
			D2D1::StrokeStyleProperties(cap, cap, cap, D2D1_LINE_JOIN_ROUND),
			nullptr, 0, style.GetAddressOf());
	}

	if (!dash && soft && roundCap) {
		// 软笔：只有填充轮廓能体现速度造成的粗细变化
		auto geo = FreehandStroke::outline(factory.Get(), pts, stroke, true, times);
		if (geo) ctx->FillGeometry(geo.Get(), brush.Get());
	}
	else {
		ComPtr<ID2D1PathGeometry> path;
		factory->CreatePathGeometry(path.GetAddressOf());
		ComPtr<ID2D1GeometrySink> sink;
		path->Open(sink.GetAddressOf());
		sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
		for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(pts[i]);
		sink->EndFigure(D2D1_FIGURE_END_OPEN);
		sink->Close();
		ctx->DrawGeometry(path.Get(), brush.Get(), stroke, style.Get());
	}
	pop();
}

bool AnnotHost::dimensionToolActive() const
{
	if (!toolSub) return false;
	// 图片编辑器：独立「尺寸」工具 id；截图/标注栏：arrow + 当前线型是尺寸
	if (getCurToolId() == L"dimension") return true;
	if (getCurToolId() != L"arrow") return false;
	return toolSub->isLine && toolSub->lineKind == ToolSub::LineDimension;
}

bool AnnotHost::penPendingAlive() const
{
	if (!pendingPen || !history) return false;
	for (auto& s : history->shapes) {
		if (s.get() == pendingPen && !s->isUndo) return true;
	}
	return false;
}

void AnnotHost::finalizePendingPen()
{
	if (!pendingPen) return;
	ShapeBase* shape = pendingPen;
	pendingPen = nullptr;
	shape->penClearPreview();
	// 只落了一个锚点时不保留退化形状
	if (shape->penPointCount() < 2 && history) history->removeShapeGroup(shape);
	refresh();
}

// 右键收笔：不追加锚点，直接以最后一次单击落下的锚点结束。
// 若按右键位置落点，末段方向会跟着右键位置随机改变，收笔处容易多出一段弯钩。
bool AnnotHost::finishPenStroke(const POINT& pos)
{
	(void)pos;
	if (!pendingPen) return false;
	if (!penPendingAlive()) {
		pendingPen = nullptr;
		return false;
	}
	ShapeBase* shape = pendingPen;
	pendingPen = nullptr;
	shape->penClearPreview();
	if (shape->penPointCount() < 2 && history) history->removeShapeGroup(shape);
	refresh();
	applyAnnotCursor();
	return true;
}

bool AnnotHost::annotDown(POINT pos, BOOL isRight)
{
	ensureHistory();
	dropStaleEditingText();
	// 序号输入框的常态编辑：点画布就退出。左键不吞（照常落笔/选中），右键只收编辑
	if (toolSub && toolSub->numberEditActive()) {
		toolSub->endNumberEdit();
		if (isRight) return true;
	}
	if (toolSub) toolSub->blurInputs();
	editStepTarget = nullptr;
	editStepBefore.clear();
	// 双击判定提前算：编辑文字时"再次双击 = 全选"也要用它（见下面的 editingText 分支）。
	// 时间戳只给左键记 —— 右键是"逐层退出"，不该打断双击序列
	POINT screenPos{};
	GetCursorPos(&screenPos);
	const auto now = GetTickCount64();
	const bool isDblClick = (now - annotLastDownTime <= GetDoubleClickTime())
		&& std::abs(screenPos.x - annotLastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
		&& std::abs(screenPos.y - annotLastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
	// 编辑中点到文字框内：仍然在编辑 —— 单击把插入符挪到点击处，双击（未全选时）全选。
	// 以前只有"插入符那一小条"不退出，点别处一律提交退出：想改一段话中间的一个字，
	// 只能把整段重打，用起来是割裂的（文本编辑器的公认手感是点哪插哪）。
	if (editingText) {
		const auto ip = toImgPos(pos);
		if (editingText->hitLabelBox((float)ip.x, (float)ip.y)) {
			// 已经全选时不再按双击处理：双击进编辑后紧跟的那一下单击要落插入符，
			// 否则这一下等于什么都没发生（用户看到的就是"点了没反应"）
			if (isDblClick && !editingText->selectAll_) editingText->selectAllText();
			else editingText->placeCaretAt((float)ip.x, (float)ip.y);
			positionImeWindow();
			// 这一下也记进双击序列：编辑中连点两下（双击）要能被识别成全选
			annotLastDownTime = now;
			annotLastDownPos = screenPos;
			return true;
		}
		editingText->finishEdit(false);
	}
	// finishEdit 可能因失焦先于本事件完成；吞掉紧随其后的点选/新建
	if (GetTickCount64() < annotSuppressUntil) {
		annotSuppressUntil = 0;
		clearAnnotSelection();
		return true;
	}
	if (isRight) {
		// 钢笔式笔画还没收笔：右键先把它收掉，不进入"逐层退出"
		if (finishPenStroke(pos)) return true;
		return annotRightClick();
	}
	annotLastDownTime = now;
	annotLastDownPos = screenPos;

	// 钢笔式点按：后续左键继续落锚点（Shift = 最近一段直线），不进入拖拽/点选
	if (pendingPen) {
		if (penPendingAlive() && getCurToolId() == L"arrow") {
			const auto penImg = toImgPos(pos);
			pendingPen->penAddPoint((float)penImg.x, (float)penImg.y,
				(GetKeyState(VK_SHIFT) & 0x8000) != 0);
			annotMouseDown = false;
			refresh();
			applyAnnotCursor();
			return true;
		}
		finalizePendingPen();
	}

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

	// 双击落在线段"中间热区"（提示挖空那块）时，语义是"在这条线上打字"，不能走下面的
	// "双击切端点 free" —— 它会把这个双击吃掉（日志里 shaft=1 shaftDist=3.5 dbl=1 却
	// 插不进光标就是这个）。用悬停提示标志判断，它在按下前已经算好了；跳过这里之后，
	// 下面通用路径会自己处理"已有文字就编辑、没有就落笔"
	if (isDblClick && !editingText && !textMidHintActive) {
		// 双击是"针对某一个标注"的动作：多选先收成单选
		extraSelection_.clear();
		// 先判断"这一下双击是不是落在线上文字上"再决定撤不撤上一次落笔 ——
		// 顺序反了的话，双击箭头里的文字会先把箭头（或文字）undo 掉，表现就是
		// "重复编辑箭头里的文字，箭头整个不见了，只剩文字"
		const auto imgDbl = toImgPos(pos);
		const float dx = (float)imgDbl.x, dy = (float)imgDbl.y;
		// 找"双击点所在的线上文字"：命中线的（文字是整体的一部分）和正好选中文字的
		// 两种都要认 —— 尺寸/角度自带的文字都靠这条路进编辑，绝不能被 undo 吃掉
		auto labelUnder = [&](ShapeBase* host) -> ShapeText* {
			if (!host || !history) return nullptr;
			// 命中的直接就是文字/气泡：双击就是进编辑（不能先被 undo 掉）
			if (auto* t = dynamic_cast<ShapeText*>(host))
				return t->hitLabelBox(dx, dy) ? t : nullptr;
			for (auto& s : history->shapes) {
				auto* t = dynamic_cast<ShapeText*>(s.get());
				if (!t || s->isUndo || t->attachTo != host) continue;
				// 文字框是转过角度的（竖排/斜排），命中要按文字自己的朝向判
				if (t->hitLabelBox(dx, dy)) return t;
			}
			return nullptr;
		};
		ShapeText* dblText = labelUnder(shapeHover);
		if (!dblText) dblText = labelUnder(hitShapeAt(dx, dy));
		const bool dblLabel = (dblText != nullptr);
		if (prevPressCreatedShape && history && !dblLabel) history->undo();
		// 双击线段上的文字（文字是线段的一部分）：直接进文字编辑，
		// 不用先单独选中文字；线（以及尺寸/角度）必须原样留着
		if (dblText) {
			shapeHover = dblText;
			setEditingText(dblText);
			// 文字：双击即全选；气泡/爆裂：先落插入符（它们双击后一般还要补字）
			dblText->startEdit(!dblText->isFrame());
			annotMouseDown = false;
			return true;
		}
		// 双击箭头/线段端点切换 free（绕对端旋转 ↔ 自由拖点）
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
				// 同一条规则：文字双击即全选，气泡/爆裂先落插入符
				txt->startEdit(!txt->isFrame());
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
	SetCapture(annotInputHwnd());

	const auto tool = getCurToolId();
	auto imgPos = toImgPos(pos);
	const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

	// 多选：组旋转柄优先；按在任一已选标注上 = 整组移动。空工具才有多选手势。
	if (tool.empty() && hasMultiSelection()) {
		if (ShapeBase::rectEmpty(groupBox_)) syncGroupBoxFromSelection();
		const float ix = (float)imgPos.x, iy = (float)imgPos.y;
		// 和普通矩形控件同一套：先命中 8 个缩放柄，再判角外旋转圈，最后框内移动
		const int handle = hitGroupHandle(ix, iy);
		if (handle >= 0) {
			beginGroupScale(handle, imgPos);
			return true;
		}
		if (hitGroupRotateCorner(ix, iy) >= 0) {
			beginGroupTransform(true, imgPos);
			return true;
		}
		if (auto* hit = hitShapeAt(ix, iy); hit && isShapeSelected(hit)) {
			// 多选下 Ctrl/Shift+点已选标注：从选择里去掉，不进入整组拖动
			if (ctrlDown || shiftDown) {
				toggleShapeInSelection(hit);
				annotMouseDown = false;
				ReleaseCapture();
				return true;
			}
			beginGroupTransform(false, imgPos);
			return true;
		}
		else if (hit && (ctrlDown || shiftDown)) {
			// 组框内部压着的未选标注：Ctrl/Shift+点仍可加选
			toggleShapeInSelection(hit);
			annotMouseDown = false;
			ReleaseCapture();
			return true;
		}
		if (groupBoxContains(ix, iy)) {
			beginGroupTransform(false, imgPos);
			return true;
		}
	}

	// 角度三点式：上一笔没画完时，下一次点击/拖拽继续给这条角度（点、拖都行）
	if (pendingAngle) {
		bool alive = false;
		if (history) {
			for (auto& s : history->shapes)
				if (s.get() == pendingAngle && !s->isUndo) { alive = true; break; }
		}
		if (!alive) pendingAngle = nullptr;
		else if (tool != L"arrow" || !toolSub || !toolSub->isLine
			|| toolSub->lineKind != ToolSub::LineAngle) {
			// 换工具了：放弃衔接；只落了一个点的退化形状直接删掉
			if (pendingAngle->anglePointCount() < 2) history->removeShape(pendingAngle);
			pendingAngle = nullptr;
		}
		else {
			newShape = pendingAngle;
			shapeHover = pendingAngle;
			shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
			// 第三点一落就显示角度：文字先挂上，拖动中由 updateAngleLabel 实时改写
			if (pendingAngle->angleComplete())
				ensureShaftText(pendingAngle, pendingAngle->angleLabelText(), pendingAngle->angleLabelFontPx());
			lockAnnotDragFromHover();
			refresh();
			return true;
		}
	}

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

	// 按下时先命中已有标注（空工具也能点选），未命中才落笔
	// 录屏工具启用时跳过点选，直接落笔，避免标注过程中拖到旧图形
	// 文本工具落在线段中间时要例外：那儿就是要打字，不能被"命中已有箭头"抢走
	const bool textOnShaft = (shaftHost != nullptr);
	// 连线：箭头/线段工具必须从形状**边缘的锚点圆点**起笔才联动（见 updateLinkHints）。
	// 形状内部其它位置不再触发连线 —— 那些位置仍是普通的点选/移动，避免误连。
	// 没拖就抬手（就是单击）见 annotUp：空线删掉、改成选中那个形状。
	bool linkFromShape = false;
	linkPressTarget = nullptr;
	if (tool == L"arrow" && !annotSkipHitSelect() && !textOnShaft) {
		if (ShapeBase* t = ShapeLink::pickTarget(this, (float)imgPos.x, (float)imgPos.y);
			t && !(dimensionToolActive() && dynamic_cast<ShapeRect*>(t))) {
			D2D1_POINT_2F dot{};
			// 锚点热区与画出来的小圆点同口径（略放大一点便于命中）
			if (ShapeLink::edgePointToward(t, (float)imgPos.x, (float)imgPos.y, dot)
				&& std::hypot((float)imgPos.x - dot.x, (float)imgPos.y - dot.y) <= 9.f * dpi) {
				linkFromShape = true;
				linkPressTarget = t;
			}
		}
	}
	if (!annotSkipHitSelect() && !textOnShaft && !linkFromShape && !altForceDraw()) {
		if (auto* hit = hitShapeAt((float)imgPos.x, (float)imgPos.y)) {
			// 文本工具：马赛克这类"整块铺满的背景"不抢这一下，直接往下走"落笔造文字"，
			// 于是"先马赛克、后文字"能正常叠起来（文字压在马赛克上）。
			// 尺寸工具：矩形仅作为底图，不抢占此次操作，直接落笔画尺寸线。
			const bool rectIgnored = dimensionToolActive() && dynamic_cast<ShapeRect*>(hit);
			if (!textToolIgnoresShape(tool, hit) && !rectIgnored) {
				newShape = nullptr;
				const bool wasSelected = isShapeSelected(hit);
				if (!wasSelected) {
					// 空工具 + Ctrl/Shift：加选/减选，不进入拖动
					if (tool.empty() && (ctrlDown || shiftDown)) {
						toggleShapeInSelection(hit);
						annotMouseDown = false;
						ReleaseCapture();
						return true;
					}
					extraSelection_.clear();
					if (auto* a = dynamic_cast<ShapeArrow*>(shapeHover)) a->resetFree();
					else if (auto* l = dynamic_cast<ShapeLine*>(shapeHover)) l->resetFree();
					shapeHover = hit;
					// 记录候选：抬手且确认是点选而非拖动后，再载入属性。
					styleLoadCandidate = hit;
					refresh();
				}
				// 记录编辑前几何：本次若拖动/缩放/旋转，抬手时作为独立一步进撤销栈
				editStepTarget = hit;
				editStepBefore = hit->editSnapshot();
				if (wasSelected) {
					shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
					lockAnnotDragFromHover();
				}
				else {
					// 首次点选：无论按下位置，均按整体移动处理；
					// 拖边与缩放需在松开后重新将指针停在边缘（此时标注已选中）再次按下。
					hit->hoverDraggerIndex = 8;
					hit->mouseDown((float)imgPos.x, (float)imgPos.y);
					lockAnnotDragFromHover();
				}
				return true;
			}
		}
	}

	// 有工具时保留二次选中载入的样式供新标注使用；无工具时还原默认值。
	clearAnnotSelection(!tool.empty());
	newShape = nullptr;

	if (tool.empty()) {
		// 贴图挪窗 / 长图平移等宿主手势优先；没人接管且有标注可框时才开始框选。
		// 画布上没有任何标注 → 保持原行为（不起框）。
		if (annotEmptyToolDrag(pos)) return true;
		if (hasSelectableShapes()) {
			beginMarquee(imgPos);
			return true;
		}
		annotMouseDown = false;
		ReleaseCapture();
		return true;
	}

	// 文本工具落在线段中间：把文字挂到这条箭头/线段上（之后跟着它移动，见 paintShapes）
	// 落笔点使用鼠标位置，保持所见即所得
	shapeHover = history->createShape(tool, imgPos.x, imgPos.y);
	newShape = shapeHover;
	if (toolSub) {
		// 样式已实际应用于新标注：采纳该样式，不再回退；未创建成功则还原。
		if (shapeHover) toolSub->commitShapeStylePreview();
		else toolSub->endShapeStylePreview();
	}
	// 落笔位置正好在线段中间：给文字开洞（线断在字两边）+ 挂到线上（始终中心对齐、一律横排）
	if (shapeHover && shaftHost) {
		if (auto* t = dynamic_cast<ShapeText*>(shapeHover)) {
			t->punchBg = true;
			t->attachTo = shaftHost;
			float mx = shaftMx, my = shaftMy;   // 用扫描时记下的中点（不再重算）
			if (auto* a = dynamic_cast<ShapeArrow*>(shaftHost)) { a->shaftMidPoint(mx, my); }
			else if (auto* l = dynamic_cast<ShapeLine*>(shaftHost)) { l->shaftMidPoint(mx, my); }
			t->setAttachPose(mx, my);   // 立刻摆正，别等下一帧
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
	if (toolSub) toolSub->blurInputs();
	if (editingText && textBox && textBox->isPosIn(pos)) return;
	auto imgPos = toImgPos(pos);
	const auto tool = getCurToolId();

	// 钢笔式笔画：未收笔前，鼠标位置只作为预览落点（Shift = 预览为直线）
	if (pendingPen) {
		if (!penPendingAlive() || tool != L"arrow") {
			finalizePendingPen();
		}
		else if (!annotMouseDown) {
			pendingPen->penPreviewPoint((float)imgPos.x, (float)imgPos.y,
				(GetKeyState(VK_SHIFT) & 0x8000) != 0);
			refresh();
		}
	}

	if (marqueeActive_) {
		updateMarquee(imgPos);
		refresh();
		return;
	}
	if (groupScaleActive_) {
		updateGroupScale(imgPos);
		applyAnnotCursor();
		return;
	}
	if (groupMoveActive_ || groupRotateActive_) {
		updateGroupTransform(imgPos, groupRotateActive_);
		applyAnnotCursor();
		return;
	}

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
			// 尺寸：自带文字跟着线长实时刷新 —— 创建中和之后的二次编辑拖动都要刷新。
			// 以形状自身是否为尺寸为准（不依赖当前工具），用户改过的文字由 setAutoText 拦住。
			if (auto* ar = dynamic_cast<ShapeArrow*>(shapeHover); ar && ar->isDimension()) {
				const std::wstring label = dimensionTextFor(ar);
				if (auto* t = ensureShaftText(ar, label, 24.f * dpi))
					t->setAutoText(label);
			}
			// 角度：ShapeLine::mouseDrag 里已经调 updateAngleLabel()
			// 拖动中也要刷起点提示点（连线时它跟着箭头起点沿边缘滑）
			updateLinkHints(imgPos, tool);
			// 拖动中不重算上层的模糊/马赛克：每次重算都要回读像素 + 模糊，层数一多会明显卡顿。
			// 抬手时（mouseUp）会有一次精确重算，拖动期间上层保持上一帧即可。
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
	// 序号跟手预览需每帧重绘
	if (numberCursorFollow || getCurToolId() == L"number") refresh();
}

void AnnotHost::annotUp(POINT pos, BOOL isRight)
{
	if (isRight) return;
	if (marqueeActive_) {
		finishMarquee();
		annotMouseDown = false;
		annotDragIndex = -1;
		ReleaseCapture();
		applyAnnotCursor();
		return;
	}
	if (groupMoveActive_ || groupRotateActive_ || groupScaleActive_) {
		finishGroupTransform();
		annotMouseDown = false;
		annotDragIndex = -1;
		ReleaseCapture();
		applyAnnotCursor();
		return;
	}
	// 取走本次按下的编辑快照：早退路径也不会留下陈旧状态
	ShapeBase* editTarget = editStepTarget;
	std::vector<float> editBefore = std::move(editStepBefore);
	editStepTarget = nullptr;
	editStepBefore.clear();
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
		// 左键点按（没拖）：箭头/线段进入钢笔式点按，等后续锚点与右键收笔
		if (!linkPressTarget) {
			const auto penPos = toImgPos(annotPressPos);
			if (shapeHover->penBegin((float)penPos.x, (float)penPos.y)) {
				pendingPen = shapeHover;
				lastCreatedShape_ = shapeHover;
				shapeHover = nullptr;
				newShape = nullptr;
				refresh();
				applyAnnotCursor();
				return;
			}
		}
		// 整体删：拖拽中可能已经挂了自带文字（尺寸），空线不能把文字孤零零留下
		history->removeShapeGroup(shapeHover);
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
	// 拖动/缩放/旋转已有标注 = 一步编辑：几何真的变了才进撤销栈
	if (!justCreated && editTarget && editTarget == shapeHover) {
		const auto after = editTarget->editSnapshot();
		if (hasDragged && !after.empty() && after != editBefore)
			history->addEditStep(editTarget, editBefore, after);
	}
	// 抬手后按最终几何再采样一次（拖拽中已经跟手重算，这里收个尾更稳）
	refreshFiltersAbove(shapeHover);

	// 尺寸：自带文字（拖拽中已经挂上，这里只兜底刷新；用户改过的文字不会被覆盖）
	if (justCreated) {
		if (auto* ar = dynamic_cast<ShapeArrow*>(shapeHover); ar && ar->isDimension()) {
			const std::wstring label = dimensionTextFor(ar);
			if (auto* t = ensureShaftText(ar, label, 24.f * dpi))
				t->setAutoText(label);
		}
	}

	// 角度三点式：没画完就挂起等第三点；画完则确保自动测量的角度文字已挂上
	if (justCreated) {
		if (auto* ln = dynamic_cast<ShapeLine*>(shapeHover); ln && ln->isAngle()) {
			if (!ln->angleComplete()) {
				pendingAngle = ln;
			}
			else {
				pendingAngle = nullptr;
				ensureShaftText(ln, ln->angleLabelText(), ln->angleLabelFontPx());
				ln->updateAngleLabel();
			}
		}
	}

	// 共用：画完一律不立刻出控点；再次点击才选中（文字编辑靠 editingText，不靠选中态）
	if (justCreated) {
		lastCreatedShape_ = justCreated;
		shapeHover = nullptr;
	}
	// 仅纯点选（无拖动）才载入标注样式；拖动 / 缩放 / 旋转只修改几何，不影响工具默认值。
	// 交由定时器延后一拍执行，避免在鼠标抬起消息中重排属性栏。
	if (styleLoadCandidate && styleLoadCandidate == shapeHover
		&& std::abs(pos.x - annotPressPos.x) <= 3
		&& std::abs(pos.y - annotPressPos.y) <= 3) {
		setTimer(1, 214);
	}
	styleLoadCandidate = nullptr;
	refresh();
	applyAnnotCursor();
}

void AnnotHost::annotKey(UINT key)
{
	dropStaleEditingText();
	if (editingText) {
		// 与双击全选同一套状态（selectAll_ 蓝底白字）；编辑态所有键都先到这里，
		// 不会落到命令表里被当成"截图工具"的 Ctrl+A。
		const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		if (ctrl && key == 'A') editingText->selectAllText();
		else if (key == VK_ESCAPE) editingText->finishEdit();
		else if (key == VK_BACK) editingText->backspace();
		else if (key == VK_DELETE) editingText->deleteForward();
		else if (key == VK_LEFT) editingText->moveCaret(-1);
		else if (key == VK_RIGHT) editingText->moveCaret(1);
		else if (key == VK_HOME) editingText->moveCaretTo(false);
		else if (key == VK_END) editingText->moveCaretTo(true);
		else if (key == VK_RETURN) editingText->insertText(L"\n");
		if (editingText && editingText->isEditingNow()) positionImeWindow();
		return;
	}
	if (!history) return;
	bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (ctrl && key == 'Z') {
		// 与「系统按键」约定一致：Ctrl+Z 撤销、Ctrl+Shift+Z 重做
		if (shift) history->redo();
		else history->undo();
	}
	else if (key == VK_DELETE || key == VK_BACK) {
		// 序号尾巴被拖出控制圈：先把尾巴收回圈上，这一次按键不当"删除"
		if (!hasMultiSelection() && shapeHover && shapeHover->retractTail()) return;
		if (!hasMultiSelection()) {
			history->removeHoverShape();
		}
		else {
			const auto targets = selectedShapes();
			clearAnnotSelection();
			for (auto* t : targets) {
				// 前一个目标可能把线上文字/宿主一起带走了：先确认还在列表里，避免对已释放对象做虚调用
				bool alive = false;
				for (auto& s : history->shapes) {
					if (s.get() == t && !s->isUndo) { alive = true; break; }
				}
				if (alive) history->removeShapeGroup(t);
			}
			pruneAnnotSelection();
			refresh();
		}
	}
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
	// 离屏合成：开洞就从这张底图上抠，不能用画布兜底色（透明导出会被垫成实色）
	punchSource_ = srcBmp.Get();
	paintShapes(d2d->deviceContext.Get(), false, true);
	punchSource_ = nullptr;
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
