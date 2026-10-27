#include "pch.h"
#include "ToolbarWindow.h"
#include "ToolbarTheme.h"
#include "Tip.h"
#include "Win/WinCap.h"

void ToolbarWindow::initChrome(float padLeft, float padRight, float padTop, float padBottom,
	Ling::FlexDirection direction)
{
	ToolbarTheme::refresh();
	tip = std::make_unique<Tip>(this);
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(
		Ling::Color(ToolbarTheme::background).getD2DColor(), brushBg.GetAddressOf());
	canvas = body->makeChild<Ling::Canvas>();
	canvas->setPositionType(Ling::Position::Absolute);
	canvas->setSizePercent(100.f, 100.f);
	contentNode = body->makeChild<Ling::Node>();
	contentNode->setPositionType(Ling::Position::Absolute);
	contentNode->setPosition(Ling::Edge::Left, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Right, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Top, padTop);
	contentNode->setPosition(Ling::Edge::Bottom, padBottom);
	contentNode->setAlignItems(Ling::Align::Center);
	contentNode->setFlexDirection(direction);
	contentNode->setPaddingLeft(padLeft);
	contentNode->setPaddingRight(padRight);
}

void ToolbarWindow::layout()
{
	Ling::WinBase::layout();
	if (!canvas) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	drawChrome(ctx);
	canvas->finishPaint();
}

void ToolbarWindow::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

LRESULT ToolbarWindow::onHitTest(const POINT pos)
{
	// 压住选区边角时把命中穿透给遮罩，保证悬停光标与拖边可用
	if (auto* host = maskHost(); host && host->hwnd && host->cutMask && host->cutMask->hasRect()) {
		POINT client = pos;
		ScreenToClient(host->hwnd, &client);
		const auto hit = host->cutMask->hitTest(client);
		if (hit != MaskHit::None && hit != MaskHit::Inside)
			return HTTRANSPARENT;
	}
	return HTCLIENT;
}

void ToolbarWindow::hideHoverTip()
{
	if (tip) tip->hide();
	hideExtraHoverTips();
}
