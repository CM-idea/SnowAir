#include "pch.h"
#include <cmath>
#include <vector>
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

	D2D1_COLOR_F sampleConic(float t)
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
	// WinBase 析构不销毁 HWND：本窗由 ToolSub 持有时只被 hide()/closePanel()，从没 DestroyWindow，
	// ToolSub 析构后就会留下一个隐藏的"幽灵窗"。此后系统一旦给它发 WM_DPICHANGED（睡眠唤醒、
	// 改缩放时会有），窗口过程会重入已经释放的 ToolColorPanel → 读到的 body 为空 → 崩溃在
	// Ling::Node::applyDpiChange（0xC0000005 / 0xC000041D，见 issue #76）。析构时必须真正关窗。
	if (hwnd) { close(); hwnd = nullptr; }
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
	ToolbarChrome::paintPropBubble(ctx, w, h, dpi, brushBg.Get(), arrowX, tipDown_,
		borderless_ ? 0.f : ToolbarTheme::borderWidth);
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

void PaintCustomColorSwatch(Ling::Canvas* canvas, float dpi)
{
	(void)dpi;   // 尺寸/圆角都由 Canvas 自身与像素尺寸决定，这里不再需要 dpi
	if (!canvas) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	const float wPx = canvas->w;
	const float hPx = canvas->h;
	ctx->Clear(D2D1::ColorF(0, 0.f));
	if (wPx <= 0.f || hPx <= 0.f) {
		canvas->finishPaint();
		return;
	}
	// 逐像素按角度取锥形渐变的饱和色，不再画几十个抗锯齿扇形：
	// 扇形的抗锯齿边缘互相叠加会混出深色接缝，18px 的小色块看起来又灰又有噪点。
	// 圆角由 Canvas 自身的 Composition 圆角裁剪负责，这里只需铺满整块位图。
	const UINT W = (UINT)std::lround(wPx);
	const UINT H = (UINT)std::lround(hPx);
	constexpr float pi = 3.14159265358979f;
	const float cx = W * 0.5f;
	const float cy = H * 0.5f;
	std::vector<uint32_t> pixels(size_t(W) * H);
	for (UINT y = 0; y < H; ++y) {
		const float dy = (float)y + 0.5f - cy;
		for (UINT x = 0; x < W; ++x) {
			const float dx = (float)x + 0.5f - cx;
			const float t = (std::atan2(dy, dx) + pi) / (2.f * pi);
			const auto c = sampleConic(t);
			const uint32_t r = (uint32_t)std::lround((std::clamp)(c.r, 0.f, 1.f) * 255.f);
			const uint32_t g = (uint32_t)std::lround((std::clamp)(c.g, 0.f, 1.f) * 255.f);
			const uint32_t b = (uint32_t)std::lround((std::clamp)(c.b, 0.f, 1.f) * 255.f);
			pixels[size_t(y) * W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;   // BGRA
		}
	}
	ComPtr<ID2D1Bitmap> bmp;
	ctx->CreateBitmap(D2D1::SizeU(W, H), pixels.data(), W * sizeof(uint32_t),
		D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
			D2D1_ALPHA_MODE_IGNORE)), bmp.GetAddressOf());
	if (bmp)
		ctx->DrawBitmap(bmp.Get(), D2D1::RectF(0.f, 0.f, wPx, hPx), 1.f,
			D2D1_INTERPOLATION_MODE_LINEAR);
	canvas->finishPaint();
}

void ToolColorPanel::paintCustomSwatch()
{
	PaintCustomColorSwatch(customCanvas, dpi);
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
		const float slot = ToolbarTheme::iconSize + ToolbarTheme::propGap;
		const float marginH{ (slot - ToolbarTheme::propIconInner) * 0.5f };
		const float marginV{ (ToolbarTheme::propBtnSize - ToolbarTheme::propIconInner) * 0.5f };
		btn->setSize(ToolbarTheme::propIconInner, ToolbarTheme::propIconInner);
		btn->setMargin(marginH, marginV, marginH, marginV);
		btn->setFlexGrow(0.f);
		btn->setFlexShrink(0.f);
		btn->setBorderRadius(hoverRadius);
		btn->setHoverBg(ToolbarTheme::hoverBg);
		btn->setAlignItems(Ling::Align::Center);
		btn->setJustifyContent(Ling::Justify::Center);
		btn->onClick.add([this](Ling::Button* b) { onSwatchClick(b); });
		if (i < ARRAYSIZE(colorNames))
			tip->bind(btn, Lang::get(wfmt(L"color.%s", colorNames[i])));
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
		+ (ToolbarTheme::iconSize + ToolbarTheme::propGap) * static_cast<float>(colors.size())
		+ ToolbarTheme::shadowPad * 2.f;
	const float logicH = ToolbarTheme::propBtnSize + marginTop + ToolbarTheme::shadowPad;
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
	const float gap = ToolbarTheme::propBubbleGap;
	// 颜色面板是三级弹窗：贴着所属二级栏的外侧展开；下方放不下就翻到上方，
	// 不覆盖二级栏本身。
	const RECT parent = owner->colorPanelParentBodyRect();
	const float refTop = static_cast<float>(parent.top) + subPad;
	const float refBottom = static_cast<float>(parent.bottom) - subPad;
	const bool preferBelow = owner->colorPanelPreferBelow();
	const auto place = ToolbarChrome::placeBubble(
		workArea, refTop, refBottom, w, h, gap,
		triggerCX - w / 2.f, triggerCX, preferBelow, tipDown_, arrowX, x, y);
	// 换向后无条件同步箭头方向，不能只在“检测到翻转”时更新
	applyTipDirection(place.tipDown);
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
	// 三级弹窗可能由二级栏打开；确保它在最上层，不被捕获遮罩或二级栏压住
	if (hwnd) {
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void ToolColorPanel::closePanel()
{
	if (!open_) return;
	open_ = false;
	tip->hide();
	hide();
}
