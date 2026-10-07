#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "../Tip.h"
#include "../History.h"
#include "ToolLong.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"

ToolLong::ToolLong(WinCap* win) : Ling::WinBase(), win(win)
{
	dpi = win->dpi;
	refreshSize();
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutLongTool();
	});
}

float ToolLong::iconSideInset() const
{
	return (btnSize - ToolbarTheme::dragIconSize) * 0.5f;
}

float ToolLong::dragMarginRight() const
{
	return ToolbarTheme::dragGap - hoverInset - iconSideInset();
}

float ToolLong::contentPadRight() const
{
	return ToolbarTheme::paddingRight - hoverInset - iconSideInset();
}

float ToolLong::contentWidth() const
{
	const float drag = ToolbarTheme::paddingLeft + ToolbarTheme::dragIconSize + dragMarginRight();
	if (!editMode_) {
		// drag + long | edit | save | close | clipboard
		return drag + btnSize * 5.f + contentPadRight();
	}
	const float sep = spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f;
	// drag + 7 annot + | + undo + | + save close clipboard
	return drag + btnSize * 11.f + sep * 2.f + contentPadRight();
}

void ToolLong::refreshSize()
{
	setSize(contentWidth() + ToolbarTheme::shadowPad * 2.f,
		btnSize + ToolbarTheme::shadowPad * 2.f);
}

ToolLong::~ToolLong()
{
}

void ToolLong::syncCaptureState(bool capturing, bool paused, bool finished)
{
	this->capturing = capturing;
	this->paused = paused;
	this->finished = finished;
	applyLongBtnStyle();
	bindLongTip();
	if (hwnd) layout();
}

void ToolLong::setEditMode(bool on)
{
	if (editMode_ == on) return;
	editMode_ = on;
	curAnnotId_.clear();
	buildBar();
	if (hwnd) {
		layout();
		win->layoutLongTool();
		win->layoutTools();
	}
}

void ToolLong::cancelAnnotSelect()
{
	curAnnotId_.clear();
	for (auto* b : annotBtns) applyToggleStyle(b, false);
	win->clearLongAnnotTool();
}

void ToolLong::updateUndoEnabled()
{
	if (!btnUndo) return;
	const bool ok = win->history && win->history->canUndo();
	if (ok) {
		btnUndo->setColor(Icon::ColorNormal);
		btnUndo->setHoverColor(Icon::ColorNormal);
		btnUndo->setHoverBg(ToolbarTheme::hoverBg);
	}
	else {
		btnUndo->setColor(Icon::ColorDisabled);
		btnUndo->setHoverColor(Icon::ColorDisabled);
		btnUndo->setHoverBg(0);
	}
}

float ToolLong::getBtnCenterX() const
{
	const float pad = ToolbarTheme::shadowPad * dpi;
	float x = pad + ToolbarTheme::paddingLeft * dpi
		+ ToolbarTheme::dragIconSize * dpi + dragMarginRight() * dpi;
	if (!editMode_ || curAnnotId_.empty()) {
		// 对准编辑钮（第 2 个图标）
		return x + btnSize * dpi * 1.5f;
	}
	const float sep = (spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f) * dpi;
	static const wchar_t* kAnnot[] = {
		L"rect", L"arrow", L"pen", L"text", L"number", L"mosaic", L"eraser"
	};
	for (const auto* id : kAnnot) {
		if (curAnnotId_ == id) {
			x += btnSize * dpi * 0.5f;
			return x;
		}
		x += btnSize * dpi;
	}
	return x + btnSize * dpi * 0.5f;
}

void ToolLong::applyLongBtnStyle()
{
	if (!btnLong) return;
	const bool on = capturing && !paused && !finished;
	const uint32_t c = on ? Icon::ColorActive : Icon::ColorNormal;
	btnLong->setColor(c);
	btnLong->setHoverColor(c);
}

void ToolLong::bindLongTip()
{
	if (!tip || !btnLong) return;
	std::wstring key = L"long.start";
	if (finished) key = L"cap.long";
	else if (capturing && paused) key = L"long.resume";
	else if (capturing) key = L"long.pause";
	tip->bind(btnLong, Lang::get(key));
}

void ToolLong::applyToggleStyle(Ling::Button* btn, bool selected)
{
	if (!btn) return;
	const uint32_t c = selected ? Icon::ColorActive : Icon::ColorNormal;
	btn->setColor(c);
	btn->setHoverColor(c);
	btn->setHoverBg(ToolbarTheme::hoverBg);
}

void ToolLong::applyAnnotSelect(const std::wstring& id)
{
	curAnnotId_ = id;
	for (auto* b : annotBtns) {
		if (!b) continue;
		applyToggleStyle(b, b->id == id);
	}
}

void ToolLong::onAnnotClick(const std::wstring& id)
{
	if (!curAnnotId_.empty() && curAnnotId_ == id) {
		// 已选中标注时再点同工具：保留选中，刷新属性栏
		if (win->hasSelectedAnnot()) {
			win->startLongAnnotate(id);
			win->presentSelectedAnnotStyle();
			return;
		}
		cancelAnnotSelect();
		return;
	}
	applyAnnotSelect(id);
	win->startLongAnnotate(id);
	win->presentSelectedAnnotStyle();
}

Ling::Button* ToolLong::makeIconBtn(const std::wstring& code, const std::wstring& id)
{
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setId(id);
	btn->setText(code);
	const float inner{ btnSize - hoverInset * 2.f };
	btn->setSize(inner, inner);
	btn->setMargin(hoverInset);
	btn->setFlexGrow(0.f);
	btn->setFlexShrink(0.f);
	btn->setBorderRadius(hoverRadius);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setAlignItems(Ling::Align::Center);
	btn->setJustifyContent(Ling::Justify::Center);
	btn->setFontFamily(Icon::Family);
	btn->setFontSize(Icon::Size);
	btn->setColor(Icon::ColorNormal);
	btn->setHoverColor(Icon::ColorNormal);
	return btn;
}

Ling::Node* ToolLong::makeSpliter()
{
	auto wrap = contentNode->makeChild<Ling::Node>();
	wrap->setWidth(spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f);
	wrap->setHeightPercent(100.f);
	wrap->setAlignItems(Ling::Align::Center);
	wrap->setJustifyContent(Ling::Justify::Center);
	wrap->setFlexGrow(0.f);
	wrap->setFlexShrink(0.f);
	auto line = wrap->makeChild<Ling::Node>();
	line->setWidth(spliterW);
	line->setHeight(spliterH);
	line->setBg(ToolbarTheme::splitter);
	return wrap;
}

void ToolLong::buildBar()
{
	annotBtns.clear();
	btnLong = btnEdit = btnUndo = dragHandle = nullptr;
	tip->hide();
	contentNode->removeAllChildren();
	refreshSize();

	const float marginV = (btnSize - ToolbarTheme::dragIconSize) * 0.5f;
	dragHandle = contentNode->makeChild<Ling::Button>();
	dragHandle->setId(L"drag");
	dragHandle->setText(Icon::DragHandle);
	dragHandle->setFontFamily(Icon::Family);
	dragHandle->setFontSize(Icon::Size);
	dragHandle->setColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverBg(0);
	dragHandle->setSize(ToolbarTheme::dragIconSize, ToolbarTheme::dragIconSize);
	dragHandle->setMargin(0.f, marginV, dragMarginRight(), marginV);
	dragHandle->setFlexGrow(0.f);
	dragHandle->setFlexShrink(0.f);
	dragHandle->setAlignItems(Ling::Align::Center);
	dragHandle->setJustifyContent(Ling::Justify::Center);
	tip->bind(dragHandle, L"拖动");

	if (!editMode_) {
		btnLong = makeIconBtn(Icon::LongShot, L"long");
		btnLong->onClick.add([this](Ling::Button* b) { onClick(b); });
		applyLongBtnStyle();
		bindLongTip();

		btnEdit = makeIconBtn(Icon::Pen, L"edit");
		tip->bind(btnEdit, Lang::get(L"long.edit"));
		btnEdit->onClick.add([this](Ling::Button* b) { onClick(b); });

		auto btnSave = makeIconBtn(Icon::Save, L"save");
		tip->bind(btnSave, Lang::get(L"tool.save"));
		btnSave->onClick.add([this](Ling::Button* b) { onClick(b); });

		auto btnClose = makeIconBtn(Icon::Cancel, L"close");
		btnClose->setColor(Icon::ColorCancel);
		btnClose->setHoverColor(Icon::ColorCancel);
		tip->bind(btnClose, Lang::get(L"tool.close"));
		btnClose->onClick.add([this](Ling::Button* b) { onClick(b); });

		auto btnDone = makeIconBtn(Icon::Done, L"clipboard");
		btnDone->setColor(Icon::ColorDone);
		btnDone->setHoverColor(Icon::ColorDone);
		tip->bind(btnDone, Lang::get(L"tool.clipboard"));
		btnDone->onClick.add([this](Ling::Button* b) { onClick(b); });
		return;
	}

	struct AnnotItem { const wchar_t* id; const wchar_t* icon; const wchar_t* tip; };
	const AnnotItem annots[] = {
		{ L"rect", Icon::Rect, L"tool.rect" },
		{ L"arrow", Icon::Arrow, L"tool.arrow" },
		{ L"pen", Icon::Pen, L"tool.pen" },
		{ L"text", Icon::Text, L"tool.text" },
		{ L"number", Icon::Number, L"tool.number" },
		{ L"mosaic", Icon::Mosaic, L"tool.mosaic" },
		{ L"eraser", Icon::Eraser, L"tool.eraser" },
	};
	for (const auto& a : annots) {
		auto* btn = makeIconBtn(a.icon, a.id);
		tip->bind(btn, Lang::get(a.tip));
		btn->onClick.add([this, id = std::wstring(a.id)](Ling::Button*) { onAnnotClick(id); });
		annotBtns.push_back(btn);
	}

	makeSpliter();

	btnUndo = makeIconBtn(Icon::Undo, L"undo");
	btnUndo->onClick.add([this](Ling::Button*) {
		if (win->history && win->history->canUndo())
			win->history->undo();
		updateUndoEnabled();
	});
	tip->bind(btnUndo, Lang::get(L"tool.undo"));
	updateUndoEnabled();

	makeSpliter();

	auto btnSave = makeIconBtn(Icon::Save, L"save");
	tip->bind(btnSave, Lang::get(L"tool.save"));
	btnSave->onClick.add([this](Ling::Button* b) { onClick(b); });

	auto btnClose = makeIconBtn(Icon::Cancel, L"close");
	btnClose->setColor(Icon::ColorCancel);
	btnClose->setHoverColor(Icon::ColorCancel);
	tip->bind(btnClose, Lang::get(L"tool.close"));
	btnClose->onClick.add([this](Ling::Button* b) { onClick(b); });

	auto btnDone = makeIconBtn(Icon::Done, L"clipboard");
	btnDone->setColor(Icon::ColorDone);
	btnDone->setHoverColor(Icon::ColorDone);
	tip->bind(btnDone, Lang::get(L"tool.clipboard"));
	btnDone->onClick.add([this](Ling::Button* b) { onClick(b); });
}

void ToolLong::onCreated()
{
	ToolbarTheme::refresh();   // 建窗时套用当前工具栏主题
	tip = std::make_unique<Tip>(this);
	tip->excludeFromCapture();
	body->setBg(0);
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(
		Ling::Color(ToolbarTheme::background).getD2DColor(), brushBg.GetAddressOf());
	chromeCanvas = body->makeChild<Ling::Canvas>();
	chromeCanvas->setPositionType(Ling::Position::Absolute);
	chromeCanvas->setSizePercent(100.f, 100.f);
	contentNode = body->makeChild<Ling::Node>();
	contentNode->setPositionType(Ling::Position::Absolute);
	contentNode->setPosition(Ling::Edge::Left, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Top, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Right, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Bottom, ToolbarTheme::shadowPad);
	contentNode->setAlignItems(Ling::Align::Center);
	contentNode->setFlexDirection(Ling::FlexDirection::Row);
	contentNode->setPaddingLeft(ToolbarTheme::paddingLeft);
	contentNode->setPaddingRight(contentPadRight());
	buildBar();
	onMouseDown.add([this](POINT pos, bool isRight) { onDragDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { onDragMove(pos); });
	onMouseUp.add([this](POINT pos, bool isRight) { onDragUp(pos, isRight); });
	show();
}

void ToolLong::layout()
{
	Ling::WinBase::layout();
	paintChrome();
}

void ToolLong::paintChrome()
{
	if (!chromeCanvas || !brushBg) return;
	auto ctx = chromeCanvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	const float pad = ToolbarTheme::shadowPad * dpi;
	const float radius = toolbarRadius * dpi;
	const float borderW = ToolbarTheme::borderWidth * dpi;
	const D2D1_RECT_F bar{ pad, pad, w - pad, h - pad };
	ToolbarChrome::paintRoundBar(ctx, bar, radius, brushBg.Get(), borderW);
	chromeCanvas->finishPaint();
}

void ToolLong::onDragDown(POINT pos, bool isRight)
{
	if (isRight) return;
	if (!dragHandle || !dragHandle->isPosIn(pos)) return;
	draggingBar = true;
	POINT scr{};
	GetCursorPos(&scr);
	dragMouseScreen = scr;
	dragWinX = x;
	dragWinY = y;
	SetCapture(hwnd);
}

void ToolLong::onDragMove(POINT)
{
	if (!draggingBar) return;
	POINT scr{};
	GetCursorPos(&scr);
	setPosition(dragWinX + scr.x - dragMouseScreen.x, dragWinY + scr.y - dragMouseScreen.y);
}

void ToolLong::onDragUp(POINT, bool)
{
	if (!draggingBar) return;
	draggingBar = false;
	ReleaseCapture();
	win->markMainToolUserPlaced();
}

void ToolLong::onClick(Ling::Button* btn)
{
	if (btn->id == L"long") {
		win->longToggle();
		return;
	}
	if (btn->id == L"edit") {
		win->toggleLongEdit();
		return;
	}
	if (btn->id == L"clipboard") {
		win->longCopyToClipboard();
	}
	else if (btn->id == L"save") {
		if (!win->longSaveToFile()) return;
	}
	win->close();
}

void ToolLong::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
