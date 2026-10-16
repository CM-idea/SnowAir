#include "pch.h"
#include <uxtheme.h>
#include <vssym32.h>
#include "Tray.h"
#include "TrayIcon.h"
#include "App.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinSetting.h"
#include "Setting.h"
#include "Win/SettingTheme.h"
#include "Win/MenuTheme.h"
#include "Util.h"
#include <string>
#include <vector>

namespace {
	static std::unique_ptr<Tray> trayIns;

	// 托盘菜单：用户要 180 宽 + 圆角 + 原间距/配色 + 右侧显示已设热键。
	// 系统 HMENU 宽度不可控，自绘 HMENU 又拿不到 Win11 圆角，故自建圆角弹层；
	// 配色/字体全部取自菜单主题（GetThemeColor），行高按系统菜单（30）。不抢前台，鼠标钩子点外关闭。
	enum MenuCmd : UINT {
		kCmdCapture = 1, kCmdCaptureFullscreen, kCmdRecord, kCmdDemoCanvas,
		kCmdQuickTranslate, kCmdHistory, kCmdOpenDir, kCmdSettings, kCmdExit,
	};

	struct MenuRow { UINT id; const wchar_t* langKey; const wchar_t* shortcutId; };

	// 功能尚未实装的托盘项：置灰且不可点，与「全局热键」页的"功能开发中"一致。
	// 快捷翻译实装后删除本函数的判断即可，行与命令派发都不必改
	bool trayRowDisabled(UINT cmd) { return cmd == kCmdQuickTranslate; }

	const MenuRow kMenuRows[] = {
		{ kCmdCapture,           L"tray.capture",           L"capture" },
		{ kCmdCaptureFullscreen, L"tray.captureFullscreen", L"capture_fullscreen" },
		{ kCmdRecord,            L"tray.record",            L"record" },
		{ kCmdDemoCanvas,        L"tray.demoCanvas",        L"fullscreen_canvas" },
		{ 0,                     nullptr,                   nullptr },
		{ kCmdQuickTranslate,    L"tray.quickTranslate",    nullptr },
		{ 0,                     nullptr,                   nullptr },
		{ kCmdHistory,           L"tray.history",           L"open_history" },
		{ kCmdOpenDir,           L"tray.openDir",           L"open_screenshot_dir" },
		{ 0,                     nullptr,                   nullptr },
		{ kCmdSettings,          L"tray.setting",           L"show_main" },
		{ kCmdExit,              L"tray.exit",              nullptr },
	};

	// 菜单外观（配色/尺寸）统一由 Win/MenuTheme.h 提供，托盘与贴图右键菜单共用。
	using MenuPalette = MenuTheme::Palette;
	using namespace MenuTheme;
	inline MenuPalette menuPalette() { return MenuTheme::palette(); }

	void runTrayCommand(UINT cmd);
	void closeTrayMenu();

	class TrayMenuWindow : public Ling::WinBase
	{
	public:
		TrayMenuWindow() : Ling::WinBase()
		{
			dpi = (float)GetDpiForSystem() / 96.f;
			if (dpi <= 0.f) dpi = 1.f;
			createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
		}
		~TrayMenuWindow() { if (hwnd) close(); }

		float logicalHeight() const
		{
			float h = kMenuPadY * 2.f;
			for (const auto& row : kMenuRows) h += (row.id == 0 ? (float)kSepH : (float)kRowH);
			return h;
		}

		void openAt(POINT pt)
		{
			setSize((float)kMenuW, logicalHeight());
			const int mw = (int)(w + 0.5f);
			const int mh = (int)(h + 0.5f);

			// 菜单左下角贴光标、底边在光标上方 2px。
			// ★ 钳制要用【整屏 rcMonitor】而不是【工作区 rcWork】：托盘图标在任务栏里、光标落在
			//   任务栏上，用 rcWork（不含任务栏）钳制会把菜单顶到任务栏上沿之上，右键时菜单与
			//   光标之间就凭空多出一整条任务栏的高度 —— 看起来"没在鼠标位置弹出"。
			MONITORINFO mi{ sizeof(MONITORINFO) };
			GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
			const RECT& sg = mi.rcMonitor;

			constexpr int kGap{ 2 };
			int mx = pt.x;                  // 左边缘 = 光标 x
			int my = pt.y - mh - kGap;      // 底边缘 = 光标上方 2px
			if (mx + mw > sg.right) mx = pt.x - mw;   // 右边放不下 → 翻到光标左侧
			if (my < sg.top) my = pt.y + kGap;        // 上面放不下 → 翻到光标下方
			if (mx < sg.left) mx = sg.left;
			if (mx + mw > sg.right) mx = sg.right - mw;   // 菜单比屏还宽时贴右
			if (my < sg.top) my = sg.top;
			if (my + mh > sg.bottom) my = sg.bottom - mh;
			setPosition(mx, my);
			show();
			refresh();
		}
	protected:
		void onCreated() override
		{
			const MenuPalette p = menuPalette();
			body->setBg(0);
			auto* shell = body->makeChild<Ling::Node>();
			shell->setWidthPercent(100.f);
			shell->setFlexDirection(Ling::FlexDirection::Column);
			shell->setJustifyContent(Ling::Justify::Start);
			// 交叉轴拉伸：条目/分隔线靠 stretch 拿到「菜单宽 − 左右外边距」，
			// 不能再用 width:100%（百分比不含 margin，会往右溢出被窗口裁掉 → 左右不对称）
			shell->setAlignItems(Ling::Align::Stretch);
			shell->setPadding(0.f, (float)kMenuPadY, 0.f, (float)kMenuPadY);
			shell->setBg(p.bg);
			shell->setBorder(1.f, p.border);
			shell->setBorderRadius((float)kMenuRadius);

			// 置灰项的"灰"：同前景色、只把 alpha 压到 0x66（配色跟着主题走，别写死颜色）
			const uint32_t fgDisabled = (p.fg & 0xFFFFFF00u) | 0x66u;
			for (const auto& row : kMenuRows) {
				if (row.id == 0) {
					auto* line = shell->makeChild<Ling::Node>();
					line->setHeight(1.f);
					line->setMargin((float)kSepMarginX, (float)((kSepH - 1) / 2),
						(float)kSepMarginX, (float)((kSepH - 1) / 2));
					line->setBg(p.sep);
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
				const bool disabled = trayRowDisabled(row.id);
				// 置灰行悬停也不高亮（悬停底色取 0 = 与常态同色），点也不派发（不进 rows）
				btn->setHoverBg(disabled ? 0u : p.hover);

				auto* title = btn->makeChild<Ling::Label>();
				title->setText(Lang::get(row.langKey));
				title->setFontSize(12.f);
				title->setColor(disabled ? fgDisabled : p.fg);
				title->setFlexShrink(0.f);

				auto* grow = btn->makeChild<Ling::Node>();
				grow->setFlexGrow(1.f);

				if (row.shortcutId) {
					std::wstring chord = Setting::get()->getShortcutKey(row.shortcutId);
					if (!chord.empty()) {
						auto* key = btn->makeChild<Ling::Label>();
						key->setText(chord);
						key->setFontSize(12.f);
						key->setColor(p.fg);
						key->setFlexShrink(0.f);
					}
				}

				// 行 → 命令记下来；按下/抬起由窗口级处理（见下面 onMouseDown/onMouseUp）
				// ⚠ 置灰行不入表：它连"按下"都不记，自然也不会派发
				if (!disabled) rows.push_back({ btn, row.id });
			}

		// 菜单「抬起才派发」：Ling 的按钮是在**按下**时就派发 onClick 的，那样命令一执行菜单
		// 就被关掉，紧接着的抬起不再被菜单吃掉，可能落到托盘图标上，再触发一次托盘单击动作
		//（默认就是「截图」）—— 表现就是"点了设置又弹截图"。所以按下只记行，抬起才执行。
		onMouseDown.add([this](POINT pos, bool isRight) {
			pressedRow = nullptr;
			pressedCmd = 0;
			if (isRight) return;
			for (auto& r : rows) {
				if (r.btn && r.btn->isPosIn(pos)) { pressedRow = r.btn; pressedCmd = r.cmd; break; }
			}
			});
		onMouseUp.add([this](POINT pos, bool isRight) {
			auto* row = pressedRow;
			const UINT cmd = pressedCmd;
			pressedRow = nullptr;
			pressedCmd = 0;
			if (!row || isRight || !row->isPosIn(pos)) return;   // 按下与抬起不在同一行：不派发
			Ling::App::get()->dq.TryEnqueue([cmd]() {
				closeTrayMenu();
				runTrayCommand(cmd);
				});
			});
	}

private:
	struct RowHit { Ling::Button* btn; UINT cmd; };
	std::vector<RowHit> rows;
	Ling::Button* pressedRow{ nullptr };
	UINT pressedCmd{ 0 };
};

	std::unique_ptr<TrayMenuWindow> g_trayMenu;
	HHOOK g_menuMouseHook{ nullptr };
	// 「吞一次点击」：菜单打开时，点在菜单外的那一下（含配对的抬起）不再往下传。用于关菜单，
	// 同时保证它不会落到托盘图标上再触发一次托盘单击动作（默认就是「截图」）。
	bool g_swallowUp{ false };

	void closeTrayMenu()
	{
		if (g_menuMouseHook) { UnhookWindowsHookEx(g_menuMouseHook); g_menuMouseHook = nullptr; }
		if (g_trayMenu) { g_trayMenu->close(); g_trayMenu.reset(); }
		g_swallowUp = false;
	}

	LRESULT CALLBACK menuMouseProc(int code, WPARAM wp, LPARAM lp)
	{
		if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
		const bool down = (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN);
		const bool up = (wp == WM_LBUTTONUP || wp == WM_RBUTTONUP || wp == WM_MBUTTONUP);
		if (!down && !up) return CallNextHookEx(nullptr, code, wp, lp);
		// 关菜单那一下配对的抬起：吃掉，别漏给下面
		if (up && g_swallowUp) { g_swallowUp = false; return 1; }
		if (!g_trayMenu) return CallNextHookEx(nullptr, code, wp, lp);

		auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
		if (!ms) return CallNextHookEx(nullptr, code, wp, lp);
		RECT rc{};
		GetWindowRect(g_trayMenu->hwnd, &rc);
		// 菜单内：交给菜单自己处理（行按钮的按下/抬起在 TrayMenuWindow 里派发）
		if (PtInRect(&rc, ms->pt)) return CallNextHookEx(nullptr, code, wp, lp);

		// 菜单外：这一下只用来关菜单，吞掉不往下传（按下时连配对的抬起一起吞）
		Ling::App::get()->dq.TryEnqueue([]() { closeTrayMenu(); });
		if (down) g_swallowUp = true;
		return 1;
	}

	void openOutputDir()
	{
		const auto dir = Util::outputDir();
		if (dir.empty()) return;
		ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void runTrayCommand(UINT cmd)
	{
		switch (cmd) {
		case kCmdCapture:            WinCap::init(); break;
		case kCmdCaptureFullscreen:  if (auto* app = App::get()) app->captureFullScreenSilent(); break;
		case kCmdRecord:             WinCap::beginRecordPick(); break;
		case kCmdDemoCanvas:         WinCap::beginDemoCanvas(); break;
		case kCmdQuickTranslate:     WinSetting::init(1); break;
		case kCmdHistory:            WinSetting::init(5); break;
		case kCmdOpenDir:            openOutputDir(); break;
		case kCmdSettings:           WinSetting::init(0); break;
		case kCmdExit:               if (auto* app = Ling::App::get()) app->quit(0); break;
		default: break;
		}
	}
}

Tray::Tray()
{
	auto lingApp = Ling::App::get();
	lingApp->initTray(100, L"SnowAir 轻雪");
	// 按用户设置应用托盘图标样式（内置=Logo 字形上色 / 自定义=图片文件）+ 显隐
	TrayIcon::apply();
	Setting::get()->initShortcutKeys();
	lingApp->onTrayMouseEvent.add([this](bool isDown, bool isRight) {
		if (isDown) return;
		if (isRight) { this->onTrayRightClick(); return; }
		switch (Setting::get()->getTrayClickAction()) {
		case 1:  WinSetting::init(); break;
		case 2:  break;
		default: WinCap::init(); break;
		}
	});
}

Tray::~Tray() { closeTrayMenu(); TrayIcon::dispose(); }

void Tray::init()
{
	auto ptr = new Tray();
	trayIns.reset(ptr);
}

Tray* Tray::get() { return trayIns.get(); }

void Tray::onTrayRightClick()
{
	if (g_trayMenu) return;
	POINT pt{};
	if (!GetCursorPos(&pt)) return;
	g_trayMenu = std::make_unique<TrayMenuWindow>();
	if (!g_trayMenu->hwnd) { g_trayMenu.reset(); return; }
	g_trayMenu->openAt(pt);
	g_swallowUp = false;
	g_menuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, menuMouseProc, GetModuleHandleW(nullptr), 0);
}