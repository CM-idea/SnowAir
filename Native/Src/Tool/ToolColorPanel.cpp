#include "pch.h"
#include <cmath>
#include "../Lang.h"
#include "../Tip.h"
#include "ToolColorPanel.h"
#include "ToolSub.h"
#include "IconCodes.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"

using namespace Microsoft::WRL;

namespace {
	D2D1_COLOR_F rgbStop(uint32_t rgb)
	{
		return D2D1::ColorF(
			((rgb >> 16) & 0xFF) / 255.f,
			((rgb >> 8) & 0xFF) / 255.f,
			(rgb & 0xFF) / 255.f);
	}

	D2D1_COLOR_F sampleQtConic(float t)
	{
		struct Stop { float p; uint32_t rgb; };
		static constexpr Stop stops[]{
			{ 0.00f, 0xFF0000 },
			{ 0.20f, 0xFFE900 },
			{ 0.40f, 0x28FF00 },
			{ 0.60f, 0x00FFF5 },
			{ 0.80f, 0x1207FF },
			{ 1.00f, 0xFF00DF },
		};
		t = (std::clamp)(t, 0.f, 1.f);
		for (size_t i = 0; i + 1 < ARRAYSIZE(stops); i++) {
			if (t <= stops[i + 1].p || i + 2 == ARRAYSIZE(stops)) {
				const float span = stops[i + 1].p - stops[i].p;
				const float u = span > 0.f ? (t - stops[i].p) / span : 0.f;
				const auto a = rgbStop(stops[i].rgb);
				const auto b = rgbStop(stops[i + 1].rgb);
				return D2D1::ColorF(
					a.r + (b.r - a.r) * u,
					a.g + (b.g - a.g) * u,
					a.b + (b.b - a.b) * u);
			}
		}
		return rgbStop(stops[ARRAYSIZE(stops) - 1].rgb);
	}
}

ToolColorPanel::ToolColorPanel(ToolSub* owner) : Ling::WinBase(), owner(owner)
{
	dpi = owner->dpi;
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

ToolColorPanel::~ToolColorPanel()
{
}

void ToolColorPanel::onCreated()
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
	const auto pad = ToolbarChrome::propBubbleContentPad(false);
	contentNode->setPosition(Ling::Edge::Left, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Right, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Top, pad.top);
	contentNode->setPosition(Ling::Edge::Bottom, pad.bottom);
	contentNode->setFlexDirection(Ling::FlexDirection::Row);
	contentNode->setAlignItems(Ling::Align::Center);
	contentNode->setPaddingLeft(ToolbarTheme::propPad);
	contentNode->setPaddingRight(ToolbarTheme::propPad);
}

void ToolColorPanel::layout()
{
	Ling::WinBase::layout();
	if (!chromeCanvas) return;
	auto ctx = chromeCanvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	paintChrome(ctx);
	chromeCanvas->finishPaint();
	paintCustomSwatch();
}

void ToolColorPanel::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void ToolColorPanel::paintChrome(ID2D1DeviceContext* ctx)
{
	ToolbarChrome::paintPropBubble(ctx, w, h, dpi, brushBg.Get(), arrowX, tipDown_);
}

void ToolColorPanel::applyTipDirection(bool tipDown)
{
	tipDown_ = tipDown;
	ToolbarChrome::applyPropBubbleContentPad(contentNode, tipDown_);
}

void ToolColorPanel::applyActiveStyle(size_t index, bool on)
{
	if (index >= rings.size()) return;
	// 描边画在 20×20 环上，内侧 18 色块；不要铺满整颗按钮
	if (on) {
		rings[index]->setBorder(1.f, Icon::ColorActive);
	}
	else {
		rings[index]->setBorder(0.f, 0);
	}
}

void ToolColorPanel::paintCustomSwatch()
{
	if (!customCanvas) return;
	auto ctx = customCanvas->startPaint();
	if (!ctx) return;
	const float wPx = customCanvas->w;
	const float hPx = customCanvas->h;
	ctx->Clear(D2D1::ColorF(0, 0.f));
	if (wPx <= 0.f || hPx <= 0.f) {
		customCanvas->finishPaint();
		return;
	}
	auto d2d = Ling::D2D::get();
	const float radius = swatchRadius * dpi;
	ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
	d2d->d2dFactory->CreateRoundedRectangleGeometry(
		D2D1::RoundedRect(D2D1::RectF(0, 0, wPx, hPx), radius, radius),
		clipGeo.GetAddressOf());
	ComPtr<ID2D1Layer> layer;
	ctx->CreateLayer(D2D1::SizeF(wPx, hPx), layer.GetAddressOf());
	if (clipGeo && layer) {
		D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
			D2D1::InfiniteRect(), clipGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		ctx->PushLayer(&lp, layer.Get());
	}
	constexpr int segs = 48;
	constexpr float pi2 = 6.28318530718f;
	const float cx = wPx * 0.5f;
	const float cy = hPx * 0.5f;
	const float R = std::sqrt(cx * cx + cy * cy) + 1.f;
	for (int i = 0; i < segs; i++) {
		const float t0 = float(i) / float(segs);
		const float t1 = float(i + 1) / float(segs);
		const float a0 = (t0 + 0.5f) * pi2;
		const float a1 = (t1 + 0.5f) * pi2;
		ComPtr<ID2D1PathGeometry> wedge;
		d2d->d2dFactory->CreatePathGeometry(wedge.GetAddressOf());
		ComPtr<ID2D1GeometrySink> sink;
		wedge->Open(sink.GetAddressOf());
		sink->BeginFigure({ cx, cy }, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine({ cx + R * std::cos(a0), cy + R * std::sin(a0) });
		sink->AddLine({ cx + R * std::cos(a1), cy + R * std::sin(a1) });
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		sink->Close();
		ComPtr<ID2D1SolidColorBrush> brush;
		ctx->CreateSolidColorBrush(sampleQtConic((t0 + t1) * 0.5f), brush.GetAddressOf());
		if (brush) ctx->FillGeometry(wedge.Get(), brush.Get());
	}
	if (clipGeo && layer) ctx->PopLayer();
	customCanvas->finishPaint();
}

void ToolColorPanel::rebuildBtns()
{
	tip->hide();
	contentNode->removeAllChildren();
	btns.clear();
	rings.clear();
	customCanvas = nullptr;
	static const wchar_t* colorNames[]{
		L"red", L"green", L"blue", L"yellow", L"black", L"white", L"custom"
	};
	const auto& colors = owner->colorPalette();
	const size_t customIdx = owner->customColorIdx();
	const size_t selected = owner->selectedColorIdx();
	for (size_t i = 0; i < colors.size(); i++) {
		auto btn = contentNode->makeChild<Ling::Button>();
		const float slot = Icon::Size + ToolbarTheme::propGap;
		const float marginH{ (slot - iconInner) * 0.5f };
		const float marginV{ (btnSize - iconInner) * 0.5f };
		btn->setSize(iconInner, iconInner);
		btn->setMargin(marginH, marginV, marginH, marginV);
		btn->setFlexGrow(0.f);
		btn->setFlexShrink(0.f);
		btn->setBorderRadius(hoverRadius);
		btn->setHoverBg(ToolbarTheme::hoverBg);
		btn->setAlignItems(Ling::Align::Center);
		btn->setJustifyContent(Ling::Justify::Center);
		btn->onClick.add([this](Ling::Button* b) { onSwatchClick(b); });
		if (i < ARRAYSIZE(colorNames))
			tip->bind(btn, Lang::get(std::format(L"color.{}", colorNames[i])));
		btns.push_back(btn);

		auto ring = btn->makeChild<Ling::Node>();
		ring->setSize(swatchRing, swatchRing);
		ring->setBorderRadius(swatchRadius + 1.f);
		ring->setAlignItems(Ling::Align::Center);
		ring->setJustifyContent(Ling::Justify::Center);
		rings.push_back(ring);

		if (i == customIdx) {
			auto swatch = ring->makeChild<Ling::Canvas>();
			swatch->setSize(swatchSize, swatchSize);
			swatch->setBorderRadius(swatchRadius);
			customCanvas = swatch;
		}
		else {
			auto label = ring->makeChild<Ling::Label>();
			label->setSize(swatchSize, swatchSize);
			label->setBg(colors[i]);
			label->setBorderRadius(swatchRadius);
			if (i == 5) label->setBorder(1.f, 0xA8A8A8FF);
		}
		applyActiveStyle(i, i == selected);
	}
	const float logicW = ToolbarTheme::propPad * 2.f
		+ (Icon::Size + ToolbarTheme::propGap) * static_cast<float>(colors.size())
		+ ToolbarTheme::shadowPad * 2.f;
	const float logicH = btnSize + marginTop + ToolbarTheme::shadowPad;
	setSize(logicW, logicH);
}

void ToolColorPanel::onSwatchClick(Ling::Button* btn)
{
	size_t idx = 0;
	for (size_t i = 0; i < btns.size(); i++) {
		if (btns[i] == btn) { idx = i; break; }
	}
	owner->onColorPicked(idx);
}

void ToolColorPanel::refreshSelection()
{
	const size_t selected = owner->selectedColorIdx();
	for (size_t i = 0; i < btns.size(); i++)
		applyActiveStyle(i, i == selected);
	paintCustomSwatch();
	refresh();
}

void ToolColorPanel::updatePosition(const RECT& workArea)
{
	if (!open_) return;
	dpi = owner->dpi;
	float triggerCX = 0.f, triggerBottom = 0.f;
	owner->getColorTriggerAnchor(triggerCX, triggerBottom);
	const float subPad = ToolbarTheme::shadowPad * owner->dpi;
	const float gap = ToolSub::mainGap;
	const float refTop = static_cast<float>(owner->y) + subPad;
	const float refBottom = static_cast<float>(owner->y) + owner->h - subPad;
	const bool preferBelow = !owner->tipDown();
	const auto place = ToolbarChrome::placeBubble(
		workArea, refTop, refBottom, w, h, gap,
		triggerCX - w / 2.f, triggerCX, preferBelow, tipDown_, arrowX, x, y);
	if (place.tipFlipped) applyTipDirection(place.tipDown);
	arrowX = place.arrowX;
	if (place.moved) setPosition(place.x, place.y);
	if (hwnd && owner->hwnd) {
		SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner->hwnd));
	}
	if (place.needPaint) refresh();
}

void ToolColorPanel::open(const RECT& workArea)
{
	dpi = owner->dpi;
	rebuildBtns();
	open_ = true;
	updatePosition(workArea);
	show();
}

void ToolColorPanel::closePanel()
{
	if (!open_) return;
	open_ = false;
	tip->hide();
	hide();
}
