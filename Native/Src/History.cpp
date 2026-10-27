#include "pch.h"
#include "History.h"
#include "Win/AnnotHost.h"
#include "Win/WinCap.h"
#include "Win/CutMask.h"
#include "Tool/ToolSub.h"
#include "Shape/ShapeBase.h"
#include "Shape/ShapeRect.h"
#include "Shape/ShapeEllipse.h"
#include "Shape/ShapeArrow.h"
#include "Shape/ShapeNumber.h"
#include "Shape/ShapeLine.h"
#include "Shape/ShapePen.h"
#include "Shape/ShapeText.h"
#include "Shape/ShapeMosaic.h"
#include "Shape/ShapePatina.h"
#include "Shape/ShapeWatermark.h"
#include "Shape/ShapeHighlight.h"

namespace {
// 白板"调整背景"的一步操作：本身不画任何东西，只负责在撤销/重做时把尺寸换回去。
// 放进 shapes 里是为了和标注共用同一套撤销顺序（先撤销最后做的操作）。
class ShapeBoardResize : public ShapeBase
{
public:
	ShapeBoardResize(AnnotHost* win, int page, const D2D1_RECT_F& before, const D2D1_RECT_F& after)
		: ShapeBase(win), page_(page), before_(before), after_(after)
	{
	}
	void paint(ID2D1DeviceContext*) override {}
	void onUndoChanged(bool undone) override
	{
		if (auto* cap = dynamic_cast<WinCap*>(win)) {
			cap->demoBoardRestoreRect(page_, undone ? before_ : after_);
			cap->refresh();
		}
	}
private:
	int page_;
	D2D1_RECT_F before_, after_;
};

// 编辑一步：记录目标标注编辑前后的几何，撤销时回退到 before、重做时回到 after
class ShapeEditStep : public ShapeBase
{
public:
	ShapeEditStep(AnnotHost* win, ShapeBase* target,
		std::vector<float> before, std::vector<float> after)
		: ShapeBase(win), target_(target), before_(std::move(before)), after_(std::move(after))
	{
	}
	ShapeEditStep(AnnotHost* win, ShapeBase* target,
		std::vector<float> before, std::vector<float> after,
		std::wstring beforeText, std::wstring afterText)
		: ShapeBase(win), target_(target), before_(std::move(before)), after_(std::move(after)),
		  beforeText_(std::move(beforeText)), afterText_(std::move(afterText)), hasText_(true)
	{
	}
	void paint(ID2D1DeviceContext*) override {}
	void onUndoChanged(bool undone) override
	{
		if (!target_) return;
		target_->applyEditSnapshot(undone ? before_ : after_);
		if (hasText_) {
			if (auto* t = dynamic_cast<ShapeText*>(target_))
				t->applyTextFromUndo(undone ? beforeText_ : afterText_);
		}
		target_->syncLinks();
		win->refresh();
	}
	ShapeBase* target() const { return target_; }
private:
	ShapeBase* target_{ nullptr };
	std::vector<float> before_;
	std::vector<float> after_;
	std::wstring beforeText_;
	std::wstring afterText_;
	bool hasText_{ false };
};

void SetShapeUndo(ShapeBase* shape, bool undone)
{
	if (!shape) return;
	shape->isUndo = undone;
	shape->onUndoChanged(undone);
}

// 尺寸/角度的数值文字是拖拽过程中后建的，和宿主线是一个整体：
// 单独撤销只会删掉文字，留下一条没有数值的线。
ShapeBase* undoGroupHost(ShapeBase* shape)
{
	if (auto* t = dynamic_cast<ShapeText*>(shape)) {
		if (t->attachTo) return t->attachTo;
	}
	return shape;
}
}

History::History(AnnotHost* win):win{win}
{

}

History::~History()
{

}
ShapeBase* History::createShape(const std::wstring& state, const int& x, const int& y)
{
    removeUndoShape();
    ShapeBase* result{nullptr};
	auto curId = win->getCurToolId();
    if (curId == L"rect") {
        if (win->toolSub->isEllipse) {
            auto shape = std::make_unique<ShapeEllipse>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
        else {
            // 矩形 / 多边形 / 星形共用一套框选交互，形态由属性栏决定
            const int rk = win->toolSub->rectKind;
            const int sides = rk == ToolSub::PolygonShape ? win->toolSub->polygonSides
                : rk == ToolSub::StarShape ? win->toolSub->starPoints : 0;
            auto shape = std::make_unique<ShapeRect>(win, rk, sides);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
    }
    else if (curId == L"arrow") {
        if (win->toolSub->isLine) {
            // 标注的形态：气泡 = 带默认文字的对话气泡；
            // 尺寸 = 标记箭头的外观 + 中间自带文字（唯一区别是带不带文字）；
            // 波浪线/角度走线段（角度是三点折线）
            const int lk = win->toolSub->lineKind;
            if (lk == ToolSub::LineBubble) {
                auto shape = std::make_unique<ShapeText>(win, true);
                result = shape.get();
                shapes.push_back(std::move(shape));
            }
            else if (lk == ToolSub::LineBurst) {
                // 爆裂：和气泡同一套文字/编辑交互，只是外框换成尖角爆炸轮廓
                auto shape = std::make_unique<ShapeText>(win, true, true);
                result = shape.get();
                shapes.push_back(std::move(shape));
            }
            else if (lk == ToolSub::LineDimension) {
                // 尺寸 = 标记箭头（标注箭头样式：两端是竖线）+ 中间自带文字，只有两个端点
                auto shape = std::make_unique<ShapeArrow>(win, ToolSub::ArrowAnnot, true);
                result = shape.get();
                shapes.push_back(std::move(shape));
            }
            else {
                const int kind = (lk == ToolSub::LineWave) ? ShapeLine::Wave
                    : (lk == ToolSub::LineAngle) ? ShapeLine::Angle
                    : ShapeLine::Straight;
                auto shape = std::make_unique<ShapeLine>(win, false, kind);
                result = shape.get();
                shapes.push_back(std::move(shape));
            }
        }
        else {
            auto shape = std::make_unique<ShapeArrow>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
    }
    else if (curId == L"pen") {
        if (win->toolSub->isPenMarker) {
            // 荧光笔：和线段同一套交互（首尾两端点），只是更宽、半透明、平头
            auto shape = std::make_unique<ShapeLine>(win, true);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
        else {
            auto shape = std::make_unique<ShapePen>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
    }
    else if (curId == L"number") {
        auto shape = std::make_unique<ShapeNumber>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"text") {
        auto shape = std::make_unique<ShapeText>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"mosaic") {
        auto shape = std::make_unique<ShapeMosaic>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"highlight") {
        auto shape = std::make_unique<ShapeHighlight>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    // watermark / patina：视口滤镜，由 syncViewportFilters 创建，不在此落笔
    // eraser 不建 shape，由 eraseAt 直接擦除
    if (!result) return nullptr;
    // 白板「编辑背景」时画的标注归入背景层：关掉编辑背景后不参与点选/擦除
    result->backgroundLayer = win->boardBackgroundEditing();
    result->mouseDown((float)x, (float)y);
    win->onHistoryChanged();
    return result;
}

D2D1_RECT_F annotImageRect(AnnotHost* win)
{
	if (auto* cap = dynamic_cast<WinCap*>(win)) {
		if (cap->cutMask && cap->cutMask->hasRect())
			return cap->cutMask->maskRect;
	}
	// 标注坐标是图像像素：优先用底图自身尺寸，贴图缩放后客户区尺寸不再等于图像尺寸
	const float sc = win->scale > 0.001f ? win->scale : 1.f;
	const float annotW = (float)win->w / sc;
	const float annotH = (float)win->h / sc;
	if (win->screenImg) {
		const D2D1_SIZE_U px = win->screenImg->GetPixelSize();
		if (px.width > 0 && px.height > 0
			&& std::fabs((float)px.width - annotW) <= 2.f
			&& std::fabs((float)px.height - annotH) <= 2.f)
			return D2D1::RectF(0.f, 0.f, (float)px.width, (float)px.height);
	}
	return D2D1::RectF(0.f, 0.f, annotW, annotH);
}

void History::syncViewportFilters(const std::wstring& toolId)
{
	std::wstring keep;
	if (toolId == L"patina") keep = L"patina";
	else if (toolId == L"watermark") keep = L"watermark";
	else if (toolId == L"highlight") keep = L"highlight";

	// 切换工具时丢掉其它视口滤镜（高亮洞可保留当 keep==highlight）
	for (auto& s : shapes) {
		if (s->isUndo) continue;
		const bool isWm = dynamic_cast<ShapeWatermark*>(s.get()) != nullptr;
		const bool isPa = dynamic_cast<ShapePatina*>(s.get()) != nullptr;
		const bool isHl = dynamic_cast<ShapeHighlight*>(s.get()) != nullptr;
		if (!isWm && !isPa && !isHl) continue;
		std::wstring kind = isWm ? L"watermark" : (isPa ? L"patina" : L"highlight");
		if (keep.empty() || kind != keep) {
			SetShapeUndo(s.get(), true);
			if (win->shapeHover == s.get()) win->shapeHover = nullptr;
		}
	}

	if (keep != L"watermark" && keep != L"patina") {
		win->refresh();
		win->onHistoryChanged();
		return;
	}

	const auto vp = annotImageRect(win);
	if (vp.right - vp.left < 2.f || vp.bottom - vp.top < 2.f) return;

	ShapeBase* existing = nullptr;
	for (auto& s : shapes) {
		if (s->isUndo) continue;
		if (keep == L"watermark" && dynamic_cast<ShapeWatermark*>(s.get())) { existing = s.get(); break; }
		if (keep == L"patina" && dynamic_cast<ShapePatina*>(s.get())) { existing = s.get(); break; }
	}

	removeUndoShape();
	if (!existing) {
		if (keep == L"watermark") {
			auto shape = std::make_unique<ShapeWatermark>(win);
			existing = shape.get();
			shapes.push_back(std::move(shape));
		}
		else {
			auto shape = std::make_unique<ShapePatina>(win);
			existing = shape.get();
			shapes.push_back(std::move(shape));
		}
	}
	if (auto* f = dynamic_cast<ShapeWatermark*>(existing)) {
		f->setViewport(vp);
		f->applyStyle();
	}
	else if (auto* f = dynamic_cast<ShapePatina*>(existing)) {
		f->setViewport(vp);
		f->applyStyle();
	}
	win->refresh();
	win->onHistoryChanged();
}

void History::clearAll()
{
	// 先快照一份指针再逐条删：removeShape 会改动 shapes，边遍历边删会使迭代器失效。
	// 且必须走 removeShape —— 它同时收尾 hover / 选中 / 正在编辑的文本等关联状态，
	// 直接 shapes.clear() 会让这些状态残留为悬空指针。
	std::vector<ShapeBase*> victims;
	victims.reserve(shapes.size());
	for (auto& s : shapes) if (s) victims.push_back(s.get());
	for (auto* v : victims) {
		// 渐隐画笔这类临时笔迹不归清除管（它们自己会消失）
		if (v->isEphemeral()) continue;
		removeShape(v);
	}
	win->refresh();
	win->onHistoryChanged();
}

void History::undo()
{
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        // 渐隐画笔等临时笔迹不进撤销栈（laser 不提交成 op）
        if (cur->isUndo || cur->isEphemeral()) continue;

        // 撤销以线为单位：文字单独成条时回溯到宿主，宿主与其文字一起走
        ShapeBase* host = undoGroupHost(cur);
        if (host->isUndo || host->isEphemeral()) host = cur;
        if (host != cur) SetShapeUndo(cur, true);
        SetShapeUndo(host, true);
        for (auto& s : shapes) {
            if (s.get() == host || s->isUndo) continue;
            if (auto* t = dynamic_cast<ShapeText*>(s.get()); t && t->attachTo == host)
                SetShapeUndo(s.get(), true);
        }
        if (win->shapeHover == cur || win->shapeHover == host) {
            win->shapeHover = nullptr;
        }
        win->refresh();
        win->onHistoryChanged();
        break;
    }
}

void History::redo()
{
    for (size_t i = 0; i < shapes.size(); i++)
    {
        auto cur = shapes[i].get();
        if (!cur->isUndo) continue;
        // 与撤销对称：恢复一条线时把它挂着的文字一起恢复
        SetShapeUndo(cur, false);
        for (auto& s : shapes) {
            if (s.get() == cur || !s->isUndo) continue;
            if (auto* t = dynamic_cast<ShapeText*>(s.get()); t && t->attachTo == cur)
                SetShapeUndo(s.get(), false);
        }
        win->refresh();
        win->onHistoryChanged();
        break;
    }
}

bool History::canUndo() const
{
	for (auto& s : shapes) {
		if (s && !s->isUndo && !s->isEphemeral()) return true;
	}
	return false;
}

/// <summary>
/// 进删除除 hover 状态的 shape
/// </summary>
void History::removeHoverShape()
{
    if (!win->shapeHover) return;
    auto target = win->shapeHover;
    // 正在编辑的话先收尾：TextBox 是 WinPin 上共用的一个，
    // 删了 shape 却留着它显示，下一次编辑就会带着上一次的文字。
    if (auto txt = dynamic_cast<ShapeText*>(target)) {
        txt->finishEdit();
    }
    removeShapeGroup(target);
}

// 「整体删除」：箭头/线段 ↔ 线上文字是一个整体，删一个同组的另一个一起走。
// 注意别把它做成 removeShape 的默认行为 —— finishEdit 里异步删"空文字"用的还是
// removeShape（单删），否则删一个空标签会把整条线也带走
void History::removeShapeGroup(ShapeBase* target)
{
    if (!target) return;
    std::vector<ShapeBase*> doomed{ target };
    if (auto* t = dynamic_cast<ShapeText*>(target)) {
        // 目标是线上文字：把宿主线也带上
        if (t->attachTo) doomed.push_back(t->attachTo);
    }
    else {
        // 目标是线（或别的形状）：把它挂着的线上文字一起带上
        for (auto& s : shapes) {
            if (s.get() == target) continue;
            if (auto* t = dynamic_cast<ShapeText*>(s.get())) {
                if (t->attachTo == target) doomed.push_back(t);
            }
        }
    }
    for (auto* d : doomed) {
        if (d) removeShape(d);
    }
}

void History::removeShape(ShapeBase* target)
{
    for (auto it = shapes.begin(); it != shapes.end(); ++it) {
        if (it->get() == target) {
            shapes.erase(it);
            break;
        }
    }
    // hover 指针指向的正是刚被销毁的那个元素时必须清掉，否则下一次绘制/命中就是野指针
    if (win->shapeHover == target) {
        win->shapeHover = nullptr;
    }
    // 目标已删除：引用它的编辑步骤一并丢弃，避免撤销时作用到野指针
    for (auto it2 = shapes.begin(); it2 != shapes.end(); ) {
        auto* step = dynamic_cast<ShapeEditStep*>(it2->get());
        if (step && step->target() == target) it2 = shapes.erase(it2);
        else ++it2;
    }
    win->refresh();
    win->onHistoryChanged();
}

void History::eraseAt(const float x, const float y)
{
    bool erased{ false };
    for (int i = (int)shapes.size() - 1; i >= 0; --i) {
		auto cur = shapes[i].get();
		if (cur->isUndo || cur->isEphemeral()) continue;
		// 背景层标注只在「编辑背景」打开时可擦除
		if (cur->backgroundLayer && !win->boardBackgroundEditing()) continue;
		if (!cur->hitErase(x, y)) continue;
        // 编辑中的文字先收尾，再 undo；空文本 finishEdit 会异步硬删，这里再标 isUndo 也无妨
        if (auto txt = dynamic_cast<ShapeText*>(cur)) {
            if (txt->isEditing) txt->finishEdit();
        }
        SetShapeUndo(cur, true);
        if (win->shapeHover == cur) {
            win->shapeHover = nullptr;
        }
        erased = true;
    }
    if (erased) {
		win->refresh();
		win->onHistoryChanged();
	}
}

void History::addBoardResize(int page, const D2D1_RECT_F& before, const D2D1_RECT_F& after)
{
	// 新操作会丢弃已撤销的重做栈（与 createShape 一致）
	removeUndoShape();
	shapes.push_back(std::make_unique<ShapeBoardResize>(win, page, before, after));
	win->onHistoryChanged();
}

void History::addEditStep(ShapeBase* target, const std::vector<float>& before, const std::vector<float>& after)
{
	if (!target || before.empty() || before == after) return;
	// 新操作会丢弃已撤销的重做栈
	removeUndoShape();
	shapes.push_back(std::make_unique<ShapeEditStep>(win, target, before, after));
	win->onHistoryChanged();
}

void History::addTextEditStep(ShapeBase* target,
	const std::vector<float>& before, const std::vector<float>& after,
	const std::wstring& beforeText, const std::wstring& afterText)
{
	if (!target) return;
	if (before == after && beforeText == afterText) return;
	removeUndoShape();
	shapes.push_back(std::make_unique<ShapeEditStep>(
		win, target, before, after, beforeText, afterText));
	win->onHistoryChanged();
}

void History::removeUndoShape()
{
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        if (!cur->isUndo) {
            break;
        }
        shapes.erase(shapes.begin() + i);
    }
}

void History::purgeUndone()
{
	bool touched = false;
	for (auto it = shapes.begin(); it != shapes.end(); ) {
		if (*it && (*it)->isUndo) {
			if (win->shapeHover == it->get()) win->shapeHover = nullptr;
			it = shapes.erase(it);
			touched = true;
		}
		else ++it;
	}
	if (touched) win->onHistoryChanged();
}
