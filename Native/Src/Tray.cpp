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

	const MenuRow kMenuRows[] = {
		{ kCmdCapture,           L"tray.capture",           L"capture" },
		{ kCmdCaptureFullscreen, L"tray.captureFullscreen", L"capture_fullscreen" },
		{ kCmdRecord,            L"tray.record",            L"record" },
		{ kCmdDemoCanvas,        L"tray.demoCanvas",        L"fullscreen_canvas" },
		{ 0,                     nullptr,                   nullptr },
		{ kCmdQuickTranslate,    L"tray.quickTranslate",    L"quick_translate" },
		{ 0,                     nullptr,                   nullptr },
		{ kCmdHistory,           L"tray.history",           L"open_history" },
		{ kCmdOpenDir,           L"tray.openDir",           L"open_screenshot_dir" },
		{ 0,                     nullptr,                   nullptr },
		{ kCmdSettings,          L"tray.setting",           L"show_main" },
		{ kCmdExit,              L"tray.exit",              nullptr },
	};

	constexpr int kMenuW{ 180 };
	constexpr int kRowH{ 28 };
	constexpr int kMenuPadY{ 8 };
	constexpr int kSepMarginX{ 10 };
	constexpr int kItemMarginX{ 4 };
	constexpr int kItemPadX{ 12 };
	constexpr int kItemRadius{ 4 };
	constexpr int kMenuRadius{ 8 };
	constexpr int kSepH{ 11 };

	uint32_t fromColorRef(COLORREF c)
	{
		return (uint32_t)((GetRValue(c) << 24) | (GetGValue(c) << 16) | (GetBValue(c) << 8) | 0xFF);
	}

	uint32_t themeColor(int part, int state, int prop, COLORREF fallback)
	{
		HTHEME theme = OpenThemeData(nullptr, L"Menu");
		COLORREF c = fallback;
		if (theme) {
			if (FAILED(GetThemeColor(theme, part, state, prop, &c))) c = fallback;
			CloseThemeData(theme);
		}
		return fromColorRef(c);
	}

	struct MenuPalette { uint32_t bg, border, fg, hover, hoverFg, sep; };

	MenuPalette menuPalette()
	{
		MenuPalette p;
		p.bg = 0xFFFFFFFF;       // 浅色：白底
		p.border = 0x0000001A;   // 浅色：1px rgba(0,0,0,0.1)
		p.fg = 0x0B0C0EE0;       // 浅色：前景色 alpha 224
		p.hover = 0xF4F4F5FF;    // 浅色：悬停底色
		p.hoverFg = 0x0B0C0EFF;  // 浅色：悬停前景色
		p.sep = 0x0000000D;      // 浅色：分隔线 rgba(0,0,0,0.05)
		return p;
	}

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
				btn->setHoverBg(p.hover);

				auto* title = btn->makeChild<Ling::Label>();
				title->setText(Lang::get(row.langKey));
				title->setFontSize(12.f);
				title->setColor(p.fg);
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

				const UINT cmd = row.id;
				btn->onClick.add([cmd](Ling::Button*) {
					Ling::App::get()->dq.TryEnqueue([cmd]() {
						closeTrayMenu();
						runTrayCommand(cmd);
						});
					});
			}
		}
	};

	std::unique_ptr<TrayMenuWindow> g_trayMenu;
	HHOOK g_menuMouseHook{ nullptr };

	void closeTrayMenu()
	{
		if (g_menuMouseHook) { UnhookWindowsHookEx(g_menuMouseHook); g_menuMouseHook = nullptr; }
		if (g_trayMenu) { g_trayMenu->close(); g_trayMenu.reset(); }
	}

	LRESULT CALLBACK menuMouseProc(int code, WPARAM wp, LPARAM lp)
	{
		if (code == HC_ACTION && g_trayMenu
			&& (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
			auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
			if (ms) {
				RECT rc{};
				GetWindowRect(g_trayMenu->hwnd, &rc);
				if (!PtInRect(&rc, ms->pt)) Ling::App::get()->dq.TryEnqueue([]() { closeTrayMenu(); });
			}
		}
		return CallNextHookEx(nullptr, code, wp, lp);
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
		case kCmdHistory:            WinSetting::init(2); break;
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
	g_menuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, menuMouseProc, GetModuleHandleW(nullptr), 0);
}