#include "pch.h"
#include "PinMenu.h"
#include "MenuTheme.h"
#include "../Tool/IconCodes.h"
#include "../Tool/PropSlider.h"
#include "../App.h"

using namespace MenuTheme;

namespace {
	// 子菜单指示箭头：用「下拉」字形旋转 90° 得到右指箭头
	// （不新增字形，字重/尺寸与其它图标天然一致）。
	// 注意 Composition 的 CenterPoint 是"视觉局部坐标里的点"，必须等 layout 拿到真实 w/h 再设。
	struct ChevronRightLabel : Ling::Label
	{
		explicit ChevronRightLabel(Ling::WinBase* win) : Ling::Label(win) {}
	protected:
		void layout() override
		{
			Ling::Label::layout();
			visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.f });
			visual.RotationAngleInDegrees(-90.f);
		}
	};
}

std::unique_ptr<PinMenu> PinMenu::s_root;
HHOOK PinMenu::s_hook{ nullptr };

PinMenu::PinMenu(std::vector<Item> items) : Ling::WinBase(), items_(std::move(items))
{
	dpi = (float)GetDpiForSystem() / 96.f;
	if (dpi <= 0.f) dpi = 1.f;
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

PinMenu::~PinMenu() {}

void PinMenu::popup(std::vector<Item> items, POINT at)
{
	closeAll();
	s_root.reset(new PinMenu(std::move(items)));
	if (!s_root->hwnd) { s_root.reset(); return; }
	s_root->openAt(at);
	s_hook = SetWindowsHookExW(WH_MOUSE_LL, lowLevelProc, GetModuleHandleW(nullptr), 0);
}

bool PinMenu::isOpen() { return s_root != nullptr; }
void PinMenu::closeMenu() { closeAll(); }

void PinMenu::closeAll()
{
	if (s_hook) { UnhookWindowsHookEx(s_hook); s_hook = nullptr; }
	if (s_root) { s_root->closeTree(); s_root.reset(); }
}

void PinMenu::closeTree()
{
	if (submenu_) { submenu_->closeTree(); submenu_.reset(); }
	submenuOwner_ = -1;
	close();
}

bool PinMenu::treeHit(PinMenu* m, POINT pt)
{
	if (!m || !m->hwnd) return false;
	RECT rc{};
	GetWindowRect(m->hwnd, &rc);
	if (PtInRect(&rc, pt)) return true;
	return treeHit(m->submenu_.get(), pt);
}

LRESULT CALLBACK PinMenu::lowLevelProc(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION && s_root
		&& (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
		auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
		// 点在根菜单或其任一子菜单窗口内都不算「点外部」——交给对应窗口自身处理
		if (ms && !treeHit(s_root.get(), ms->pt))
			Ling::App::get()->dq.TryEnqueue([]() { closeAll(); });
	}
	return CallNextHookEx(nullptr, code, wp, lp);
}

float PinMenu::logicalHeight() const
{
	float h = kMenuPadY * 2.f;
	for (const auto& it : items_)
		h += it.separator ? (float)kSepH : (float)kRowH;
	return h;
}

void PinMenu::applySize()
{
	bool hasSlider = false;
	for (const auto& it : items_) if (it.slider) { hasSlider = true; break; }
	setSize(hasSlider ? (float)(kMenuW + 60) : (float)kMenuW, logicalHeight());
}

void PinMenu::openAt(POINT at)
{
	applySize();
	const int mw = (int)(w + 0.5f);
	const int mh = (int)(h + 0.5f);

	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
	const RECT& sg = mi.rcMonitor;

	int mx = at.x;
	int my = at.y;
	if (mx + mw > sg.right) mx = at.x - mw;
	if (my + mh > sg.bottom) my = at.y - mh;
	if (mx < sg.left) mx = sg.left;
	if (mx + mw > sg.right) mx = sg.right - mw;
	if (my < sg.top) my = sg.top;
	if (my + mh > sg.bottom) my = sg.bottom - mh;
	setPosition(mx, my);
	show();
	refresh();
}

void PinMenu::openSubmenu(int rowIndex, Ling::Button* ownerBtn)
{
	if (rowIndex < 0 || rowIndex >= (int)items_.size()) return;
	if (items_[rowIndex].submenu.empty()) { closeSubmenu(); return; }
	// 同一行的子菜单已经开着 → 保持，不重开
	if (submenu_ && submenuOwner_ == rowIndex) return;
	closeSubmenu();

	auto sub = std::unique_ptr<PinMenu>(new PinMenu(items_[rowIndex].submenu));
	if (!sub->hwnd) return;
	sub->applySize();

	const int mw = (int)(sub->w + 0.5f);
	const int mh = (int)(sub->h + 0.5f);
	// 默认贴父窗右缘、与父行顶对齐；右侧放不下就翻到父窗左侧
	const int parentLeft = x;
	const int parentRight = (int)std::lround(x + w);
	int mx = parentRight;
	int my = (int)std::lround(y + ownerBtn->y * dpi);

	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfoW(MonitorFromPoint(POINT{ mx, my }, MONITOR_DEFAULTTONEAREST), &mi);
	const RECT& sg = mi.rcMonitor;
	if (mx + mw > sg.right) mx = parentLeft - mw;
	if (mx < sg.left) mx = sg.left;
	if (my + mh > sg.bottom) my = sg.bottom - mh;
	if (my < sg.top) my = sg.top;

	sub->setPosition(mx, my);
	sub->show();
	sub->refresh();
	submenu_ = std::move(sub);
	submenuOwner_ = rowIndex;
}

void PinMenu::closeSubmenu()
{
	if (submenu_) { submenu_->closeTree(); submenu_.reset(); }
	submenuOwner_ = -1;
}

void PinMenu::onCreated()
{
	const Palette p = palette();
	body->setBg(0);
	auto* shell = body->makeChild<Ling::Node>();
	shell->setWidthPercent(100.f);
	shell->setFlexDirection(Ling::FlexDirection::Column);
	shell->setJustifyContent(Ling::Justify::Start);
	shell->setAlignItems(Ling::Align::Stretch);
	shell->setPadding(0.f, (float)kMenuPadY, 0.f, (float)kMenuPadY);
	shell->setBg(p.bg);
	shell->setBorder(1.f, p.border);
	shell->setBorderRadius((float)kMenuRadius);

	for (int idx = 0; idx < (int)items_.size(); ++idx) {
		auto& it = items_[idx];
		if (it.separator) {
			auto* line = shell->makeChild<Ling::Node>();
			line->setHeight(1.f);
			line->setMargin((float)kSepMarginX, (float)((kSepH - 1) / 2),
				(float)kSepMarginX, (float)((kSepH - 1) / 2));
			line->setBg(p.sep);
			continue;
		}

		if (it.slider) {
			auto* row = shell->makeChild<Ling::Node>();
			row->setHeight((float)kRowH);
			row->setMargin((float)kItemMarginX, 0.f, (float)kItemMarginX, 0.f);
			row->setPadding((float)kItemPadX, 0.f, (float)kItemPadX, 0.f);
			row->setFlexDirection(Ling::FlexDirection::Row);
			row->setAlignItems(Ling::Align::Center);
			if (!it.text.empty()) {
				auto* title = row->makeChild<Ling::Label>();
				title->setText(it.text);
				title->setFontSize(12.f);
				title->setColor(p.fg);
				title->setFlexShrink(0.f);
				title->setMargin(0.f, 0.f, 8.f, 0.f);
			}
			auto onSlide = it.onSlide;
			auto pair = PropSlider::mount(row, it.sliderMin, it.sliderMax, it.sliderValue,
				[onSlide](float v) { if (onSlide) onSlide(v); });
			if (pair.slider) {
				pair.slider->setTrackColor(p.hover);
				pair.slider->setFillColor(0x34C759FFu);
				pair.slider->setThumbColor(p.fg);
				pair.slider->setHoverThumbColor(p.fg);
			}
			if (pair.value) pair.value->setColor(p.fg);
			continue;
		}

		auto* btn = shell->makeChild<Ling::Button>();
		btn->setText(L"");
		btn->setHeight((float)kRowH);
		btn->setFlexDirection(Ling::FlexDirection::Row);
		btn->setJustifyContent(Ling::Justify::Start);
		btn->setAlignItems(Ling::Align::Center);
		btn->setPadding((float)kItemPadX, 0.f, (float)kItemPadX, 0.f);
		btn->setMargin((float)kItemMarginX, 0.f, (float)kItemMarginX, 0.f);
		btn->setBorderRadius((float)kItemRadius);
		btn->setBg(0);
		btn->setHoverBg(it.enabled ? p.hover : 0);

		auto* title = btn->makeChild<Ling::Label>();
		title->setText(it.text);
		title->setFontSize(12.f);
		title->setColor(it.enabled ? p.fg : 0x88888888u);
		title->setFlexShrink(0.f);

		// 撑开中间，把尾部标记（子菜单箭头 / 勾选）顶到最右
		auto* grow = btn->makeChild<Ling::Node>();
		grow->setFlexGrow(1.f);

		const bool hasSub = !it.submenu.empty();
		if (hasSub) {
			auto* arrow = btn->makeChild<ChevronRightLabel>();
			arrow->setWidth(16.f);
			arrow->setFontFamily(Icon::Family);
			arrow->setFontSize(12.f);
			arrow->setText(Icon::Dropdown);
			arrow->setColor(it.enabled ? p.fg : 0x88888888u);
			arrow->setFlexShrink(0.f);
		}
		else if (it.checkable) {
			auto* mark = btn->makeChild<Ling::Label>();
			mark->setWidth(16.f);
			mark->setFontFamily(Icon::Family);
			mark->setFontSize(14.f);
			mark->setText(it.checked ? Icon::Done : L"");
			mark->setColor(it.enabled ? p.fg : 0x88888888u);
			mark->setFlexShrink(0.f);
		}

		if (it.enabled && it.action) {
			auto action = it.action;
			btn->onClick.add([action](Ling::Button*) {
				Ling::App::get()->dq.TryEnqueue([action]() {
					closeAll();
					action();
				});
			});
		}

		if (it.enabled && hasSub) {
			// 按钮存活即代表本窗口存活（按钮归本窗口所有，窗口销毁时事件一并失效），故直接捕获 this
			auto* b = btn;
			btn->onEnter.add([this, idx, b](Ling::Button*) {
				openSubmenu(idx, b);
			});
		}
		else {
			// 悬停到普通行 → 收起已展开的子菜单
			btn->onEnter.add([this](Ling::Button*) {
				closeSubmenu();
			});
		}
	}
}

void PinMenu::layout()
{
	Ling::WinBase::layout();
}
