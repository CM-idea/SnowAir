#include "pch.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolNestedPanel.h"
#include "ToolSub.h"
#include "IconCodes.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"
#include "PropSlider.h"
#include "SerialEmoji.h"

using namespace Microsoft::WRL;

ToolNestedPanel::ToolNestedPanel(ToolSub* owner) : Ling::WinBase(), owner(owner)
{
	dpi = owner->dpi;
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

ToolNestedPanel::~ToolNestedPanel() = default;

void ToolNestedPanel::onCreated()
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

void ToolNestedPanel::layout()
{
	Ling::WinBase::layout();
	if (!chromeCanvas) return;
	auto ctx = chromeCanvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	paintChrome(ctx);
	chromeCanvas->finishPaint();
}

void ToolNestedPanel::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void ToolNestedPanel::paintChrome(ID2D1DeviceContext* ctx)
{
	ToolbarChrome::paintPropBubble(ctx, w, h, dpi, brushBg.Get(), arrowX, tipDown_);
}

void ToolNestedPanel::applyTipDirection(bool tipDown)
{
	tipDown_ = tipDown;
	ToolbarChrome::applyPropBubbleContentPad(contentNode, tipDown_);
}

void ToolNestedPanel::clearContent()
{
	if (contentNode) contentNode->removeAllChildren();
}

void ToolNestedPanel::prepareContentRow()
{
	if (!contentNode) return;
	contentNode->setFlexDirection(Ling::FlexDirection::Row);
	contentNode->setFlexWrap(Ling::Wrap::NoWrap);
	contentNode->setAlignItems(Ling::Align::Center);
	contentNode->setPaddingLeft(ToolbarTheme::propPad);
	contentNode->setPaddingRight(ToolbarTheme::propPad);
	contentNode->setPaddingTop(0.f);
	contentNode->setPaddingBottom(0.f);
}

Ling::Button* ToolNestedPanel::makeIconBtn(const wchar_t* icon, const std::wstring& tipText, bool active,
	std::function<void()> onClick)
{
	auto btn = contentNode->makeChild<Ling::Button>();
	const float slot = Icon::Size + ToolbarTheme::propGap;
	const float marginH = (slot - iconInner) * 0.5f;
	const float marginV = (btnSize - iconInner) * 0.5f;
	btn->setSize(iconInner, iconInner);
	btn->setMargin(marginH, marginV, marginH, marginV);
	btn->setText(icon);
	btn->setFontFamily(Icon::Family);
	btn->setFontSize(Icon::Size);
	btn->setBg(0);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setBorderRadius(ToolbarTheme::hoverRadius);
	btn->setColor(active ? Icon::ColorActive : Icon::ColorNormal);
	btn->setHoverColor(active ? Icon::ColorActive : Icon::ColorNormal);
	tip->bind(btn, tipText);
	btn->onClick.add([onClick](Ling::Button*) { if (onClick) onClick(); });
	return btn;
}

void ToolNestedPanel::addSlider(float minV, float maxV, float val, std::function<void(float)> onChange)
{
	PropSlider::mount(contentNode, minV, maxV, val, std::move(onChange));
}

void ToolNestedPanel::finishOpen(const RECT& workArea, Ling::Button* anchor, float logicW, float logicH)
{
	if (logicH <= 0.f) logicH = btnSize + marginTop + ToolbarTheme::shadowPad;
	setSize(logicW, logicH);
	anchorBtn = anchor;
	workArea_ = workArea;
	open_ = true;
	place(workArea, anchor);
	show();
	refresh();
}

void ToolNestedPanel::place(const RECT& workArea, Ling::Button* anchor)
{
	dpi = owner->dpi;
	float ax = (float)owner->x + (float)owner->w * 0.5f;
	if (anchor) {
		// 与色板一致：节点坐标已是窗口客户区物理像素，勿再 *dpi
		ax = (float)owner->x + (anchor->x + anchor->w * 0.5f);
	}
	const float subPad = ToolbarTheme::shadowPad * owner->dpi;
	const float gap = ToolSub::mainGap;
	const float refTop = (float)owner->y + subPad;
	const float refBottom = (float)owner->y + owner->h - subPad;
	const bool preferBelow = !owner->tipDown();
	const auto place = ToolbarChrome::placeBubble(
		workArea, refTop, refBottom, w, h, gap,
		ax - w / 2.f, ax, preferBelow, tipDown_, arrowX, x, y);
	if (place.tipFlipped) applyTipDirection(place.tipDown);
	arrowX = place.arrowX;
	if (place.moved) setPosition(place.x, place.y);
	if (hwnd && owner->hwnd) {
		SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner->hwnd));
	}
	needPaint_ = place.needPaint;
}

void ToolNestedPanel::updatePosition(const RECT& workArea)
{
	if (!open_) return;
	workArea_ = workArea;
	needPaint_ = false;
	place(workArea, anchorBtn);
	if (needPaint_) refresh();
}

void ToolNestedPanel::closePanel()
{
	open_ = false;
	kind_ = Kind::None;
	anchorBtn = nullptr;
	hide();
	clearContent();
}

void ToolNestedPanel::openCorner(const RECT& workArea, Ling::Button* anchor)
{
	closePanel();
	kind_ = Kind::Corner;
	clearContent();
	prepareContentRow();
	addSlider(0.f, 40.f, owner->cornerRadius, [this](float nv) {
		owner->cornerRadius = nv;
		if (nv > 0.f) owner->lastCornerRadius = nv;
		owner->refreshNestedTrigger();
		owner->notifyStyleChanged();
	});
	finishOpen(workArea, anchor,
		ToolbarTheme::propPad * 2.f + PropSlider::rowLogicW() + ToolbarTheme::shadowPad * 2.f);
}

void ToolNestedPanel::openWmAngle(const RECT& workArea, Ling::Button* anchor)
{
	if (open_ && kind_ == Kind::WmAngle) {
		closePanel();
		owner->refreshNestedTrigger();
		return;
	}
	closePanel();
	kind_ = Kind::WmAngle;
	clearContent();
	prepareContentRow();
	addSlider(0.f, 360.f, owner->watermarkAngle, [this](float nv) {
		owner->watermarkAngle = nv;
		owner->notifyStyleChanged();
	});
	finishOpen(workArea, anchor,
		ToolbarTheme::propPad * 2.f + PropSlider::rowLogicW() + ToolbarTheme::shadowPad * 2.f);
	owner->refreshNestedTrigger();
}

void ToolNestedPanel::rebuildPatinaWmRow()
{
	clearContent();
	prepareContentRow();
	struct Plan { int id; const wchar_t* icon; const wchar_t* tipKey; };
	const Plan plans[] = {
		{ 2, Icon::CornerMidBR, L"tool.patinaWmCB" },
		{ 1, Icon::CornerBR, L"tool.patinaWmBR" },
		{ 0, Icon::Right, L"tool.patinaWmRight" },
	};
	for (auto& pl : plans) {
		makeIconBtn(pl.icon, Lang::get(pl.tipKey), owner->patinaPlan == pl.id, [this, id = pl.id]() {
			owner->patinaPlan = id;
			owner->isPatinaWatermark = true;
			rebuildPatinaWmRow();
			refresh();
			owner->refreshNestedTrigger();
			owner->notifyStyleChanged();
		});
	}
	addSlider(8.f, 80.f, (float)owner->patinaWmSize, [this](float nv) {
		owner->patinaWmSize = (int)std::round(nv);
		owner->notifyStyleChanged();
	});
}

void ToolNestedPanel::openPatinaWm(const RECT& workArea, Ling::Button* anchor)
{
	// 对齐 Tauri2：二级栏已开再点 → 关水印；否则开启并弹出（默认 plan=0 右下）
	if (owner->isPatinaWatermark && open_ && kind_ == Kind::PatinaWm) {
		owner->isPatinaWatermark = false;
		owner->refreshNestedTrigger();
		owner->notifyStyleChanged();
		closePanel();
		return;
	}

	closePanel();
	kind_ = Kind::PatinaWm;
	owner->isPatinaWatermark = true;
	owner->patinaPlan = 0;
	rebuildPatinaWmRow();
	finishOpen(workArea, anchor,
		ToolbarTheme::propPad * 2.f
		+ (Icon::Size + ToolbarTheme::propGap) * 3.f
		+ PropSlider::rowLogicW() + ToolbarTheme::shadowPad * 2.f);
	owner->refreshNestedTrigger();
	owner->notifyStyleChanged();
}

void ToolNestedPanel::rebuildEmojiGrid()
{
	clearContent();
	if (!contentNode) return;
	// QT：7 列 × 40 格，间距 6，边距 14/12 → 宽约 344、内容高约 202
	constexpr float cell{ 40.f };
	constexpr float gap{ 6.f };
	constexpr float padX{ 14.f };
	constexpr float padY{ 12.f };
	contentNode->setFlexDirection(Ling::FlexDirection::Row);
	contentNode->setFlexWrap(Ling::Wrap::Wrap);
	contentNode->setAlignItems(Ling::Align::FlexStart);
	contentNode->setPaddingLeft(padX);
	contentNode->setPaddingRight(padX);
	contentNode->setPaddingTop(padY);
	contentNode->setPaddingBottom(padY);

	const std::wstring current = owner->serialEmoji.empty() ? SerialEmoji::Default : owner->serialEmoji;
	const int rows = (SerialEmoji::kCount + SerialEmoji::kCols - 1) / SerialEmoji::kCols;
	for (int i = 0; i < SerialEmoji::kCount; ++i) {
		const wchar_t* emoji = SerialEmoji::kList[i];
		const int col = i % SerialEmoji::kCols;
		const int row = i / SerialEmoji::kCols;
		auto btn = contentNode->makeChild<Ling::Button>();
		btn->setSize(cell, cell);
		btn->setMargin(0.f, 0.f, col + 1 < SerialEmoji::kCols ? gap : 0.f, row + 1 < rows ? gap : 0.f);
		btn->setFlexGrow(0.f);
		btn->setFlexShrink(0.f);
		btn->setText(emoji);
		btn->setFontFamily(L"Segoe UI Emoji");
		btn->setFontSize(30.f);
		// 彩色 emoji 用近黑作非彩色回退色即可，勿用灰/绿 tint
		btn->setColor(0x000000FFu);
		btn->setHoverColor(0x000000FFu);
		btn->setBg(current == emoji ? ToolbarTheme::hoverBg : 0);
		btn->setHoverBg(ToolbarTheme::hoverBg);
		btn->setBorderRadius(ToolbarTheme::hoverRadius);
		btn->setAlignItems(Ling::Align::Center);
		btn->setJustifyContent(Ling::Justify::Center);
		btn->onClick.add([this, emoji](Ling::Button*) {
			owner->serialEmoji = emoji;
			owner->updateEmojiTriggerGlyph();
			closePanel();
			owner->refreshNestedTrigger();
			owner->notifyStyleChanged();
		});
	}
}

void ToolNestedPanel::openEmoji(const RECT& workArea, Ling::Button* anchor)
{
	if (open_ && kind_ == Kind::Emoji) {
		closePanel();
		owner->refreshNestedTrigger();
		return;
	}
	closePanel();
	kind_ = Kind::Emoji;
	rebuildEmojiGrid();
	constexpr float cell{ 40.f };
	constexpr float gap{ 6.f };
	constexpr float padX{ 14.f };
	constexpr float padY{ 12.f };
	const float logicW = padX * 2.f + SerialEmoji::kCols * cell + (SerialEmoji::kCols - 1) * gap;
	const int rows = (SerialEmoji::kCount + SerialEmoji::kCols - 1) / SerialEmoji::kCols;
	const float logicH = ToolbarTheme::caretSize + padY * 2.f
		+ rows * cell + (rows > 0 ? (rows - 1) * gap : 0.f);
	finishOpen(workArea, anchor, logicW, logicH);
	owner->refreshNestedTrigger();
}
