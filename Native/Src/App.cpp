#include "pch.h"
#include <dwmapi.h>
#include "App.h"
#include "Setting.h"
#include "Tray.h"
#include "TrayIcon.h"
#include "Lang.h"
#include "Update.h"
#include "Util.h"
#include "./Win/WinCap.h"
#include "./Win/WinPin.h"
#include "./Win/WinSetting.h"
#include "./Win/WinImageEdit.h"
#include "./Win/ScreenCapture.h"
#include "Plugin/PluginHost.h"
#include "SingleInstance.h"

std::unique_ptr<App> app;

namespace {
    // 前台窗口是否占满整个显示器：其他程序的无边框全屏 / 独占全屏都算。
    // 本进程自己的窗口（截图、演示画布、贴图等全屏）不算，避免自己全屏时把热键关掉。
    bool foregroundFullscreen()
    {
        HWND fg = GetForegroundWindow();
        if (!fg || !IsWindowVisible(fg) || IsIconic(fg)) return false;
        DWORD pid{ 0 };
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) return false;
        wchar_t cls[64]{};
        GetClassNameW(fg, cls, _countof(cls));
        // 桌面 / 任务栏这类壳窗口本身就是"满屏"的，不算全屏应用
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0
            || wcscmp(cls, L"Shell_TrayWnd") == 0) return false;
        // 带标题栏的最大化窗口不算全屏：任务栏设成自动隐藏时，最大化的窗口矩形同样铺满显示器，
        // 只看矩形会误判 —— 用户只是最大化浏览网页就把全局热键全关了。真全屏窗口不处于
        // 「带标题栏 + 最大化」状态（游戏常见的无边框窗口模式即使被最大化也照样算全屏）。
        if (IsZoomed(fg) && (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION)) return false;
        RECT rc{};
        if (!GetWindowRect(fg, &rc)) return false;
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
        const RECT& m = mi.rcMonitor;
        // 留 1px 容差：部分全屏窗口带不可见的 1px 边框
        return rc.left <= m.left + 1 && rc.top <= m.top + 1
            && rc.right >= m.right - 1 && rc.bottom >= m.bottom - 1;
    }
}


App::~App()
{
    PluginHost::instance().shutdown();
}

void App::init()
{
    auto ptr = new App();
    app.reset(ptr);
    // 启动阶段只挂托盘待命：托盘图标走 Ling::D2D::getNoDevice()，只建 D2D 工厂与字体，不建 D3D 设备。
    // 但启动流程里若有人真的用到了图形设备，那一刻还没有任何窗口，用完也就没人回收 —— 于是"什么都没操作"
    // 也要常驻二十几 MB，一直到第一次截图/设置窗口关闭（窗口数归零）才降下来。这里等启动流程走完补一刀：
    // 仍然没有窗口就把设备放掉，下次真正要画时 D2D::get() 会自动重建。
    SetTimer(nullptr, 0, 1500, +[](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        auto* lapp = Ling::App::get();
        if (lapp && lapp->windows.empty())
            Ling::D2D::dispose();
    });
    // 首次截图慢的大头是 Tip/comctl32 首次初始化（约 150ms）。启动 2s 后（1.5s 的 D2D dispose
    // 之后）用一个隐藏小窗 + Tip 预热一次，代价约 0.2MB；首次截图从 ~256ms 降到 ~101ms。
    // SNOWAIR_WARMUP=off 可关闭；=tipd2d 可切到极速档（常驻 D2D 设备，~15.8MB，~48ms）。
    SetTimer(nullptr, 0x5A1A, 2000, +[](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        auto* lapp = Ling::App::get();
        if (lapp && lapp->windows.empty())
            WinCap::warmupOnce();
    });
    // 全屏窗口监听：设置里开了「全屏窗口时关闭全局热键」后，其他程序全屏时暂停全部热键，
    // 退出全屏自动恢复。1 秒轮询足够（切全屏/退出全屏本来就有过渡帧）。
    // 注意 hWnd 传 NULL 的线程定时器：系统会**忽略 nIDEvent，另发一个 id**（回调里的 id 不是 0x5A1B），
    // 所以回调里不能按 id 过滤，否则每次都直接 return —— 定时器等于没开，热键永远不会暂停。
    // 这个回调只由本定时器触发（WM_TIMER 的 lParam 就是它），不需要再校验 id。
    SetTimer(nullptr, 0x5A1B, 1000, +[](HWND, UINT, UINT_PTR, DWORD) {
        auto* st = Setting::get();
        if (!st) return;
        st->setHotkeysSuppressed(st->getPauseHotkeysFullscreen() && foregroundFullscreen());
    });
}

void App::dispose()
{
    // 托盘单例的析构还会去碰 Ling 的事件表、菜单窗口和图标缓存，必须赶在它们之前拆；
    // 留到静态析构时，图标缓存（TrayIcon 里的 g_cache）可能已被先一步销毁 → 退出时崩溃
    Tray::dispose();
    // 窗口对象是文件级静态变量，交给静态析构就晚了（那时 CoUninitialize 已经跑完），
    // 所以趁这里把还开着的窗口先放掉
    WinPin::dispose();
    WinCap::dispose();
    WinSetting::dispose();
    Lang::dispose();
    Setting::dispose();
    app.reset();
}

App* App::get()
{
    return app.get();
}

namespace {
    // 重启用的命令行：沿用当前参数，去掉提权 / 等待标记，再补上 --wait-pid=<本进程>，
    // 让新实例先等本进程退干净再去抢单实例（窗口销毁在进程退出之后，直接抢会输）。
    std::wstring relaunchParams(bool elevated)
    {
        std::wstring params;
        if (auto* la = Ling::App::get()) {
            for (const auto& [key, val] : la->args) {
                if (key == L"--elevated" || key == L"--wait-pid") continue;
                params += L" " + key;
                if (val != L"true") params += L"=" + val;
            }
        }
        if (elevated) params += L" --elevated";
        params += L" --wait-pid=" + std::to_wstring(GetCurrentProcessId());
        return params;
    }
}

// 以管理员身份重新启动：先以 runas 拉起一个提权新实例（带上本进程 PID 让它等我们退干净），
// 本进程随即退出。
bool App::restartAsAdmin()
{
    const auto params = relaunchParams(true);
    if (!Util::spawnElevated(params)) return false;
    if (auto* la = Ling::App::get()) la->quit(0);
    return true;
}

// 以当前权限重新启动：导入配置后调用，配置在新实例启动时自然会重新读一遍
bool App::restart()
{
    const auto params = relaunchParams(false);
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.lpFile = exePath;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return false;
    if (auto* la = Ling::App::get()) la->quit(0);
    return true;
}

void App::createBitmapFromBGRA(int w, int h, const std::vector<BYTE>& data, ID2D1Bitmap1** img)
{
    if (!img || w <= 0 || h <= 0) return;
    if (data.size() < (size_t)w * 4 * h) return;
    D2D1_BITMAP_PROPERTIES1 props = {
       .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)},
       .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    auto d2d = Ling::D2D::get();
    d2d->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data.data(), w * 4, props, img);
}

void App::takeScreenShot(int x, int y, int w, int h, ID2D1Bitmap1** img)
{
    // GDI 抓屏（BitBlt+GetDIBits）与建位图分开：前者可放工作线程（见 WinCap::init），
    // 这里保留同步实现供兜底路径使用。截图走 ScreenCapture（HDR 屏优先生效），失败回退 GDI。
    auto data = ScreenCapture::captureOrGdi(x, y, w, h);
    createBitmapFromBGRA(w, h, data, img);
}

std::tuple<int, int, int, int> App::getScreenArea()
{
	return std::make_tuple(GetSystemMetrics(SM_XVIRTUALSCREEN), 
        GetSystemMetrics(SM_YVIRTUALSCREEN), 
        GetSystemMetrics(SM_CXVIRTUALSCREEN), 
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

bool App::supportsCaptureExclusion()
{
    // 老系统上这个调用不但不失败，还会把窗口变成捕获画面里的一整块黑（实测 build 18363：
    // 返回 TRUE，读回来的 affinity 就是 0x11 —— 内核照存，可那会儿的 DWM 只认"非零即
    // 受保护内容"，一律涂黑）。所以必须自己拦住，让老系统退回"照旧被录进去"。
    // GetVersionEx 会被兼容性清单骗，只有 RtlGetVersion 给的是真版本号
    static const bool supported = []() {
        OSVERSIONINFOW vi{ sizeof(vi) };
        auto rtlGetVersion = (LONG(WINAPI*)(OSVERSIONINFOW*))GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        return rtlGetVersion && rtlGetVersion(&vi) == 0 && vi.dwBuildNumber >= 19041;
    }();
    return supported;
}

void App::excludeFromCapture(HWND hwnd)
{
    if (!hwnd || !supportsCaptureExclusion()) return;
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
}

void App::clearCaptureExclusion(HWND hwnd)
{
	if (!hwnd) return;
	SetWindowDisplayAffinity(hwnd, WDA_NONE);
}

// 托盘「截取全屏」：静默模式下直接抓图；关闭「静默截图」时带整屏选区进入工具栏，由用户手动完成。
void App::captureFullScreenSilent()
{
	auto [sx, sy, sw, sh] = getScreenArea();
	if (sw <= 0 || sh <= 0) return;
	// 「静默截图」关闭：跳过框选直接带选区进标注，显示工具栏，由用户手动完成
	if (!Setting::get()->getSilentCapture()) {
		WinCap::beginWithRect(RECT{ sx, sy, sx + sw, sy + sh });
		return;
	}
	auto pixels = ScreenCapture::captureOrGdi(sx, sy, sw, sh);
	if (pixels.empty()) return;

	// 文件名与「功能设置 → 文件输出 → 手动保存文件名格式」同一套模板（默认 SnowAir_{{YYYY-MM-DD_HH-mm-ss}}），
	// 落到 图片\SnowAir 下；扩展名与编码取「保存格式」（PNG/JPEG/WebP/BMP）。
	// 「仅复制图片」档不落盘
	auto* st = Setting::get();
	const int action = st->getCaptureFinishAction();
	const std::wstring ext = Util::extensionFromSaveFormat(st->getSaveFormat());
	const std::wstring path = (action != 3) ? Util::buildSavePath(
		st->getManualSaveFormat(), Setting::defaultManualSaveFormat(), ext) : std::wstring{};
	const bool saved = !path.empty() && Util::saveToFile(path, sw, sh, pixels.data());

	if (action == 1 && saved) Util::addFileToClipboard(path);
	else if (action == 2 && saved) Util::addFileToClipboardCut(path);
	else Util::saveToClipboard(sw, sh, pixels.data());
	st->setLastCaptureRect(sx, sy, sw, sh);
	if (saved) st->addHistoryFile(path, sw, sh, L"fullscreen");
	// 静默截图没有窗口可反馈：托盘图标临时换成绿色的「成功」勾，1.2s 后自动恢复
	TrayIcon::flashSuccess();
}

// 全局热键「截取焦点窗口」：静默模式直接抓当前前台窗口；关闭「静默截图」时改为带窗口选区进入工具栏。
// 与 captureFullScreenSilent 的差别只在"抓哪块"；文件名统一走「手动保存文件名格式」，
// 其中 {{FOCUS_WINDOW_APP_NAME}} 会替换成前台应用名。
void App::captureFocusedWindowSilent()
{
	HWND fg = GetForegroundWindow();
	if (!fg) return;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if (!pid) return;
	// 自己的窗口里只认「真正的窗口」：设置页 / 图片编辑页；
	// 截图遮罩、托盘菜单、工具条这些不截（截出来是废图）
	const bool ownWindow = (pid == GetCurrentProcessId());
	if (ownWindow) {
		bool allowed = false;
		if (auto* lapp = Ling::App::get()) {
			for (auto* w : lapp->windows) {
				if (!w || w->hwnd != fg) continue;
				allowed = dynamic_cast<WinSetting*>(w) != nullptr
					|| dynamic_cast<WinImageEdit*>(w) != nullptr;
				break;
			}
		}
		if (!allowed) return;
	}

	// 扩展边框（DWM）比 GetWindowRect 更贴近用户看到的那一圈；取不到再退回 GetWindowRect
	RECT rc{};
	if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
		if (!GetWindowRect(fg, &rc)) return;
	}
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;
	if (w <= 0 || h <= 0) return;
	// 「静默截图」关闭：跳过框选，直接把前台窗口区域带进标注工具栏
	if (!Setting::get()->getSilentCapture()) {
		WinCap::beginWithRect(rc);
		return;
	}

	// 应用名要在覆盖/任何窗口出现前取（此刻前台仍是目标窗口）
	std::wstring appName = Util::foregroundAppName();
	// 截自己的窗口时 foregroundAppName 会返回空（同进程不取名字），文件名占位符补一个
	if (appName.empty() && ownWindow) appName = L"SnowAir";
	auto pixels = ScreenCapture::captureOrGdi(rc.left, rc.top, w, h);
	if (pixels.empty()) return;

	const std::wstring ext = Util::extensionFromSaveFormat(Setting::get()->getSaveFormat());
	// 文件名走「手动保存文件名格式」（已并入原焦点窗口格式），仍支持 {{FOCUS_WINDOW_APP_NAME}}
	// 「仅复制图片」档不落盘
	const bool saveFile = Setting::get()->getCaptureFinishAction() != 3;
	const std::wstring path = saveFile ? Util::buildSavePath(
		Setting::get()->getManualSaveFormat(), Setting::defaultManualSaveFormat(), ext, appName) : std::wstring{};
	const bool saved = !path.empty() && Util::saveToFile(path, w, h, pixels.data());

	if (Setting::get()->getCaptureFinishAction() == 1 && saved) Util::addFileToClipboard(path);
	else if (Setting::get()->getCaptureFinishAction() == 2 && saved) Util::addFileToClipboardCut(path);
	else Util::saveToClipboard(w, h, pixels.data());
	Setting::get()->setLastCaptureRect(rc.left, rc.top, w, h);
	if (saved) Setting::get()->addHistoryFile(path, w, h, L"focused");
	// 静默截图没有窗口可反馈：托盘图标临时换成绿色的「成功」勾，1.2s 后自动恢复
	TrayIcon::flashSuccess();
}

// 全局热键「重复截图」：静默模式按上次选区直接再抓一次；关闭「静默截图」时带该选区进入工具栏。
void App::captureLastRegionSilent()
{
	int rx = 0, ry = 0, rw = 0, rh = 0;
	if (!Setting::get()->getLastCaptureRect(rx, ry, rw, rh)) return;

	// 分辨率/显示器数量变了就把选区夹回当前虚拟桌面，别抓到桌面外
	auto [sx, sy, sw, sh] = getScreenArea();
	if (sw <= 0 || sh <= 0) return;
	if (rx < sx) { rw -= (sx - rx); rx = sx; }
	if (ry < sy) { rh -= (sy - ry); ry = sy; }
	if (rx + rw > sx + sw) rw = sx + sw - rx;
	if (ry + rh > sy + sh) rh = sy + sh - ry;
	if (rw <= 0 || rh <= 0) return;

	// 「静默截图」关闭：跳过框选，按上次选区直接进入标注工具栏
	if (!Setting::get()->getSilentCapture()) {
		WinCap::beginWithRect(RECT{ rx, ry, rx + rw, ry + rh });
		return;
	}

	auto pixels = ScreenCapture::captureOrGdi(rx, ry, rw, rh);
	if (pixels.empty()) return;

	// 与区域截图收工一致：按「截图完成执行」三档决定存不存文件、剪贴板放位图还是文件
	const int action = Setting::get()->getCaptureFinishAction();
	const auto saved = (action != 3) ? Util::saveCompletedCapture(pixels, rw, rh) : std::wstring{};
	if (!saved.empty() && action == 1) Util::addFileToClipboard(saved);
	else if (!saved.empty() && action == 2) Util::addFileToClipboardCut(saved);
	else Util::saveToClipboard(rw, rh, pixels.data());
	if (!saved.empty()) Setting::get()->addHistoryFile(saved, rw, rh, L"capture");
	// 静默截图没有窗口可反馈：托盘图标临时换成绿色的「成功」勾，1.2s 后自动恢复
	TrayIcon::flashSuccess();
}

App::App()
{
    Ling::init();
    auto app = Ling::App::get();
    app->initArgs();
    Ling::D2D::addFonts({ L"icon.ttf" });
    // 录制中直接退出会让编码线程和 D3D 设备一起卡住，退出前先把录制停掉
    app->onBeforeQuit.add([]() { WinCap::stopIfRecording(); });

    // 提权重启时需等待上一实例退出，该步骤必须在单实例判定之前
    if (auto it = app->args.find(L"--wait-pid"); it != app->args.end()) {
        DWORD pid = static_cast<DWORD>(std::stoul(it->second));
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(h, 15000);
            CloseHandle(h);
        }
    }

    // 单实例判定在读取配置之前进行：第二个实例仅通知已有实例打开设置中心，
    // 不读写 config / local.json，避免多余的同步盘写入，以及仅应由主实例执行的迁移。
    bool hotkeysYielded = false;
    const auto autoQuitIt = app->args.find(L"--auto-quit");
    const bool autoQuit = autoQuitIt != app->args.end() && autoQuitIt->second == L"true";
    if (!autoQuit) {
        // 单实例 + 全局热键归属：完整版与 Lite 版可以并存，但热键只归完整版
        const auto decision = enforceSingleInstance(autoQuit);
        if (!decision.continueLaunch) {
            // 已经由同类实例接管：本进程必须真的退出（仅从 init 返回的话
            // wWinMain 会继续跑消息循环，进程会挂着不走）
            if (auto* lingApp = Ling::App::get()) lingApp->exit(0);
            return;
        }
        hotkeysYielded = !decision.ownHotkeys;
    }

    Setting::init();
    // 上面判定出的热键归属要等配置就绪再应用（applyHotkeys 会据此决定注册与否）
    if (hotkeysYielded && Setting::get()) Setting::get()->setHotkeysYielded(true);
    Lang::init();
    PluginHost::instance().init();

    // 使用 find 读取，不要使用 operator[]：后者会把未传入的键以空值插入 args，
    // 后面重建命令行时会混进 --elevated= / --auto-quit= 这类空值噪声。
    auto elevIt = app->args.find(L"--elevated");
    const bool launchedElevated = elevIt != app->args.end() && elevIt->second == L"true";

    // 以管理员身份运行：开局自举。配置开启、当前进程尚未提权、且本次启动不是提权进程时，
    // 用 runas 重启自身并退出，让"下一次打开软件"（限手工重开 / 开机自启等）都以管理员身份运行。
    if (Setting::get()->getRunAsAdmin() && !launchedElevated && !Util::isElevated()) {
        std::wstring params;
        for (const auto& [key, val] : app->args) {
            params += L" " + key;
            if (val != L"true") params += L"=" + val;
        }
        params += L" --elevated --wait-pid=" + std::to_wstring(GetCurrentProcessId());
        if (Util::spawnElevated(params)) app->exit(0); // 提权进程已拉起，原进程退出
    }

    // 提权重启后，用提权身份把自启补登记一次：非提权进程建不出"最高权限"的计划任务，
    // 打开开关时那一次登记可能已被系统拒绝，这里补上，免得自启悄悄失效。
    if (launchedElevated && Setting::get()->getAutoStart()) {
        Setting::get()->setAutoStart(true);
    }
    // 开了自启就确保登记项指向当前这个 exe：换目录 / 重装 / 升级换版本后，旧的自启项
    // 还指着老 exe，这里按当前 exe 重登记一次把它接管过来（路径没变则什么都不做）。
    Setting::get()->takeOverAutoStart();

    if (autoQuit) {
        WinCap::init();
    }
    else {
        Tray::init();
        // 打开软件不再"随即进入截图模式"：一律先只挂托盘图标待命。
        // 截图的触发交给：托盘图标单击（按设置 → 截图 / 打开设置 / 无操作，见 Tray.cpp）、
        // 全局热键，或再次运行 exe（等同"打开设置"）。
        // 只有显式命令行入口 --enter=pin/long/video/ocr/demo/edit 才直接进入对应模式
        // （--enter=demo 由 WinCap::init() 换成演示画布，不需要框选）；
        // --enter=tray 与 --auto-start（开机自启）同样是纯托盘待命。
        const auto enterIt = app->args.find(L"--enter");
        const bool enterDirect = enterIt != app->args.end()
            && enterIt->second != L"tray" && enterIt->second != L"true";
        if (enterDirect) {
            // 图片编辑不是截图会话：直接开编辑窗口（--enter=edit，别名 image）。
            // 参数一次性，取走后清掉，后续逻辑不会再看到它。
            const std::wstring enterVal = enterIt->second;
            if (enterVal == L"edit" || enterVal == L"image") {
                app->args.erase(enterIt);
                WinImageEdit::init();
                return;
            }
            WinCap::init();
            return;
        }
        Update::checkLater();
    }
}