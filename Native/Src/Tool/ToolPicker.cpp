#include "pch.h"
#include "ToolPicker.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "IconCodes.h"
#include "ToolSub.h"
#include "ToolInitialDefs.h"   // 初始属性默认形态 → 气泡图标
#include "ToolIconCompose.h"   // 复合图标（马赛克切模糊时叠两层）
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"
#include "DpiUtil.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
	std::vector<ToolPicker*> g_toolPickers;
}

ToolPicker::ToolPicker() : Ling::WinBase()
{
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
	g_toolPickers.push_back(this);
	static bool sizeHookInstalled = false;
	if (!sizeHookInstalled && Setting::get()) {
		sizeHookInstalled = true;
		Setting::get()->addToolbarSizeListener([] {
			for (auto* picker : g_toolPickers) picker->applyToolbarSize();
		});
	}
}

ToolPicker::~ToolPicker()
{
	g_toolPickers.erase(std::remove(g_toolPickers.begin(), g_toolPickers.end(), this),
		g_toolPickers.end());
	// WinBase 析构不销毁 HWND：不 close 就析构会留下"幽灵窗"，WndProc 重入已释放对象
	if (hwnd) { close(); hwnd = nullptr; }
}

void ToolPicker::onCreated()
{
	const float actual = DpiUtil::dpiForWindow(hwnd);
	if (std::fabs(actual - dpi) > 0.01f) {
		dpi = actual;
		body->applyDpiChange();
	}
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
	const int sizeMode = Setting::get() ? Setting::get()->getToolbarSize() : 1;
	ToolbarTheme::propGap = (sizeMode == 0) ? 18.f : (sizeMode == 2 ? 14.f : 16.f);
	const float edge = (sizeMode == 0) ? 12.f : (sizeMode == 2 ? 9.f : 10.f);
	row->setPaddingLeft(edge);
	row->setPaddingRight(edge);
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

void ToolPicker::applyToolbarSize()
{
	if (!row) return;
	const int mode = Setting::get() ? Setting::get()->getToolbarSize() : 1;
	ToolbarTheme::propBtnSize = (mode == 0) ? 48.f : (mode == 2 ? 39.f : 42.f);
	ToolbarTheme::propIconInner = ToolbarTheme::propBtnSize - 8.f;
	ToolbarTheme::iconSize = (mode == 0) ? 24.f : (mode == 2 ? 21.f : 22.f);
	ToolbarTheme::propGap = (mode == 0) ? 18.f : (mode == 2 ? 14.f : 16.f);
	const float edge = (mode == 0) ? 12.f : (mode == 2 ? 9.f : 10.f);
	const float n = static_cast<float>(items.size());
	const float slot = ToolbarTheme::iconSize + ToolbarTheme::propGap;
	row->setPaddingLeft(edge);
	row->setPaddingRight(edge);
	setSize(edge * 2.f + n * slot + ToolbarTheme::shadowPad * 2.f,
		ToolbarTheme::propBtnSize + ToolbarTheme::caretSize + ToolbarTheme::shadowPad * 2.f);
	rebuildButtons();
	placeNearAnchor();
	layout();
	refresh();
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
	const float slot = ToolbarTheme::iconSize + ToolbarTheme::propGap;
	const float marginH = (slot - ToolbarTheme::propIconInner) * 0.5f;
	const float marginV = (ToolbarTheme::propBtnSize - ToolbarTheme::propIconInner) * 0.5f;
	for (auto& item : items) {
		auto btn = row->makeChild<Ling::Button>();
		btn->setId(item.id);
		const bool sel = item.id == selectedId;
		const uint32_t color = sel ? Icon::ColorActive : Icon::ColorNormal;
		// 图标跟着「初始属性」里的默认形态走（文案仍是工具名）
		const auto* def = ToolInitial::find(ToolInitial::defIdOfSlot(item.id));
		const auto* form = def ? ToolInitial::currentForm(*def) : nullptr;
		if (form && form->overlayGlyph) {
			btn->setText(L"");
			ToolIcon::overlayGlyph(btn, form->glyph, form->overlayGlyph, ToolbarTheme::iconSize,
				color, ToolIcon::withAlpha(color, 0x40));
		}
		else {
			std::wstring icon = item.icon ? item.icon : L"";
			if (form) {
				const std::wstring formIcon = ToolInitial::iconForSlot(item.id);
				if (!formIcon.empty()) icon = formIcon;
			}
			btn->setText(icon);
		}
		btn->setFontFamily(Icon::Family);
		btn->setFontSize(ToolbarTheme::iconSize);
		btn->setBg(0);
		btn->setColor(color);
		btn->setHoverColor(color);
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

	// 窗口刚建出来时默认在主屏（可能是 100%），而气泡要显示在锚点所在屏（可能是 300%）。
	// 若直接按目标 dpi 算好物理尺寸再搬过去，系统会认为窗口是从旧 dpi 搬来的，
	// 再按「建议矩形」乘一次缩放 —— 首次悬停气泡变成 N 倍大、第二次才恢复，就是这么来的。
	// 顺序改成：先搬到锚点所在屏让 WM_DPICHANGED 落定，再用真实 dpi 重算尺寸。
	POINT anchorScreen{ (LONG)(anchorBtn->x + anchorBtn->w * 0.5f),
		(LONG)(anchorBtn->y + anchorBtn->h * 0.5f) };
	if (bar->hwnd) ClientToScreen(bar->hwnd, &anchorScreen);
	if (std::fabs(DpiUtil::dpiForPoint(anchorScreen) - dpi) > 0.01f) {
		// 已经显示着跨屏换锚点：先藏起来，别让用户看到搬家过程
		if (isVisible) Ling::WinBase::hide();
		ToolbarChrome::settleDpiAt(this, anchorScreen.x, anchorScreen.y);
	}

	// 用锚点所在显示器的缩放排版。不能抄整个工具条窗口的 dpi：工具条横跨两屏时
	// MonitorFromWindow 只按面积最大的那块算，气泡却贴着另一个屏上的按钮。
	const float targetDpi = DpiUtil::dpiForPoint(anchorScreen);
	if (std::fabs(targetDpi - dpi) > 0.01f) {
		dpi = targetDpi;
		// 只改成员 dpi 不会重推已经建好的 row / canvas 内边距，必须让 body 重排一次
		body->applyDpiChange();
	}
	applyToolbarSize();
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
