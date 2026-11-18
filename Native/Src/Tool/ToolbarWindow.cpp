#include "pch.h"
#include "ToolbarWindow.h"
#include "ToolbarTheme.h"
#include "Tip.h"
#include "Win/WinCap.h"

namespace {
	// 命中测试用：point（窗口客户区物理像素）是否落在本窗自己的可交互控件上。
	// Button / Slider / TextBox 是工具条里仅有的会吞鼠标的节点。
	bool hitOwnControl(Ling::Node* node, POINT pt)
	{
		if (!node || !node->visual.IsVisible()) return false;
		if (dynamic_cast<Ling::Button*>(node) || dynamic_cast<Ling::Slider*>(node)
			|| dynamic_cast<Ling::TextBox*>(node)) {
			if (node->isPosIn(pt)) return true;
		}
		for (auto& child : node->children)
			if (hitOwnControl(child.get(), pt)) return true;
		return false;
	}
}

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
	POINT client = pos;
	ScreenToClient(hwnd, &client);
	// 光标压在自己的按钮/滑条上时，工具条必须优先命中：
	// 否则工具条被拖到选区边线上后，按钮所在位置正好落在遮罩的「拖边命中带」里，
	// HTTRANSPARENT 会把点击全部让给下层选区控件，表现为「工具点不动、却拖动了选框」。
	if (hitOwnControl(body.get(), client)) return HTCLIENT;
	// 空白处压住选区边角时仍把命中穿透给遮罩，保证悬停光标与拖边可用
	if (auto* host = maskHost(); host && host->hwnd && host->cutMask && host->cutMask->hasRect()) {
		POINT hostPt = pos;
		ScreenToClient(host->hwnd, &hostPt);
		const auto hit = host->cutMask->hitTest(hostPt);
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
