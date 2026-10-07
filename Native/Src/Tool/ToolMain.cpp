#include "pch.h"
#include "../Win/WinPin.h"
#include "../History.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolMain.h"
#include "ToolSub.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"

ToolMain::ToolMain(WinPin* win) : Ling::WinBase(), win(win)
{
	dpi = win->dpi;
	x = win->x;
	y = (int)(win->y + win->h + 5.f * win->dpi);
	refreshSize();
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutTools();
	});
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

void ToolMain::refreshSize()
{
	float logicW{ ToolbarTheme::paddingLeft + ToolbarTheme::dragIconSize + dragMarginRight() };
	for (auto& id : btnIds) {
		logicW += (id == L"|") ? splitterSlotW() : btnSize;
	}
	logicW += contentPadRight();
	setSize(logicW + ToolbarTheme::shadowPad * 2.f, btnSize + ToolbarTheme::shadowPad * 2.f);
}

float ToolMain::splitterSlotW() const
{
	return spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f;
}

float ToolMain::iconSideInset() const
{
	return (btnSize - hoverInset * 2.f - Icon::Size) * 0.5f;
}

float ToolMain::dragMarginRight() const
{
	return ToolbarTheme::dragGap - hoverInset - iconSideInset();
}

float ToolMain::contentPadRight() const
{
	return ToolbarTheme::paddingRight - hoverInset - iconSideInset();
}

float ToolMain::contentLeadWidth() const
{
	return ToolbarTheme::paddingLeft + ToolbarTheme::dragIconSize + dragMarginRight();
}

ToolMain::~ToolMain()
{
}

void ToolMain::init()
{
}

float ToolMain::getBtnCenterX()
{
	float result{ (ToolbarTheme::shadowPad + contentLeadWidth()) * dpi };
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		if (btnIds[i] == L"|") {
			result += splitterSlotW() * dpi;
			continue;
		}
		if (curId == btnIds[i]) {
			result += btnSize * dpi * 0.5f;
			return result;
		}
		result += btnSize * dpi;
	}
	return result;
}

void ToolMain::onCreated()
{
	tip = std::make_unique<Tip>(this);
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
	initDragHandle();
	const float sepMarginV = (btnSize - spliterH) * 0.5f;
	const float sepMarginH = ToolbarTheme::splitterGap - hoverInset;
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		auto& id = btnIds[i];
		if (id == L"|") {
			auto spliter = contentNode->makeChild<Ling::Node>();
			spliter->setSize(spliterW, spliterH);
			spliter->setMargin(sepMarginH, sepMarginV, sepMarginH, sepMarginV);
			spliter->setBg(ToolbarTheme::splitter);
			spliter->setFlexGrow(0.f);
			spliter->setFlexShrink(0.f);
			splitterNodes.push_back(spliter);
		}
		else {
			auto btn = contentNode->makeChild<Ling::Button>();
			btn->setId(id);
			btn->setText(btnCodes[i]);
			styleToolbarBtn(btn);
			btn->setFontFamily(Icon::Family);
			btn->setFontSize(Icon::Size);
			applyActionColors(btn);
			btn->onClick.add([this](Ling::Button* btn) {onClick(btn);});
			tip->bind(btn, Lang::get(std::format(L"tool.{}", id)));
			btns.push_back(btn);
		}
	}
	onMouseDown.add([this](POINT pos, bool isRight) { onDragDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { onDragMove(pos); });
	onMouseUp.add([this](POINT pos, bool isRight) { onDragUp(pos, isRight); });
	show();
}

void ToolMain::initDragHandle()
{
	dragHandle = contentNode->makeChild<Ling::Button>();
	dragHandle->setId(L"drag");
	dragHandle->setText(Icon::DragHandle);
	dragHandle->setFontFamily(Icon::Family);
	dragHandle->setFontSize(Icon::Size);
	dragHandle->setColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverBg(0);
	const float marginV = (btnSize - ToolbarTheme::dragIconSize) * 0.5f;
	dragHandle->setSize(ToolbarTheme::dragIconSize, ToolbarTheme::dragIconSize);
	dragHandle->setMargin(0.f, marginV, dragMarginRight(), marginV);
	dragHandle->setFlexGrow(0.f);
	dragHandle->setFlexShrink(0.f);
	dragHandle->setAlignItems(Ling::Align::Center);
	dragHandle->setJustifyContent(Ling::Justify::Center);
	tip->bind(dragHandle, L"拖动");
}

void ToolMain::onDragDown(POINT pos, bool isRight)
{
	if (isRight || !dragHandle || !dragHandle->isPosIn(pos)) return;
	draggingBar = true;
	dragGrabOffset = pos;
	if (hwnd) SetCapture(hwnd);
	tip->hide();
}

void ToolMain::onDragMove(POINT)
{
	if (!draggingBar) return;
	POINT pt{};
	GetCursorPos(&pt);
	setPosition(pt.x - dragGrabOffset.x, pt.y - dragGrabOffset.y);
	if (win && win->toolSub && win->toolSub->hasContent()) {
		RECT winRect{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
		win->toolSub->updatePosition(ToolbarChrome::workAreaNear(winRect));
	}
}

void ToolMain::onDragUp(POINT, bool)
{
	if (!draggingBar) return;
	draggingBar = false;
	if (GetCapture() == hwnd) ReleaseCapture();
	if (win) win->markMainToolUserPlaced();
}

void ToolMain::layout()
{
	Ling::WinBase::layout();
	paintChrome();
}

void ToolMain::paintChrome()
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

void ToolMain::styleToolbarBtn(Ling::Button* btn)
{
	const float inner{ btnSize - hoverInset * 2.f };
	btn->setSize(inner, inner);
	btn->setMargin(hoverInset);
	btn->setFlexGrow(0.f);
	btn->setFlexShrink(0.f);
	btn->setBorderRadius(hoverRadius);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setAlignItems(Ling::Align::Center);
	btn->setJustifyContent(Ling::Justify::Center);
}

void ToolMain::applyActionColors(Ling::Button* btn)
{
	if (btn->id == L"close") {
		btn->setColor(Icon::ColorCancel);
		btn->setHoverColor(Icon::ColorCancel);
	}
	else if (btn->id == L"clipboard") {
		btn->setColor(Icon::ColorDone);
		btn->setHoverColor(Icon::ColorDone);
	}
	else {
		btn->setColor(Icon::ColorNormal);
		btn->setHoverColor(Icon::ColorNormal);
	}
}

void ToolMain::applyNormalStyle(Ling::Button* btn)
{
	btn->setBg(0);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	applyActionColors(btn);
}

void ToolMain::applySelectedStyle(Ling::Button* btn)
{
	btn->setBg(0);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setColor(Icon::ColorActive);
	btn->setHoverColor(Icon::ColorActive);
}

void ToolMain::cancelSelect()
{
	if (curId.empty()) return;
	for (auto b : btns)
	{
		if (b->id == curId) {
			applyNormalStyle(b);
		}
	}
	curId.clear();
	win->toolSub->hideTools();
	win->layoutTools();
}

void ToolMain::selectTool(const std::wstring& id)
{
	for (auto b : btns) {
		if (b->id == id) {
			onClick(b);
			return;
		}
	}
}

void ToolMain::onClick(Ling::Button* btn)
{
	if (btn->id == L"close") {
		win->close();
		return;
	}
	else if (btn->id == L"undo") {
		win->history->undo();
		return;
	}
	else if (btn->id == L"redo") {
		win->history->redo();
		return;
	}
	else if (btn->id == L"save") {
		win->saveToFile();
		return;
	}
	else if (btn->id == L"clipboard") {
		win->copyToClipboard();
		return;
	}
	if (btn->id == curId) {
		if (win->hasSelectedAnnot()) {
			// 已选标注：再点同工具不关属性栏
			if (curId == L"rect") win->toolSub->showRectTools();
			else if (curId == L"arrow") win->toolSub->showArrowTools();
			else if (curId == L"number") win->toolSub->showNumberTools();
			else if (curId == L"text") win->toolSub->showTextTools();
			else if (curId == L"mosaic") win->toolSub->showMosaicTools();
			win->presentSelectedAnnotStyle();
			win->layoutTools();
			return;
		}
		cancelSelect();
		return;
	}
	for (auto b:btns)
	{
		if (b->id == curId)
		{
			applyNormalStyle(b);
		}
		if (b->id == btn->id)
		{
			applySelectedStyle(b);
		}
	}
	curId = btn->id;
	if (curId == L"rect") {
		win->toolSub->showRectTools();
	}
	else if (curId == L"arrow") {
		win->toolSub->showArrowTools();
	}
	else if (curId == L"number") {
		win->toolSub->showNumberTools();
	}
	else if (curId == L"text") {
		win->toolSub->showTextTools();
	}
	else if (curId == L"mosaic") {
		win->toolSub->showMosaicTools();
	}
	else if (curId == L"eraser") {
		win->toolSub->showEraserTools();
	}
	else {
		win->toolSub->hideTools();
	}
	win->layoutTools();
}

void ToolMain::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
