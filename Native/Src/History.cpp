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
            auto shape = std::make_unique<ShapeRect>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
    }
    else if (curId == L"arrow") {
        if (win->toolSub->isLine) {
            auto shape = std::make_unique<ShapeLine>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
        else {
            auto shape = std::make_unique<ShapeArrow>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
    }
    else if (curId == L"pen") {
        auto shape = std::make_unique<ShapePen>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
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
    result->mouseDown((float)x, (float)y);
    win->onHistoryChanged();
    return result;
}

namespace {
D2D1_RECT_F filterViewport(AnnotHost* win)
{
	if (auto* cap = dynamic_cast<WinCap*>(win)) {
		if (cap->cutMask && cap->cutMask->hasRect())
			return cap->cutMask->maskRect;
	}
	return D2D1::RectF(0.f, 0.f, (float)win->w, (float)win->h);
}
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
			s->isUndo = true;
			if (win->shapeHover == s.get()) win->shapeHover = nullptr;
		}
	}

	if (keep != L"watermark" && keep != L"patina") {
		win->refresh();
		win->onHistoryChanged();
		return;
	}

	const auto vp = filterViewport(win);
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

void History::undo()
{
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        // 渐隐画笔等临时笔迹不进撤销栈（laser 不提交成 op）
        if (!cur->isUndo && !cur->isEphemeral()) {
            cur->isUndo = true;
            if (cur == win->shapeHover) {
                win->shapeHover = nullptr;
            }
            win->refresh();
            win->onHistoryChanged();
            break;
        }
    }
}

void History::redo()
{
    for (size_t i = 0; i < shapes.size(); i++)
    {
        auto cur = shapes[i].get();
        if (cur->isUndo) {
            cur->isUndo = false;
            win->refresh();
            win->onHistoryChanged();
            break;
        }
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
    win->refresh();
    win->onHistoryChanged();
}

void History::eraseAt(const float x, const float y)
{
    bool erased{ false };
    for (int i = (int)shapes.size() - 1; i >= 0; --i) {
        auto cur = shapes[i].get();
        if (cur->isUndo || cur->isEphemeral()) continue;
        if (!cur->hitErase(x, y)) continue;
        // 编辑中的文字先收尾，再 undo；空文本 finishEdit 会异步硬删，这里再标 isUndo 也无妨
        if (auto txt = dynamic_cast<ShapeText*>(cur)) {
            if (txt->isEditing) txt->finishEdit();
        }
        cur->isUndo = true;
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
