#include "pch.h"
#include "ToolPicker.h"
#include "../Lang.h"
#include "../Tip.h"
#include "IconCodes.h"
#include "ToolSub.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"

using Microsoft::WRL::ComPtr;

ToolPicker::ToolPicker() : Ling::WinBase()
{
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

ToolPicker::~ToolPicker()
{
	// WinBase 析构不销毁 HWND：不 close 就析构会留下"幽灵窗"，WndProc 重入已释放对象
	if (hwnd) { close(); hwnd = nullptr; }
}

void ToolPicker::onCreated()
{
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(
		Ling::Color(ToolbarTheme::background).getD2DColor(), brushBg.GetAddressOf());
	body->setBg(0);
	tip = std::make_unique<Tip>(this);
	canvas = body->makeChild<Ling::Canvas>();
	canvas->setPositionType(Ling::Position::Absolute);
	canvas->setSizePercent(100.f, 100.f);
	row = body->makeChild<Ling::Node>();
	row->setPositionType(Ling::Position::Absolute);
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setAlignItems(Ling::Align::Center);
	row->setPaddingLeft((ToolbarTheme::propBtnSize - ToolbarTheme::propIconInner) * 0.5f);
	row->setPaddingRight((ToolbarTheme::propBtnSize - ToolbarTheme::propIconInner) * 0.5f);
	applyRowInsets();
	onMouseMove.add([this](POINT pos) {
		if (pos.x == INT_MAX) {
			if (onHoverChange) onHoverChange(false);
		}
		else if (onHoverChange) {
			onHoverChange(true);
		}
	});
}

void ToolPicker::applyRowInsets()
{
	if (!row) return;
	ToolbarChrome::applyPropBubbleContentPad(row, preferAbove);
	row->setPosition(Ling::Edge::Left, ToolbarTheme::shadowPad);
	row->setPosition(Ling::Edge::Right, ToolbarTheme::shadowPad);
}

void ToolPicker::layout()
{
	Ling::WinBase::layout();
	if (!canvas || !brushBg) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	ToolbarChrome::paintPropBubble(ctx, w, h, dpi, brushBg.Get(), arrowX, preferAbove);
	canvas->finishPaint();
}

void ToolPicker::rebuildButtons()
{
	if (!row) return;
	row->removeAllChildren();
	pickBtns.clear();
	const float slot = Icon::Size + ToolbarTheme::propGap;
	const float marginH = (slot - ToolbarTheme::propIconInner) * 0.5f;
	const float marginV = (ToolbarTheme::propBtnSize - ToolbarTheme::propIconInner) * 0.5f;
	for (auto& item : items) {
		auto btn = row->makeChild<Ling::Button>();
		btn->setId(item.id);
		btn->setText(item.icon);
		btn->setFontFamily(Icon::Family);
		btn->setFontSize(Icon::Size);
		const bool sel = item.id == selectedId;
		btn->setBg(0);
		btn->setColor(sel ? Icon::ColorActive : Icon::ColorNormal);
		btn->setHoverColor(sel ? Icon::ColorActive : Icon::ColorNormal);
		btn->setSize(ToolbarTheme::propIconInner, ToolbarTheme::propIconInner);
		btn->setMargin(marginH, marginV, marginH, marginV);
		btn->setFlexGrow(0.f);
		btn->setFlexShrink(0.f);
		btn->setBorderRadius(ToolbarTheme::hoverRadius);
		btn->setHoverBg(ToolbarTheme::hoverBg);
		btn->setAlignItems(Ling::Align::Center);
		btn->setJustifyContent(Ling::Justify::Center);
		if (item.tipKey) tip->bind(btn, Lang::get(item.tipKey));
		btn->onClick.add([this, item](Ling::Button*) {
			if (onPick) onPick(item.id);
			hidePicker();
		});
		pickBtns.push_back(btn);
	}
}

void ToolPicker::placeNearAnchor()
{
	if (!toolbar || !anchor || !hwnd) return;
	POINT center{ (LONG)(anchor->x + anchor->w * 0.5f), (LONG)(anchor->y + anchor->h * 0.5f) };
	ClientToScreen(toolbar->hwnd, &center);
	RECT tb{ toolbar->x, toolbar->y, toolbar->x + (LONG)toolbar->w, toolbar->y + (LONG)toolbar->h };
	const RECT wa = ToolbarChrome::workAreaNear(tb);
	const float pad = ToolbarTheme::shadowPad * dpi;
	const float gap = ToolSub::mainGap;
	const float refTop = (float)tb.top + pad;
	const float refBottom = (float)tb.bottom - pad;
	const float ax = (float)center.x;
	const auto place = ToolbarChrome::placeBubble(
		wa, refTop, refBottom, w, h, gap,
		ax - w * 0.5f, ax, false, preferAbove, arrowX, x, y);
	preferAbove = place.tipDown;
	arrowX = place.arrowX;
	applyRowInsets();
	if (place.moved) setPosition(place.x, place.y);
	if (place.needPaint) refresh();
}

void ToolPicker::showFor(Ling::WinBase* bar, Ling::Button* anchorBtn, const std::vector<ToolPickItem>& list,
	const std::wstring& selected, PickFn pick)
{
	if (list.empty() || !anchorBtn) return;
	onPick = std::move(pick);
	// 同锚点已开着：只更新选中/回调，避免移回图标时重建导致「重复弹」
	if (isVisible && toolbar == bar && anchor == anchorBtn) {
		if (selectedId != selected) {
			selectedId = selected;
			rebuildButtons();
			refresh();
		}
		placeNearAnchor();
		return;
	}
	toolbar = bar;
	anchor = anchorBtn;
	items = list;
	selectedId = selected;
	const float n = (float)items.size();
	const float edge = (ToolbarTheme::propBtnSize - ToolbarTheme::propIconInner) * 0.5f; // 与主栏上下一致
	const float slot = Icon::Size + ToolbarTheme::propGap;
	setSize(edge * 2.f + n * slot + ToolbarTheme::shadowPad * 2.f,
		ToolbarTheme::propBtnSize + ToolbarTheme::caretSize + ToolbarTheme::shadowPad * 2.f);
	dpi = bar->dpi;
	rebuildButtons();
	placeNearAnchor();
	if (hwnd && bar->hwnd) {
		SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(bar->hwnd));
	}
	show();
	isVisible = true;
	refresh();
}

void ToolPicker::hidePicker()
{
	if (!isVisible) return;
	if (tip) tip->hide();
	hide();
	isVisible = false;
	anchor = nullptr;
	toolbar = nullptr;
}

void ToolPicker::reposition()
{
	if (!isVisible) return;
	placeNearAnchor();
	refresh();
}

bool ToolPicker::hitTestScreen(POINT screenPt) const
{
	if (!isVisible || !hwnd) return false;
	RECT r{};
	GetWindowRect(hwnd, &r);
	return PtInRect(&r, screenPt) != 0;
}

bool ToolPicker::hitKeepOpen(POINT screenPt) const
{
	if (!isVisible || !hwnd || !toolbar) return false;
	if (hitTestScreen(screenPt)) return true;
	RECT tb{}, pk{};
	GetWindowRect(toolbar->hwnd, &tb);
	GetWindowRect(hwnd, &pk);
	if (PtInRect(&tb, screenPt)) return true;
	// 主栏↔气泡竖直通道：宽度取两者重叠，并至少盖住锚点一带；高度含空隙
	LONG left = (std::max)(tb.left, pk.left);
	LONG right = (std::min)(tb.right, pk.right);
	if (anchor) {
		POINT c{ (LONG)(anchor->x + anchor->w * 0.5f), (LONG)(anchor->y + anchor->h * 0.5f) };
		ClientToScreen(toolbar->hwnd, &c);
		const LONG half = (std::max)((LONG)(anchor->w * 0.5f) + 8, (right - left) / 2);
		left = (std::min)(left, c.x - half);
		right = (std::max)(right, c.x + half);
	}
	if (right <= left) return false;
	RECT gap{};
	if (pk.bottom <= tb.top)
		gap = { left, pk.bottom, right, tb.top };
	else if (tb.bottom <= pk.top)
		gap = { left, tb.bottom, right, pk.top };
	else
		return false;
	// 空隙很窄时再略扩 2px，避免 WM_MOUSELEAVE 采样落在缝外
	InflateRect(&gap, 0, 2);
	return PtInRect(&gap, screenPt) != 0;
}
