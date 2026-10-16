#include "pch.h"
#include <dwmapi.h>
#include "App.h"
#include "Setting.h"
#include "Tray.h"
#include "Lang.h"
#include "Update.h"
#include "Util.h"
#include "./Win/WinCap.h"
#include "./Win/WinPin.h"
#include "./Win/WinSetting.h"

std::unique_ptr<App> app;


App::~App()
{
}

void App::init()
{
    auto ptr = new App();
    app.reset(ptr);
    // 启动阶段画托盘图标会用一次图形设备（TrayIcon::buildGlyphIcon → Ling::D2D::get()），
    // 那一刻还没有任何窗口，用完也就没人回收 —— 于是"什么都没操作"也要常驻二十几 MB，
    // 一直到第一次截图会话结束（窗口数归零触发 ~WinBase 里的 dispose）才降下来。
    // 这里等启动流程走完补一刀：仍然没有窗口就把设备放掉。下次要用 D2D::get() 会自动重建。
    SetTimer(nullptr, 0, 1500, +[](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        auto* lapp = Ling::App::get();
        if (lapp && lapp->windows.empty()) Ling::D2D::dispose();
    });
}

void App::dispose()
{
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

// 以管理员身份重新启动：先以 runas 拉起一个提权新实例（带上本进程 PID 让它等我们退干净），
// 本进程随即退出。新实例起步时会先等这个 PID 消失，再去抢单实例，避免被旧实例的
// 单实例标记窗口挡掉（窗口销毁在进程退出之后，直接抢会输）。
bool App::restartAsAdmin()
{
    auto* la = Ling::App::get();
    std::wstring params;
    if (la) {
        for (const auto& [key, val] : la->args) {
            if (key == L"--elevated" || key == L"--wait-pid") continue;
            params += L" " + key;
            if (val != L"true") params += L"=" + val;
        }
    }
    params += L" --elevated --wait-pid=" + std::to_wstring(GetCurrentProcessId());
    if (!Util::spawnElevated(params)) return false;
    if (la) la->quit(0);
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
    // 这里保留同步实现供兜底路径使用。
    auto data = Util::captureScreen(x, y, w, h);
    createBitmapFromBGRA(w, h, data, img);
}

std::tuple<int, int, int, int> App::getScreenArea()
{
	return std::make_tuple(GetSystemMetrics(SM_XVIRTUALSCREEN), 
        GetSystemMetrics(SM_YVIRTUALSCREEN), 
        GetSystemMetrics(SM_CXVIRTUALSCREEN), 
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

void App::excludeFromCapture(HWND hwnd)
{
    if (!hwnd) return;
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
    if (!supported) return;
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
}

void App::clearCaptureExclusion(HWND hwnd)
{
	if (!hwnd) return;
	SetWindowDisplayAffinity(hwnd, WDA_NONE);
}

// 托盘「截取全屏」：静默整屏抓图 → 存文件 + 剪贴板 + 历史，全程不建窗口
void App::captureFullScreenSilent()
{
	auto [sx, sy, sw, sh] = getScreenArea();
	if (sw <= 0 || sh <= 0) return;
	auto pixels = Util::captureScreen(sx, sy, sw, sh);
	if (pixels.empty()) return;

	// 文件名与「功能设置 → 文件输出 → 手动保存文件名格式」同一套模板（默认 SnowAir_{{YYYY-MM-DD_HH-mm-ss}}），
	// 落到 图片\SnowAir 下；扩展名与编码取「保存格式」（PNG/JPEG/WebP/BMP）。
	const std::wstring ext = Util::extensionFromSaveFormat(Setting::get()->getSaveFormat());
	const std::wstring path = Util::buildSavePath(
		Setting::get()->getManualSaveFormat(), Setting::defaultManualSaveFormat(), ext);
	const bool saved = !path.empty() && Util::saveToFile(path, sw, sh, pixels.data());

	if (Setting::get()->getCopyAsFile() && saved) Util::addFileToClipboard(path);
	else Util::saveToClipboard(sw, sh, pixels.data());
	if (saved) Setting::get()->addHistoryItem(pixels, sw, sh, L"fullscreen");
}

// 全局热键「截取焦点窗口」：静默抓当前前台窗口，全程不建窗口。
// 与 captureFullScreenSilent 的差别只在"抓哪块"和"文件名模板"（走焦点窗口那一套 + {{FOCUS_WINDOW_APP_NAME}}）。
void App::captureFocusedWindowSilent()
{
	HWND fg = GetForegroundWindow();
	if (!fg) return;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if (!pid || pid == GetCurrentProcessId()) return;   // 前台是我们自己的窗口：不做

	// 扩展边框（DWM）比 GetWindowRect 更贴近用户看到的那一圈；取不到再退回 GetWindowRect
	RECT rc{};
	if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
		if (!GetWindowRect(fg, &rc)) return;
	}
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;
	if (w <= 0 || h <= 0) return;

	// 应用名要在覆盖/任何窗口出现前取（此刻前台仍是目标窗口）
	const std::wstring appName = Util::foregroundAppName();
	auto pixels = Util::captureScreen(rc.left, rc.top, w, h);
	if (pixels.empty()) return;

	const std::wstring ext = Util::extensionFromSaveFormat(Setting::get()->getSaveFormat());
	const std::wstring path = Util::buildSavePath(
		Setting::get()->getFocusedWindowFormat(), Setting::defaultFocusedWindowFormat(), ext, appName);
	const bool saved = !path.empty() && Util::saveToFile(path, w, h, pixels.data());

	if (Setting::get()->getCopyAsFile() && saved) Util::addFileToClipboard(path);
	else Util::saveToClipboard(w, h, pixels.data());
	if (saved) Setting::get()->addHistoryItem(pixels, w, h, L"focused");
}

// 全局热键「重复截图」：按上一次记住的选区（屏幕坐标）静默再抓一次，全程不建窗口。
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

	auto pixels = Util::captureScreen(rx, ry, rw, rh);
	if (pixels.empty()) return;

	// 与手动「区域截图」收工一致：进剪贴板 + 记历史；勾了「截图后自动保存」才静默落盘
	Util::saveToClipboard(rw, rh, pixels.data());
	Util::quietSaveCapture(pixels, rw, rh, false, L"");
	Setting::get()->addHistoryItem(pixels, rw, rh, L"capture");
}

App::App()
{
    Ling::init();
    auto app = Ling::App::get();
    app->initArgs();
    Ling::D2D::addFonts({ L"icon.ttf" });
    // 录制中直接退出会让编码线程和 D3D 设备一起卡住，退出前先把录制停掉
    app->onBeforeQuit.add([]() { WinCap::stopIfRecording(); });
    Setting::init();
    Lang::init();

    // 用 find 读，别用 operator[]：后者会把没传的键以空值插进 args，
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
        params += L" --elevated";
        if (Util::spawnElevated(params)) app->exit(0); // 提权进程已拉起，原进程退出
    }

    // 由「设置里打开以管理员身份运行」触发的重启：上一实例还在退出中，先等它退干净再去抢单实例，
    // 否则会被上一实例的单实例标记窗口当成"重复启动"直接挡掉（见 App::restartAsAdmin）。
    if (auto it = app->args.find(L"--wait-pid"); it != app->args.end()) {
        DWORD pid = static_cast<DWORD>(std::stoul(it->second));
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(h, 15000);
            CloseHandle(h);
        }
    }

    // 提权重启后，用提权身份把自启补登记一次：非提权进程建不出"最高权限"的计划任务，
    // 打开开关时那一次登记可能已被系统拒绝，这里补上，免得自启悄悄失效。
    if (launchedElevated && Setting::get()->getAutoStart()) {
        Setting::get()->setAutoStart(true);
    }
    // 开了自启就确保登记项指向当前这个 exe：换目录 / 重装 / 升级换版本后，旧的自启项
    // 还指着老 exe，这里按当前 exe 重登记一次把它接管过来（路径没变则什么都不做）。
    Setting::get()->takeOverAutoStart();

    auto autoQuitIt = app->args.find(L"--auto-quit");
    if (autoQuitIt != app->args.end() && autoQuitIt->second == L"true") {
        WinCap::init();
    }
    else {
        bool flag = app->refuseSecondInstance();
        if (flag) return;
        Tray::init();
        // 打开软件不再"随即进入截图模式"：一律先只挂托盘图标待命。
        // 截图的触发交给：托盘图标单击（按设置 → 截图 / 打开设置 / 无操作，见 Tray.cpp）、
        // 全局热键，或再次运行 exe（等同"打开设置"）。
        // 只有显式命令行入口 --enter=pin/long/video/ocr/qr 才直接进入对应模式；
        // --enter=tray（升级重启走的也是它）和 --auto-start（开机自启）同样是纯托盘待命。
        const auto enterIt = app->args.find(L"--enter");
        const bool enterDirect = enterIt != app->args.end()
            && enterIt->second != L"tray" && enterIt->second != L"true";
        if (enterDirect) {
            WinCap::init();
            return;
        }
        Update::checkLater();
    }
}