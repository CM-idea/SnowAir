#include "pch.h"
#include <include/Ling.h>
#include <wincodec.h>
#include <chrono>
#include <fstream>
#include <mutex>
// 快速贴屏要读「资源管理器当前选中项」（Shell COM）与「前台窗口里拖选的文字」（UIA）
#include <servprov.h>
#include <shlguid.h>
#include <exdisp.h>
// 顺序有讲究：oleacc.h 必须在 UIAutomation*.h 之前，否则 ITextProvider 会重复定义
#include <oleacc.h>
#include <UIAutomationClient.h>
#include "Setting.h"
#include "Plugin/PluginHost.h"
#include "Util.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinPin.h"
#include "Win/ClipPin.h"
#include "Win/HotkeyHook.h"
#include "Win/WinSetting.h"
#include "App.h"
#include <algorithm>
#include <set>
#include <cwctype>

namespace {
    std::unique_ptr<Setting> setting;
    // 全局热键表：id（配置键 = 设置页那一行）→ msgId（RegisterHotKey 的 id，也是 onHotKey 收到的值）。
    // ⚠ 只有**真正接了动作**的行才在这里；设置页里其余几行（show_cursor / translate_selection /
    //   toggle_pins）目前没有对应实现，不注册 —— 否则会出现"按键被消费但没有任何效果"。
    struct HotkeyEntry { const wchar_t* id; UINT msgId; };
    const HotkeyEntry kHotkeys[] = {
        { L"capture",                100 },
        { L"capture_fullscreen",     101 },
        { L"capture_focused_window", 102 },
        { L"fullscreen_canvas",      103 },
        { L"record",                 104 },
        { L"toggle_pins",            105 },
        { L"show_main",              106 },
        { L"open_screenshot_dir",    107 },
        { L"open_history",           108 },
        { L"capture_repeat",         109 },
        { L"quick_pin",              110 },
        { L"rack_quick_hang",        111 },
        { L"rack_toggle",            112 },
    };
    bool isGlobalHotkeyId(const std::wstring& id)
    {
        for (const auto& hk : kHotkeys) if (id == hk.id) return true;
        return false;
    }
    // 全局热键的内置默认键位：配置里没有这一项（从没设过）时用它
    const wchar_t* defaultShortcutOf(const std::wstring& type)
    {
        if (type == L"capture") return L"F1";
        if (type == L"record") return L"F2";
        if (type == L"fullscreen_canvas") return L"F3";
        return L"";
    }
    constexpr UINT_PTR kHotkeyRetryTimerId{ 0x5A18 };
    // 配置文件的默认内容。空文件、坏 JSON、缺键都拿它兜底，所以这里列出的每一项
    // 都是代码里会直接按名字取的（见 getLang / getAutoStart）。
    // 全局热键**不在这里**给默认值 —— 键位统一由 getShortcutKey 的内置默认表决定（defaultShortcutOf）
    constexpr std::wstring_view defaultConfig{ LR"""({"common":{"autoStart":true,"runAsAdmin":false,"language":"zh-CN","updateCheckFreq":0}})""" };

    // 静默执行一条命令行（不弹出控制台窗口），用于 schtasks 注册/删除管理员自启任务。
    void runQuiet(std::wstring cmd)
    {
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring full = wfmt(L"cmd.exe /c %s", cmd.c_str());
        std::vector<wchar_t> buf(full.begin(), full.end());
        buf.push_back(0);
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
}


// —— config.json 的读取与落盘 ——
namespace {
    // 读配置文件。返回 false = 读不出来（被占用 / 没权限 / IO 错误），与「文件是空的」区分开。
    // 旧实现直接用 readFileText：读失败也返回空串，于是启动时会把读不到的配置当成空文件、
    // 立刻写回默认值 —— 一次临时的读取失败（杀软扫描、另一个实例正在保存）就会清空用户的
    // 全部设置，连带 OCR 档位与插件目录，表现成「OCR 说运行时未安装」「设置自己变回去了」。
    bool readConfigContent(const std::filesystem::path& path, std::wstring& out)
    {
        out.clear();
        std::ifstream in{ path, std::ios::binary };
        if (!in) return false;
        std::string bytes{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
        if (in.bad()) return false;
        out = Ling::Util::readTextFromBytes(bytes.data(), bytes.size());
        return true;
    }

    // 只有空白 / BOM：老版本写盘中断留下的空文件，按「首次运行」处理
    bool isBlankConfig(const std::wstring& s)
    {
        for (wchar_t c : s)
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n' && c != 0xFEFF) return false;
        return true;
    }

    // 原子写：先写同目录临时文件，成功后再整体替换目标。
    // 旧实现 CreateFile(CREATE_ALWAYS) 直接截断目标文件，写到一半崩溃 / 被杀就留下半截 JSON，
    // 下次启动解析失败 —— 用户看到的「配置文件读取错误」就是这么来的。
    bool writeConfigAtomic(const std::filesystem::path& dst, const std::wstring& text)
    {
        const std::wstring tmp = dst.wstring() + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const WORD bom = 0xFEFF;
        const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        DWORD written = 0;
        bool ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom)
            && WriteFile(h, text.data(), bytes, &written, nullptr) && written == bytes
            && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    std::filesystem::path siblingOf(const std::filesystem::path& p, const wchar_t* suffix)
    {
        return std::filesystem::path{ p.wstring() + suffix };
    }
}


Setting::Setting() :dataPath{ initDataPath() }, configPath{ initConfigPath() }
{
    if (std::filesystem::exists(configPath)) {
        std::wstring content;
        if (!readConfigContent(configPath, content)) {
            // 读不到 ≠ 空文件：原文件保持原样，本次先用内存里的默认值，绝不回写覆盖
            configReadOnly = true;
            configObj = JsonObject::Parse(defaultConfig);
            MessageBox(nullptr,
                L"config.json 读取失败（文件被占用或没有权限）。\n"
                L"本次使用默认设置，原文件已保留、不会被覆盖。\n\n"
                L"Could not read config.json. The file was left untouched.",
                L"SnowAir", MB_OK | MB_ICONWARNING);
            return;
        }
        if (isBlankConfig(content)) {
            // 真空文件（首次安装 / 上次写到一半）：补一份默认配置
            configObj = JsonObject::Parse(defaultConfig);
            save();
            return;
        }
        JsonObject obj{ nullptr };
        if (JsonObject::TryParse(content, obj)) {
            configObj = obj;
            // 留一份可用配置：下次真解析失败时用它恢复，而不是把设置清成默认值
            backupConfigFile();
            return;
        }
        // 解析失败：先留存现场，再尝试上一份可用配置，都没有才回落默认值。
        // 旧实现只把内存换成默认值、坏文件原地不动：用户每启动一次丢一次设置。
        preserveBrokenConfig();
        if (restoreFromBackup()) return;
        configObj = JsonObject::Parse(defaultConfig);
        MessageBox(nullptr,
            L"config.json 格式错误，已备份为 config.json.bad，本次使用默认设置。\n\n"
            L"config.json is malformed; a backup was kept as config.json.bad.",
            L"SnowAir", MB_OK | MB_ICONWARNING);
        return;
    }
    // 没有 config.json：真正的首次运行（或用户主动删掉想恢复默认），按默认值初始化。
    // 不从 .bak 复活：删文件是最直观的「恢复默认」，不能让它把旧设置又拉回来。
    configObj = JsonObject::Parse(defaultConfig);
}

// 启动读成功时留一份好的副本，供 restoreFromBackup 回退（只在启动时写，运行期不多写盘）
void Setting::backupConfigFile()
{
    if (configPath.empty()) return;
    std::wstring text;
    if (!readConfigContent(configPath, text)) return;
    writeConfigAtomic(siblingOf(configPath, L".bak"), text);
}

// 解析失败后的兜底：用上一份可用配置重建主文件
bool Setting::restoreFromBackup()
{
    std::wstring text;
    if (!readConfigContent(siblingOf(configPath, L".bak"), text) || isBlankConfig(text)) return false;
    JsonObject obj{ nullptr };
    if (!JsonObject::TryParse(text, obj)) return false;
    configObj = obj;
    save();
    return true;
}

// 坏档留一份现场（覆盖上一份 .bad），方便用户/排查找回原内容
void Setting::preserveBrokenConfig()
{
    if (configPath.empty()) return;
    std::error_code ec;
    std::filesystem::copy_file(configPath, siblingOf(configPath, L".bad"),
        std::filesystem::copy_options::overwrite_existing, ec);
}



Setting::~Setting()
{

}

// —— 剪切板监听：记录文本与位图（剪切板历史数据的来源）——
namespace {
    HWND g_clipWnd{ nullptr };
    constexpr const wchar_t* kClipWndClass{ L"SnowAirClipboardWatch" };

    // CF_DIB → BGRA（只处理最常见的 BI_RGB 24/32 位，其余格式不记录）
    bool dibToBgra(HGLOBAL handle, int& outW, int& outH, std::vector<BYTE>& out)
    {
        if (!handle) return false;
        const auto* bi = static_cast<const BITMAPINFOHEADER*>(GlobalLock(handle));
        if (!bi) return false;
        bool ok = false;
        const int bpp = (int)bi->biBitCount;
        if (bi->biCompression == BI_RGB && (bpp == 24 || bpp == 32)
            && bi->biWidth > 0 && bi->biHeight != 0) {
            const int w = (int)bi->biWidth;
            const int h = std::abs((int)bi->biHeight);
            const bool topDown = bi->biHeight < 0;
            const size_t srcStride = ((size_t)w * bpp + 31) / 32 * 4;
            const BYTE* bits = reinterpret_cast<const BYTE*>(bi) + bi->biSize;
            out.assign((size_t)w * 4u * h, 0xFF);
            for (int y = 0; y < h; ++y) {
                const BYTE* src = bits + (size_t)(topDown ? y : h - 1 - y) * srcStride;
                BYTE* dst = out.data() + (size_t)y * w * 4;
                if (bpp == 32) {
                    for (int x = 0; x < w; ++x, src += 4, dst += 4) {
                        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xFF;
                    }
                }
                else {
                    for (int x = 0; x < w; ++x, src += 3, dst += 4) {
                        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xFF;
                    }
                }
            }
            outW = w;
            outH = h;
            ok = true;
        }
        GlobalUnlock(handle);
        return ok;
    }

    // 剪切板里的 PNG（浏览器 / Electron / 本软件自己）→ BGRA
    bool pngToBgra(HGLOBAL handle, int& outW, int& outH, std::vector<BYTE>& out)
    {
        if (!handle) return false;
        void* data = GlobalLock(handle);
        if (!data) return false;
        const SIZE_T size = GlobalSize(handle);
        bool ok = false;
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf())))) {
            Microsoft::WRL::ComPtr<IWICStream> stream;
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
            if (SUCCEEDED(factory->CreateStream(stream.GetAddressOf()))
                && SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(data), (DWORD)size))
                && SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                    WICDecodeMetadataCacheOnDemand, dec.GetAddressOf()))
                && SUCCEEDED(dec->GetFrame(0, frame.GetAddressOf()))) {
                UINT w = 0, h = 0;
                if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0
                    && SUCCEEDED(factory->CreateFormatConverter(conv.GetAddressOf()))
                    && SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    out.resize((size_t)w * 4u * h);
                    if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.size(), out.data()))) {
                        outW = (int)w;
                        outH = (int)h;
                        ok = true;
                    }
                }
            }
        }
        GlobalUnlock(handle);
        return ok;
    }

    // CF_BITMAP（.NET / 部分 Office 程序）→ BGRA
    bool hbitmapToBgra(HBITMAP hbm, int& outW, int& outH, std::vector<BYTE>& out)
    {
        if (!hbm) return false;
        BITMAP bm{};
        if (!GetObjectW(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
        const int w = (int)bm.bmWidth, h = (int)bm.bmHeight;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = h;   // 正数 = 自下而上
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> tmp((size_t)w * 4u * h);
        HDC hdc = GetDC(nullptr);
        if (!hdc) return false;
        const int got = GetDIBits(hdc, hbm, 0, (UINT)h, tmp.data(), &bmi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, hdc);
        if (got <= 0) return false;
        out.assign(tmp.size(), 0xFF);
        for (int y = 0; y < h; ++y) {
            const BYTE* src = tmp.data() + (size_t)(h - 1 - y) * w * 4;
            BYTE* dst = out.data() + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x, src += 4, dst += 4) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xFF;
            }
        }
        outW = w;
        outH = h;
        return true;
    }

    DWORD g_lastClipSeq{ 0 };
    constexpr UINT_PTR kClipPollTimer{ 1 };
    constexpr UINT kClipPollMs{ 800 };

    void captureClipboardNow(bool force = false)
    {
        auto* s = Setting::get();
        if (!s || !g_clipWnd) return;
        // 事件与轮询可能撞在同一处：序列号没变就不再重复读一遍
        const DWORD seq = GetClipboardSequenceNumber();
        if (!force && seq == g_lastClipSeq) return;
        // 剪贴板可能仍被写入方占用：短重试，避免"刚好这一下没读到"就永远漏掉
        bool opened = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (OpenClipboard(g_clipWnd)) { opened = true; break; }
            Sleep(5);
        }
        if (!opened) return;
        g_lastClipSeq = seq;
        if (HANDLE hf = GetClipboardData(CF_HDROP)) {
            // 文件列表（资源管理器 / Word / PPT 等）：只记路径，不复制内容
            HDROP drop = static_cast<HDROP>(hf);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> files;
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len, L'\0');
                if (DragQueryFileW(drop, i, path.data(), len + 1) == 0) continue;
                files.push_back(std::move(path));
            }
            if (!files.empty()) {
                s->addClipboardFiles(files);
                CloseClipboard();
                return;
            }
        }
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                s->addClipboardText(p);
                GlobalUnlock(h);
            }
        }
        else if (HANDLE ha = GetClipboardData(CF_TEXT)) {
            // 老程序只给 ANSI 文本：按本地代码页转一次
            if (const auto* p = static_cast<const char*>(GlobalLock(ha))) {
                const int len = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
                if (len > 1) {
                    std::wstring w((size_t)len - 1, L'\0');
                    MultiByteToWideChar(CP_ACP, 0, p, -1, w.data(), len);
                    s->addClipboardText(w);
                }
                GlobalUnlock(ha);
            }
        }
        else {
            int w = 0, hh = 0;
            std::vector<BYTE> px;
            bool ok = false;
            // 优先级：PNG（浏览器/Electron，体积小且带 alpha）→ DIBV5/DIB → CF_BITMAP
            static const UINT pngFmt = RegisterClipboardFormatW(L"PNG");
            if (HANDLE hp = GetClipboardData(pngFmt)) ok = pngToBgra(hp, w, hh, px);
            if (!ok) {
                HANDLE hb = GetClipboardData(CF_DIBV5);
                if (!hb) hb = GetClipboardData(CF_DIB);
                if (hb) ok = dibToBgra(hb, w, hh, px);
            }
            if (!ok) {
                if (HBITMAP hbm = (HBITMAP)GetClipboardData(CF_BITMAP))
                    ok = hbitmapToBgra(hbm, w, hh, px);
            }
            if (ok) s->addClipboardImage(w, hh, px);
        }
        CloseClipboard();
    }

    LRESULT CALLBACK clipboardWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CLIPBOARDUPDATE) {
            // 直接读（内部带重试）；不依赖 DispatcherQueue，避免队列没跑起来时事件被吞
            captureClipboardNow();
            return 0;
        }
        if (msg == WM_TIMER && wp == kClipPollTimer) {
            // 兜底轮询：个别环境下收不到 WM_CLIPBOARDUPDATE，用序列号变化补上
            if (GetClipboardSequenceNumber() != g_lastClipSeq) captureClipboardNow();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void installClipboardWatch()
    {
        if (g_clipWnd) return;
        WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = clipboardWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClipWndClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        // 普通隐藏窗（不显示）：只有顶层窗才稳定收到 WM_CLIPBOARDUPDATE
        g_clipWnd = CreateWindowExW(0, kClipWndClass, L"", WS_OVERLAPPED,
            0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (g_clipWnd) {
            AddClipboardFormatListener(g_clipWnd);
            // 兜底轮询（800ms）：WM_CLIPBOARDUPDATE 偶发收不到时也能接住
            SetTimer(g_clipWnd, kClipPollTimer, kClipPollMs, nullptr);
            // 启动时先把当前剪贴板里的内容记一条，用户打开「剪切板」页不至于空空如也
            g_lastClipSeq = GetClipboardSequenceNumber();   // no capture on startup
        }
    }

    void uninstallClipboardWatch()
    {
        if (!g_clipWnd) return;
        KillTimer(g_clipWnd, kClipPollTimer);
        RemoveClipboardFormatListener(g_clipWnd);
        DestroyWindow(g_clipWnd);
        g_clipWnd = nullptr;
        UnregisterClassW(kClipWndClass, GetModuleHandleW(nullptr));
    }
}

void Setting::init()
{
    auto ptr = new Setting();
    setting.reset(ptr);
    // 启动时清理旧版本遗留的 AI 配置（AI 对话 / AI 视觉 OCR / AI 翻译）
    setting->purgeRemovedAiSettings();
    // 启动时清理旧程序（ScreenCapture）留下的历史键位
    setting->purgeLegacyShortcutKeys();
    installClipboardWatch();
}

void Setting::dispose()
{
    uninstallClipboardWatch();
    setting.reset();
}

Setting* Setting::get()
{
    return setting.get();
}

std::filesystem::path Setting::getDataPath()
{
    return dataPath; //复制一份路径对象，不允许就地修改
}

std::wstring Setting::getPluginDir()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return L"";
    return std::wstring{ common.GetNamedString(L"pluginDir", L"") };
}

void Setting::setPluginDir(const std::wstring& dir)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"pluginDir", JsonValue::CreateStringValue(dir));
    save();
}

const JsonObject Setting::getConfigObj()
{
    return configObj;
}

void Setting::setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys)
{
    std::wstring str;
    for (size_t i = 0; i < keys.size(); i++)
    {
        str += L"+" + keys[i];
    }
    str.erase(0,1);
    // 配置里还没有 shortcutKey 对象时（首次设置热键）要先建出来：GetNamedObject 的单参
    // 重载在缺键时会抛 hresult_error，异常会穿透 DispatcherQueue 回调导致进程被杀。
    auto shortcutKey = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!shortcutKey) {
        shortcutKey = JsonObject();
        configObj.SetNamedValue(L"shortcutKey", shortcutKey);
    }
    shortcutKey.SetNamedValue(type, JsonValue::CreateStringValue(str));
    save();
    // 改的是哪一行就整体重注册 —— 现在设置页里已实现的行全都真注册了
    if (isGlobalHotkeyId(type)) applyHotkeys();
}

std::wstring Setting::getShortcutKey(const std::wstring& type)
{
    // 一路用带默认值的重载：启动时 ensureDefaults 已经补齐过，这里只是别让运行期
    // 意外（配置被外部改动、问了个没配过的 type）变成一次崩溃
    auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
    // 配置里"有这一行"就以它为准 —— 包括用户按 Backspace/Delete 清空后写进去的空串，
    // 空串 = 不绑定。只有从来没写过的行才回落到内置默认键位，
    // 否则"清空 capture"会被误当成"没配过"，又变回 F1，清空就永远清不掉。
    if (obj && obj.HasKey(type)) {
        return std::wstring{ obj.GetNamedString(type, L"") };
    }
    return defaultShortcutOf(type);
}

// 「全局热键」页的恢复默认：清空整个 shortcutKey 对象。
// 清空后 getShortcutKey 会逐条回落到内置默认表（capture=F1 / record=F2 / fullscreen_canvas=F3，
// 其余默认不绑）。保留空对象而不是删掉这个键 —— setShortcutKey 取它时不做"缺键"兜底。
void Setting::restoreShortcutKeyDefaults()
{
    configObj.SetNamedValue(L"shortcutKey", JsonObject{});
    save();
    applyHotkeys();
}

// 没有任何键位覆盖项 = 仍是出厂键位。单行清空（Backspace/Delete）也会写进配置对象，
// 所以只要对象非空就说明用户改过，此时「恢复默认」才可点。
bool Setting::shortcutKeyAtDefaults()
{
    auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
    return !obj || obj.Size() == 0;
}

// 全局热键「重复截图」：记住的上一块选区（屏幕坐标），存成 "x,y,w,h"。
bool Setting::getLastCaptureRect(int& x, int& y, int& w, int& h)
{
    const std::wstring v = featStr(L"lastCaptureRect", L"");
    if (v.empty()) return false;
    int a[4]{ 0, 0, 0, 0 };
    size_t idx = 0, pos = 0;
    while (idx < 4) {
        const size_t comma = v.find(L',', pos);
        const std::wstring part = v.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        try { a[idx] = std::stoi(part); } catch (...) { return false; }
        ++idx;
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    if (idx != 4) return false;
    x = a[0]; y = a[1]; w = a[2]; h = a[3];
    return w > 0 && h > 0;
}

void Setting::setLastCaptureRect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    setFeatStr(L"lastCaptureRect", wfmt(L"%d,%d,%d,%d", x, y, w, h));
}

// 旧版本写入的 AI 相关配置：AI 对话（模型 / 新建会话 / 对话 API 配置）、AI 视觉 OCR
// （视觉理解模型 / HTML、Markdown 提示词）、AI 翻译（翻译提示词 / 领域 / 自定义 API）。
// 这些功能已整体移除，启动时从 config.json 中删除 —— 既不留无效键，也不让用户填过的
// API Key 等敏感项长期留在磁盘上。仅在确实删除到内容时才回写文件。
void Setting::purgeRemovedAiSettings()
{
    static const wchar_t* const kRemovedFeatKeys[] = {
        // AI 对话
        L"chatModel", L"chatNewSessionOnOpen", L"chatNewSessionOnClose", L"chatApiConfigs",
        // AI 视觉 OCR
        L"htmlVisionModel", L"ocrHtmlPrompt", L"ocrMarkdownPrompt",
        // AI 翻译
        L"translatePrompt", L"translateEnabledModels",
        L"translateApiProvider", L"translateApiUrl", L"translateApiKey", L"translateApiModel",
        L"translateApiAppId", L"translateApiSecret", L"translateApiName",
        L"translateDomain", L"translateDomainCustom",
    };
    bool changed = false;
    if (configObj.HasKey(L"features")) {
        auto features = configObj.GetNamedObject(L"features");
        for (auto* key : kRemovedFeatKeys) {
            if (features.HasKey(key)) {
                features.Remove(key);
                changed = true;
            }
        }
    }
    // 旧「AI 对话」历史（toolPin/chat）：快捷翻译改用独立键 quickTranslate/history，
    // 旧会话不再展示，直接清掉。
    auto toolRoot = configObj.GetNamedObject(L"toolPin", nullptr);
    if (toolRoot && toolRoot.HasKey(L"chat")) {
        toolRoot.Remove(L"chat");
        changed = true;
    }
    if (changed) save();
}

// 旧程序 ScreenCapture（本工程前身）写入 shortcutKey 的历史键位只有一条 `cap`（区域截图，
// 默认 Ctrl+Alt+A）。现用 id 为 capture（见 kHotkeys），`cap` 不在表内、永不会被注册，
// 启动时删除，避免残留随 config.json 长期留存。
void Setting::purgeLegacyShortcutKeys()
{
    static const wchar_t* const kLegacyIds[] = { L"cap" };
    auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!obj) return;
    bool changed = false;
    for (auto* id : kLegacyIds) {
        if (obj.HasKey(id)) {
            obj.Remove(id);
            changed = true;
        }
    }
    if (changed) save();
}

void Setting::setAutoStart(bool autoStart)
{
    std::wstring runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    // 无论开关与否，先移除两种自启方式，避免残留旧的注册表项/计划任务。
    {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValue(hKey, L"SnowAir");
            RegCloseKey(hKey);
        }
    }
    runQuiet(L"schtasks /Delete /TN \"SnowAir\" /F");

    std::wstring selfPath;   // 当前 exe 路径：登记后记进配置，供启动时判断是否需要接管
    if (autoStart) {
        wchar_t buffer[MAX_PATH];
        GetModuleFileName(nullptr, buffer, MAX_PATH);
        auto curPath = std::filesystem::path(buffer);
        selfPath = curPath.wstring();
        std::wstring commandLine = wfmt(L"\"%s\" --auto-start", curPath.wstring().c_str());

        if (getRunAsAdmin() && !Util::isBuiltinAdminAccount()) {
            // 需要静默提权（普通管理员账户 + UAC 过滤）：用计划任务（最高权限）在登录时启动。
            // 计划任务不会出现在「启动项」里，这是系统的取舍。
            runQuiet(wfmt(
                L"schtasks /Create /TN \"SnowAir\" /SC ONLOGON /RL HIGHEST /F /TR \"%s\"", commandLine.c_str()));
        }
        else {
            // 普通自启，或内置 Administrator 账户（天然提权，无需计划任务）：写当前用户 Run 键，
            // 这样会出现在「任务管理器 → 启动」里。
            HKEY hKey;
            if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
                RegSetValueEx(hKey, L"SnowAir", 0, REG_SZ, (const BYTE*)commandLine.data(), (DWORD)((commandLine.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(hKey);
            }
        }
    }
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"autoStart", JsonValue::CreateBooleanValue(autoStart));
    // 记账：这次为哪个 exe 登记的自启。启动时据此判断登记项是否还指着自己（换目录/升级后接管）
    if (autoStart) common.SetNamedValue(L"autoStartExe", JsonValue::CreateStringValue(selfPath));
    else if (common.HasKey(L"autoStartExe")) common.Remove(L"autoStartExe");
    save();
}

bool Setting::getAutoStart()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    // 出厂默认开启开机自启；老配置里没这一项时也按开启处理
    return common && common.GetNamedBoolean(L"autoStart", true);
}

// 启动时调用：开了自启就确保登记项指向当前这个 exe。软件的配置落在 %AppData%（除非 exe 同目录
// 放了便携 config.json），所以"换目录/重装/升级"后，旧的自启项（Run 项或计划任务）仍可能指着
// 老 exe —— 老 exe 要么已被删、要么是旧版本。这里按当前 exe 重登记一次，把旧项接管过来。
// 只在配置里记下的 exe 与当前不一致时动作，正常情况下零开销。
void Setting::takeOverAutoStart()
{
    if (!getAutoStart()) return;
    wchar_t buffer[MAX_PATH]{};
    if (!GetModuleFileName(nullptr, buffer, MAX_PATH)) return;
    const std::wstring cur{ buffer };
    auto common = configObj.GetNamedObject(L"common", nullptr);
    const std::wstring recorded = common
        ? std::wstring{ common.GetNamedString(L"autoStartExe", L"") } : L"";
    if (!recorded.empty() && _wcsicmp(recorded.c_str(), cur.c_str()) == 0) return;
    setAutoStart(true);
}

bool Setting::getRunAsAdmin()
{
    // 内置 Administrator 账户天然就是管理员（不受 UAC 过滤），恒视为开启。
    if (Util::isBuiltinAdminAccount()) return true;
    auto common = configObj.GetNamedObject(L"common", nullptr);
    return common && common.GetNamedBoolean(L"runAsAdmin", false);
}

void Setting::setRunAsAdmin(bool on)
{
    // 内置 Administrator 账户天然提权，开关关不掉：直接忽略（UI 侧点关闭也只会弹通知）。
    if (Util::isBuiltinAdminAccount()) return;
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"runAsAdmin", JsonValue::CreateBooleanValue(on));
    save();
    // 关机自启若处于开启状态，按新的权限方式重新注册。
    if (getAutoStart()) setAutoStart(true);
}

std::filesystem::path Setting::initDataPath()
{
    PWSTR pathTmp;
    auto hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pathTmp);
    if (FAILED(hr)) {
        _ASSERT_EXPR(FALSE, L"get roaming path，error");
        return L"";
    }
    auto dataPath = std::filesystem::path{ pathTmp };
    CoTaskMemFree(pathTmp);
    dataPath.append("SnowAir");
    if (!std::filesystem::exists(dataPath)) {
        if (!std::filesystem::create_directories(dataPath)) {
            _ASSERT_EXPR(FALSE, L"create data path，error");
        }
    }
    return dataPath;
}

std::filesystem::path Setting::initConfigPath()
{
    // 与插件的查找顺序一致（见 Util.cpp 里的 findImageReader）：先看 exe 同目录。
    // 只有那份文件本来就存在时才认它 —— 不存在就不要在程序目录里新建，
    // 装在 Program Files 下时那儿通常没有写权限，况且默认位置该是 appdata
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileName(nullptr, buffer, MAX_PATH);
    auto path = std::filesystem::path{ buffer }.parent_path().append(L"config.json");
    if (std::filesystem::exists(path)) return path;
    auto fallback = this->dataPath; //复制一份路径对象，append 会就地改
    return fallback.append(L"config.json");
}

void Setting::save()
{
    // 这次启动没读出配置：只读运行，避免默认值把用户的原文件冲掉
    if (configReadOnly) return;
    // 设置项会从多个线程改（下载回调、剪贴板监听、热键线程），串行化落盘，
    // 免得两处同时写临时文件/替换目标。原子性本身由 writeConfigAtomic 保证。
    static std::mutex saveMutex;
    const std::wstring str{ configObj.Stringify() };
    std::lock_guard<std::mutex> lock(saveMutex);
    writeConfigAtomic(configPath, str);
}

std::wstring Setting::getLang()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return L"zh-CN";
    return std::wstring{ common.GetNamedString(L"language", L"zh-CN") };
}

void Setting::setLang(const std::wstring& langCode)
{
    auto common = setting->configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        setting->configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"language", JsonValue::CreateStringValue(langCode));
    setting->save();
	Lang::get()->initLang(langCode);
}

JsonObject Setting::getToolObj(const std::wstring& tool)
{
    // 用带默认值的重载：这两层在旧配置文件里都不存在，直接 GetNamedObject 会抛异常，
    // 值被手工改成非对象时它也一样返回默认值，不会炸
    auto root = configObj.GetNamedObject(L"toolPin", nullptr);
    if (!root) {
        root = JsonObject();
        configObj.SetNamedValue(L"toolPin", root);
    }
    auto obj = root.GetNamedObject(tool, nullptr);
    if (!obj) {
        obj = JsonObject();
        root.SetNamedValue(tool, obj);
    }
    return obj;
}

bool Setting::getToolFlag(const std::wstring& tool, const std::wstring& key, bool def)
{
    return getToolObj(tool).GetNamedBoolean(key, def);
}

void Setting::setToolFlag(const std::wstring& tool, const std::wstring& key, bool val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateBooleanValue(val));
    save();
}

float Setting::getToolNum(const std::wstring& tool, const std::wstring& key, float def)
{
    return static_cast<float>(getToolObj(tool).GetNamedNumber(key, def));
}

void Setting::setToolNum(const std::wstring& tool, const std::wstring& key, float val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(val));
    save();
}

UINT32 Setting::getToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key)) return def;
    return static_cast<UINT32>(obj.GetNamedNumber(key));
}

void Setting::setToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(static_cast<double>(val)));
    save();
}

std::wstring Setting::getToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key)) return def;
    return std::wstring{ obj.GetNamedString(key, def) };
}

void Setting::setToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateStringValue(val));
    save();
}

void Setting::removeToolKey(const std::wstring& tool, const std::wstring& key)
{
    // 只删已存在的键，不额外创建 toolPin/<tool> 这类空对象
    auto root = configObj.GetNamedObject(L"toolPin", nullptr);
    if (!root) return;
    auto obj = root.GetNamedObject(tool, nullptr);
    if (!obj || !obj.HasKey(key)) return;
    obj.Remove(key);
    save();
}

long long Setting::getUpdateCheckDay()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0;
    return static_cast<long long>(common.GetNamedNumber(L"updateCheckDay", 0));
}

void Setting::setUpdateCheckDay(long long day)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return;
    // 这项不写进 defaultConfig：它是程序自己的记账，不是给用户改的配置
    common.SetNamedValue(L"updateCheckDay", JsonValue::CreateNumberValue(static_cast<double>(day)));
    save();
}

int Setting::getUpdateCheckFreq()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0;
    const int v = static_cast<int>(common.GetNamedNumber(L"updateCheckFreq", 0));
    return std::clamp(v, 0, 3);
}

void Setting::setUpdateCheckFreq(int v)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"updateCheckFreq",
        JsonValue::CreateNumberValue(static_cast<double>(std::clamp(v, 0, 3))));
    save();
}

JsonObject Setting::featuresObj()
{
	if (!configObj.HasKey(L"features")) {
		configObj.SetNamedValue(L"features", JsonObject{});
	}
	return configObj.GetNamedObject(L"features");
}

bool Setting::featBool(const wchar_t* key, bool def)
{
	return featuresObj().GetNamedBoolean(key, def);
}
void Setting::setFeatBool(const wchar_t* key, bool v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateBooleanValue(v));
	save();
}
int Setting::featInt(const wchar_t* key, int def)
{
	return (int)featuresObj().GetNamedNumber(key, def);
}
void Setting::setFeatInt(const wchar_t* key, int v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateNumberValue(v));
	save();
}
std::wstring Setting::featStr(const wchar_t* key, const wchar_t* def)
{
	return featuresObj().GetNamedString(key, def).c_str();
}
void Setting::setFeatStr(const wchar_t* key, const std::wstring& v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateStringValue(v));
	save();
}
JsonArray Setting::featArray(const wchar_t* key)
{
	auto f = featuresObj();
	if (!f.HasKey(key) || f.GetNamedValue(key).ValueType() != JsonValueType::Array)
		return JsonArray{};
	return f.GetNamedArray(key);
}
void Setting::setFeatArray(const wchar_t* key, const JsonArray& arr)
{
	featuresObj().SetNamedValue(key, arr);
	save();
}

int Setting::getOcrEngine() { return featInt(L"ocrEngine", 0); }
void Setting::setOcrEngine(int v) { setFeatInt(L"ocrEngine", v); }
int Setting::getOcrPackVariant() { return std::clamp(featInt(L"ocrPackVariant", 0), 0, 1); }
void Setting::setOcrPackVariant(int v) { setFeatInt(L"ocrPackVariant", std::clamp(v, 0, 1)); }
int Setting::getOcrResultMode() { return featInt(L"ocrResultMode", 0); }
void Setting::setOcrResultMode(int v) { setFeatInt(L"ocrResultMode", std::clamp(v, 0, 1)); }
std::wstring Setting::getTranslateProvider() { return featStr(L"translateProvider", L"microsoft"); }
void Setting::setTranslateProvider(const std::wstring& v) { setFeatStr(L"translateProvider", v); }
std::wstring Setting::getTranslateSourceLang() { return featStr(L"translateSourceLang", L"auto"); }
void Setting::setTranslateSourceLang(const std::wstring& v) { setFeatStr(L"translateSourceLang", v); }
// 默认目标语言为英语（即默认中译英）
std::wstring Setting::getTranslateTargetLang() { return featStr(L"translateTargetLang", L"en"); }
void Setting::setTranslateTargetLang(const std::wstring& v) { setFeatStr(L"translateTargetLang", v); }
bool Setting::getTranslateDictMode() { return featBool(L"translateDictMode", false); }
void Setting::setTranslateDictMode(bool on) { setFeatBool(L"translateDictMode", on); }
bool Setting::getFeatureTips() { return featBool(L"featureTips", true); }
void Setting::setFeatureTips(bool on) { setFeatBool(L"featureTips", on); }
bool Setting::getHotkeyTip() { return featBool(L"hotkeyTip", false); }
void Setting::setHotkeyTip(bool on) { setFeatBool(L"hotkeyTip", on); }

bool Setting::getFindWindowElements() { return featBool(L"findWindowElements", true); }
void Setting::setFindWindowElements(bool on) { setFeatBool(L"findWindowElements", on); }
int Setting::getTrayClickAction() { return featInt(L"trayClickAction", 0); }
void Setting::setTrayClickAction(int v) { setFeatInt(L"trayClickAction", v); }
bool Setting::getPinZoomAtCursor() { return featBool(L"pinZoomAtCursor", false); }
void Setting::setPinZoomAtCursor(bool on) { setFeatBool(L"pinZoomAtCursor", on); }
bool Setting::getPinAutoOcr() { return featBool(L"pinAutoOcr", false); }
void Setting::setPinAutoOcr(bool on) { setFeatBool(L"pinAutoOcr", on); }
bool Setting::getPinAutoFit() { return featBool(L"pinAutoFit", true); }
void Setting::setPinAutoFit(bool on) { setFeatBool(L"pinAutoFit", on); }
int Setting::getPinDoubleClickAction() { return featInt(L"pinDoubleClickAction", 0); }
void Setting::setPinDoubleClickAction(int v) { setFeatInt(L"pinDoubleClickAction", v); }
int Setting::getPinOpacity() { return featInt(L"pinOpacity", 100); }
void Setting::setPinOpacity(int v) { setFeatInt(L"pinOpacity", v); }
int Setting::getDemoDefaultTool() { return featInt(L"demoDefaultTool", 0); }
void Setting::setDemoDefaultTool(int v) { setFeatInt(L"demoDefaultTool", v); }
int Setting::getDemoEdgeBehavior() { return featInt(L"demoEdgeBehavior", 0); }
void Setting::setDemoEdgeBehavior(int v) { setFeatInt(L"demoEdgeBehavior", v); }
int Setting::getToolDefaultWidth()
{
	return std::clamp(featInt(L"toolDefaultWidth", 4), 1, 10);
}
void Setting::setToolDefaultWidth(int v)
{
	setFeatInt(L"toolDefaultWidth", std::clamp(v, 1, 10));
}
// —— 初始属性 ——
namespace {
	JsonObject toolDesignRoot(JsonObject& features)
	{
		if (!features.HasKey(L"toolDesign")
			|| features.GetNamedValue(L"toolDesign").ValueType() != JsonValueType::Object)
			features.SetNamedValue(L"toolDesign", JsonObject{});
		return features.GetNamedObject(L"toolDesign");
	}
}
bool Setting::hasToolDesign(const std::wstring& toolId)
{
	if (toolId.empty()) return false;
	auto features = featuresObj();
	if (!features.HasKey(L"toolDesign")
		|| features.GetNamedValue(L"toolDesign").ValueType() != JsonValueType::Object)
		return false;
	auto design = features.GetNamedObject(L"toolDesign");
	if (!design.HasKey(toolId)) return false;
	return design.GetNamedValue(toolId).ValueType() == JsonValueType::Object;
}
int Setting::getToolDesignInt(const std::wstring& toolId, const std::wstring& key, int def)
{
	if (toolId.empty()) return def;
	auto features = featuresObj();
	if (!features.HasKey(L"toolDesign")
		|| features.GetNamedValue(L"toolDesign").ValueType() != JsonValueType::Object)
		return def;
	auto design = features.GetNamedObject(L"toolDesign");
	if (!design.HasKey(toolId)) return def;
	auto one = design.GetNamedObject(toolId, nullptr);
	if (!one || !one.HasKey(key)) return def;
	return (int)one.GetNamedNumber(key, (double)def);
}
void Setting::setToolDesignInt(const std::wstring& toolId, const std::wstring& key, int value)
{
	if (toolId.empty() || key.empty()) return;
	auto features = featuresObj();
	auto design = toolDesignRoot(features);
	JsonObject one = design.HasKey(toolId) ? design.GetNamedObject(toolId, JsonObject{}) : JsonObject{};
	one.SetNamedValue(key, JsonValue::CreateNumberValue((double)value));
	design.SetNamedValue(toolId, one);
	features.SetNamedValue(L"toolDesign", design);
	features.SetNamedValue(L"toolDesignUsed", JsonValue::CreateBooleanValue(true));
	save();
}
std::wstring Setting::getToolDesignStr(const std::wstring& toolId, const std::wstring& key, const std::wstring& def)
{
	if (toolId.empty() || key.empty()) return def;
	auto features = featuresObj();
	if (!features.HasKey(L"toolDesign")
		|| features.GetNamedValue(L"toolDesign").ValueType() != JsonValueType::Object)
		return def;
	auto design = features.GetNamedObject(L"toolDesign");
	if (!design.HasKey(toolId)) return def;
	auto one = design.GetNamedObject(toolId, nullptr);
	if (!one || !one.HasKey(key)) return def;
	auto v = one.GetNamedValue(key);
	if (v.ValueType() != JsonValueType::String) return def;
	return std::wstring{ one.GetNamedString(key, def.c_str()) };
}
void Setting::setToolDesignStr(const std::wstring& toolId, const std::wstring& key, const std::wstring& value)
{
	if (toolId.empty() || key.empty()) return;
	auto features = featuresObj();
	auto design = toolDesignRoot(features);
	JsonObject one = design.HasKey(toolId) ? design.GetNamedObject(toolId, JsonObject{}) : JsonObject{};
	one.SetNamedValue(key, JsonValue::CreateStringValue(value));
	design.SetNamedValue(toolId, one);
	features.SetNamedValue(L"toolDesign", design);
	features.SetNamedValue(L"toolDesignUsed", JsonValue::CreateBooleanValue(true));
	save();
}
bool Setting::hasAnyToolDesign()
{
	return featBool(L"toolDesignUsed", false);
}
void Setting::clearToolDesign()
{
	auto features = featuresObj();
	if (features.HasKey(L"toolDesign")) features.Remove(L"toolDesign");
	features.SetNamedValue(L"toolDesignUsed", JsonValue::CreateBooleanValue(false));
	save();
}
bool Setting::getRememberToolStyle() { return featBool(L"rememberToolStyle", false); }
void Setting::setRememberToolStyle(bool on) { setFeatBool(L"rememberToolStyle", on); }
// 悬停预览：历史记录 / 剪切板卡片停悬时弹出大图或文字卡（默认关闭）
bool Setting::getHoverPreview() { return featBool(L"hoverPreview", false); }
void Setting::setHoverPreview(bool on) { setFeatBool(L"hoverPreview", on); }
bool Setting::getRackFlag(const wchar_t* key, bool def) { return featBool(key, def); }
void Setting::setRackFlag(const wchar_t* key, bool on) { setFeatBool(key, on); }
int Setting::getRackInt(const wchar_t* key, int def) { return featInt(key, def); }
void Setting::setRackInt(const wchar_t* key, int value) { setFeatInt(key, value); }
int Setting::getLastArrowHead() { return featInt(L"lastArrowHead", 0); }
void Setting::setLastArrowHead(int v) { setFeatInt(L"lastArrowHead", v); }
bool Setting::getAutoSaveAfterCapture() { return featBool(L"autoSaveAfterCapture", false); }
void Setting::setAutoSaveAfterCapture(bool on) { setFeatBool(L"autoSaveAfterCapture", on); }
std::wstring Setting::getSaveFormat() { return featStr(L"saveFormat", L"PNG (*.png)"); }
void Setting::setSaveFormat(const std::wstring& v) { setFeatStr(L"saveFormat", v); }
bool Setting::getCopyAsFile() { return featBool(L"copyAsFile", false); }
void Setting::setCopyAsFile(bool on) { setFeatBool(L"copyAsFile", on); }
bool Setting::getCancelConfirmDialog() { return featBool(L"cancelConfirmDialog", false); }
void Setting::setCancelConfirmDialog(bool on) { setFeatBool(L"cancelConfirmDialog", on); }

void Setting::restoreFunctionScreenshotDefaults()
{
	setAutoStart(true);
	setRunAsAdmin(false);
	setFindWindowElements(true);
	setCancelConfirmDialog(false);
	setAutoSaveAfterCapture(false);
	setCopyAsFile(false);
	setSaveFormat(L"PNG (*.png)");
	setManualSaveFormat(L"");
	setAutoSaveFormat(L"");
	setVideoRecordFormat(L"");
	setTrayClickAction(0);
	setDemoDefaultTool(0);
	setDemoEdgeBehavior(0);
	setPinZoomAtCursor(false);
	setPinAutoOcr(false);
	setPinAutoFit(true);
	setPinDoubleClickAction(0);
	setPinOpacity(100);
}

// 「功能设置」各子页的「恢复默认」：把该子页的每一项写回出厂值。
// 视频录制：编码/画质/帧率/GIF/声音/录制附加项，外加高亮圆颜色（删覆盖项 → 回落默认品牌绿）
void Setting::restoreFunctionRecordDefaults()
{
	setVideoQuality(4);
	setVideoFps(0);
	setGifQuality(1);
	setGifFps(1);
	setGifFormat(0);
	setRecordMic(0);
	setVideoEncoder(0);
	setEncodeSpeed(1);
	setHwAccel(true);
	setHideToolbarInRecord(true);
	setShowKeystrokes(false);
	setSelFollowMouse(false);
	setRecordToolbarPeek(false);
	setRecordDefaultPierce(false);
	setMouseHighlight(false);
	featuresObj().Remove(L"mouseHighlightColor");
	save();
}

// 文件输出：文件名模板与目录清空即"没改过"，界面回到灰占位符（默认模板 / 默认目录）
void Setting::restoreFunctionOutputDefaults()
{
	setManualSaveFormat(L"");
	setAutoSaveFormat(L"");
	setScreenshotDir(L"");
	setVideoRecordFormat(L"");
	setRecordDir(L"");
}

// 文本识别：回到默认的离线 PP-OCRv6（未安装时运行时会自动回落系统 OCR）
void Setting::restoreFunctionOcrDefaults()
{
	setOcrEngine(0);
	setOcrResultMode(0);
}

// 翻译：回到默认在线服务与「非」字典模式；语种不在本子页里，不动
void Setting::restoreFunctionTranslateDefaults()
{
	setTranslateProvider(L"microsoft");
	setTranslateDictMode(false);
}

// —— 与上面的 restore* 成对：判断该子页是否**仍是出厂值**（供「恢复默认」按钮置灰）——
// 判据统一取"restore* 会不会真的改动什么"：每项都与 restore 写回的值比一遍。改默认值时两处一起改。
bool Setting::functionScreenshotAtDefaults()
{
	// 内置 Administrator 账户的「以管理员身份运行」恒为开且无法关闭，故不参与判定；
	// 否则该子页永远不会被判为默认状态，「恢复默认」按钮将始终可用
	const bool runAsAdminDefault = Util::isBuiltinAdminAccount() || !getRunAsAdmin();
	return getAutoStart() && runAsAdminDefault
		&& getFindWindowElements() && !getCancelConfirmDialog() && !getAutoSaveAfterCapture()
		&& !getCopyAsFile() && getSaveFormat() == L"PNG (*.png)"
		&& getManualSaveFormat().empty() && getAutoSaveFormat().empty()
		&& getVideoRecordFormat().empty() && getTrayClickAction() == 0
		&& getDemoDefaultTool() == 0 && getDemoEdgeBehavior() == 0
		&& !getPinZoomAtCursor() && !getPinAutoOcr() && getPinAutoFit()
		&& getPinDoubleClickAction() == 0 && getPinOpacity() == 100;
}

bool Setting::functionRecordAtDefaults()
{
	return getVideoQuality() == 4 && getVideoFps() == 0 && getGifQuality() == 1
		&& getGifFps() == 1 && getGifFormat() == 0 && getRecordMic() == 0
		&& getVideoEncoder() == 0 && getEncodeSpeed() == 1 && getHwAccel()
		&& getHideToolbarInRecord() && !getShowKeystrokes() && !getSelFollowMouse()
		&& !getRecordToolbarPeek() && !getRecordDefaultPierce()
		&& !getMouseHighlight() && getMouseHighlightColor() == 0x0FDC78FF;
}

bool Setting::functionOutputAtDefaults()
{
	return getManualSaveFormat().empty() && getAutoSaveFormat().empty()
		&& getScreenshotDir().empty()
		&& getVideoRecordFormat().empty() && getRecordDir().empty();
}

bool Setting::functionOcrAtDefaults()
{
	return getOcrEngine() == 0 && getOcrResultMode() == 0;
}

bool Setting::functionTranslateAtDefaults()
{
	return getTranslateProvider() == L"microsoft" && !getTranslateDictMode();
}

int Setting::getVideoQuality() { return featInt(L"videoQuality", 4); }
void Setting::setVideoQuality(int v) { setFeatInt(L"videoQuality", v); }
int Setting::getVideoFps() { return featInt(L"videoFps", 0); }
void Setting::setVideoFps(int v) { setFeatInt(L"videoFps", v); }
int Setting::getGifQuality() { return featInt(L"gifQuality", 1); }
void Setting::setGifQuality(int v) { setFeatInt(L"gifQuality", v); }
int Setting::getGifFps() { return featInt(L"gifFps", 1); }
void Setting::setGifFps(int v) { setFeatInt(L"gifFps", v); }
int Setting::getGifFormat() { return featInt(L"gifFormat", 0); }
void Setting::setGifFormat(int v) { setFeatInt(L"gifFormat", v); }
int Setting::getRecordMic() { return featInt(L"recordMic", 0); }
void Setting::setRecordMic(int v) { setFeatInt(L"recordMic", v); }
int Setting::getVideoEncoder() { return featInt(L"videoEncoder", 0); }
void Setting::setVideoEncoder(int v) { setFeatInt(L"videoEncoder", v); }
int Setting::getEncodeSpeed() { return featInt(L"encodeSpeed", 1); }
void Setting::setEncodeSpeed(int v) { setFeatInt(L"encodeSpeed", v); }
bool Setting::getHwAccel() { return featBool(L"hwAccel", true); }
void Setting::setHwAccel(bool on) { setFeatBool(L"hwAccel", on); }
bool Setting::getHideToolbarInRecord() { return featBool(L"hideToolbarInRecord", true); }
void Setting::setHideToolbarInRecord(bool on) { setFeatBool(L"hideToolbarInRecord", on); }
bool Setting::getShowKeystrokes() { return featBool(L"showKeystrokes", false); }
void Setting::setShowKeystrokes(bool on) { setFeatBool(L"showKeystrokes", on); }
bool Setting::getSelFollowMouse() { return featBool(L"selFollowMouse", false); }
void Setting::setSelFollowMouse(bool on) { setFeatBool(L"selFollowMouse", on); }
bool Setting::getRecordToolbarPeek() { return featBool(L"recordToolbarPeek", false); }
void Setting::setRecordToolbarPeek(bool on) { setFeatBool(L"recordToolbarPeek", on); }
bool Setting::getRecordDefaultPierce() { return featBool(L"recordDefaultPierce", false); }
void Setting::setRecordDefaultPierce(bool on) { setFeatBool(L"recordDefaultPierce", on); }
bool Setting::getMouseHighlight() { return featBool(L"mouseHighlight", false); }
void Setting::setMouseHighlight(bool on) { setFeatBool(L"mouseHighlight", on); }
UINT32 Setting::getMouseHighlightColor()
{
	auto f = featuresObj();
	if (!f.HasKey(L"mouseHighlightColor")) return 0x0FDC78FF;   // 默认品牌绿
	return static_cast<UINT32>(f.GetNamedNumber(L"mouseHighlightColor"));
}
void Setting::setMouseHighlightColor(UINT32 rgba)
{
	featuresObj().SetNamedValue(L"mouseHighlightColor", JsonValue::CreateNumberValue(static_cast<double>(rgba)));
	save();
}

std::wstring Setting::defaultManualSaveFormat() { return L"SnowAir_{{YYYY-MM-DD_HH-mm-ss}}"; }
std::wstring Setting::defaultAutoSaveFormat() { return L"SnowAir_auto_{{YYYY-MM-DD_HH-mm-ss}}"; }
std::wstring Setting::defaultVideoRecordFormat() { return L"SnowAir_Video_{{YYYY-MM-DD_HH-mm-ss}}"; }

std::wstring Setting::getManualSaveFormat() { return featStr(L"manualSaveFormat", L""); }
void Setting::setManualSaveFormat(const std::wstring& v) { setFeatStr(L"manualSaveFormat", v); }
std::wstring Setting::getAutoSaveFormat() { return featStr(L"autoSaveFormat", L""); }
void Setting::setAutoSaveFormat(const std::wstring& v) { setFeatStr(L"autoSaveFormat", v); }
std::wstring Setting::getVideoRecordFormat() { return featStr(L"videoRecordFormat", L""); }
void Setting::setVideoRecordFormat(const std::wstring& v) { setFeatStr(L"videoRecordFormat", v); }

std::wstring Setting::getScreenshotDir() { return featStr(L"screenshotDir", L""); }
void Setting::setScreenshotDir(const std::wstring& v) { setFeatStr(L"screenshotDir", v); }
std::wstring Setting::getRecordDir() { return featStr(L"recordDir", L""); }
void Setting::setRecordDir(const std::wstring& v) { setFeatStr(L"recordDir", v); }

int Setting::getHistoryRetentionDays() { return featInt(L"historyRetentionDays", -1); }
void Setting::setHistoryRetentionDays(int days) { setFeatInt(L"historyRetentionDays", days); }
bool Setting::getDisableCaptureHistory() { return featBool(L"disableCaptureHistory", false); }
void Setting::setDisableCaptureHistory(bool on) { setFeatBool(L"disableCaptureHistory", on); }
int Setting::getAppTheme() { return featInt(L"appTheme", 0); }
void Setting::setAppTheme(int v) { setFeatInt(L"appTheme", v); }

// 全局热键。RegisterHotKey 是"先注册先独占"的语义，没有可调优先级：组合键已被其他进程注册
// 时，本次注册会直接失败。所以这里的策略是：
//   1) 每条都先正常 RegisterHotKey；
//   2) 失败的那些攒起来，整体装一个低级键盘钩子兜底 —— 钩子先于常规热键与加速键取得按键，
//      命中后消费该事件，效果上等同于该组合由本进程持有；
//   3) 同时起一个低频重试，等其他进程释放该组合后再切回 RegisterHotKey（并摘掉钩子）。
void Setting::applyHotkeys()
{
    auto* lingApp = Ling::App::get();
    if (!lingApp) return;
    // 先全部注销，再按当前配置重注册（配置改过、或从钩子兜底切回都要走这一遍）
    for (const auto& hk : kHotkeys) lingApp->unRegHotKey(hk.msgId);

    std::vector<HotkeyHook::Binding> fallback;
    for (const auto& hk : kHotkeys) {
        const std::wstring chord = getShortcutKey(hk.id);
        if (chord.empty()) continue;   // 用户清空过这一行 = 不绑
        if (lingApp->regHotKey(chord, hk.msgId)) continue;
        const std::wstring id{ hk.id };
        fallback.push_back({ chord, [id]() {
            if (auto* self = get()) self->runHotkeyAction(id);
        } });
    }
    HotkeyHook::setBindings(std::move(fallback));
    if (HotkeyHook::installed()) startHotkeyRetry();
    else stopHotkeyRetry();
}

// —— 快速贴屏：优先贴「当前上下文」，没有上下文时贴剪贴板 ——
//   ① 资源管理器里单击选中的文件 / 文件夹 / 图片 / 视频（不需要先复制）
//   ② 前台窗口里拖选的一段文字（UIA 选区，不需要先复制）
//   ③ 否则贴当前剪贴板内容（文字 / 图片 / 文件）
namespace {
    bool quickPinIsImagePath(const std::wstring& path)
    {
        const auto dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring e = path.substr(dot + 1);
        for (auto& c : e) c = (wchar_t)towlower(c);
        return e == L"png" || e == L"jpg" || e == L"jpeg" || e == L"webp"
            || e == L"bmp" || e == L"gif" || e == L"avif" || e == L"jxl";
    }

    // 图片贴到工作区中央（与历史页「贴图」按钮同一套落点）
    void quickPinImage(int w, int h, std::vector<BYTE>& px,
                       const std::wstring& sourcePath = L"")
    {
        if (w <= 0 || h <= 0 || px.empty()) return;
        auto* s = Setting::get();
        if (s && s->getRackFlag(L"rack.quick_pin_hang", false)) {
            std::wstring hangPath = sourcePath;
            if (hangPath.empty()) {
                auto dir = s->getDataPath() / L"RackQuick";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                hangPath = (dir / wfmt(L"quick-%llu.png", GetTickCount64())).wstring();
                if (!Util::saveToFile(hangPath, w, h, px.data())) hangPath.clear();
            }
            if (!hangPath.empty() && PluginHost::instance().loaded()) {
                // "上绳" means the image goes to the rack, not also to a
                // separate pin window on the desktop.
                PluginHost::instance().hangQuickPin(hangPath);
                return;
            }
        }

        RECT rc{};
        SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
        const int areaW = (int)rc.right - (int)rc.left;
        const int areaH = (int)rc.bottom - (int)rc.top;
        WinPin::initFromData(rc.left + (std::max)(0, (areaW - w) / 2),
            rc.top + (std::max)(0, (areaH - h) / 2), w, h, px);
    }

    // 贴一组文件：单张图片按图片贴图，其余（文件夹 / 视频 / 其它文件）贴成文件贴屏
    void quickPinPaths(const std::vector<std::wstring>& paths)
    {
        if (paths.empty()) return;
        if (paths.size() == 1 && quickPinIsImagePath(paths.front())) {
            auto [w, h, px] = Util::loadImagePixels(paths.front());
            if (w > 0 && h > 0 && !px.empty()) {
                quickPinImage(w, h, px, paths.front());
                return;
            }
        }
        ClipPin::showFiles(paths);
    }

    // 从某个 shell 文件夹视图里取出选中项的磁盘路径
    void appendSelectionPaths(IFolderView* folderView, std::vector<std::wstring>& out)
    {
        if (!folderView) return;
        Microsoft::WRL::ComPtr<IShellItemArray> items;
        if (FAILED(folderView->Items(SVGIO_SELECTION,
            IID_PPV_ARGS(items.GetAddressOf()))) || !items) return;
        DWORD n = 0;
        items->GetCount(&n);
        for (DWORD k = 0; k < n; ++k) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(k, item.GetAddressOf())) || !item) continue;
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out.emplace_back(path);
                CoTaskMemFree(path);
            }
        }
    }

    // 桌面图标选区：桌面不是前台资源管理器窗口，按 SWC_DESKTOP 单独找桌面 shell 视图
    std::vector<std::wstring> desktopSelection()
    {
        std::vector<std::wstring> out;
        Microsoft::WRL::ComPtr<IShellWindows> windows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(windows.GetAddressOf()))) || !windows) return out;
        VARIANT loc{};
        loc.vt = VT_I4;
        loc.lVal = CSIDL_DESKTOP;
        VARIANT root{};
        root.vt = VT_EMPTY;
        long shellHwnd = 0;
        Microsoft::WRL::ComPtr<IDispatch> disp;
        if (FAILED(windows->FindWindowSW(&loc, &root, SWC_DESKTOP, &shellHwnd,
            SWFO_NEEDDISPATCH, disp.GetAddressOf())) || !disp) return out;
        Microsoft::WRL::ComPtr<IServiceProvider> provider;
        if (FAILED(disp.As(&provider)) || !provider) return out;
        Microsoft::WRL::ComPtr<IShellBrowser> browser;
        if (FAILED(provider->QueryService(SID_STopLevelBrowser,
            IID_PPV_ARGS(browser.GetAddressOf()))) || !browser) return out;
        Microsoft::WRL::ComPtr<IShellView> view;
        if (FAILED(browser->QueryActiveShellView(view.GetAddressOf())) || !view) return out;
        Microsoft::WRL::ComPtr<IFolderView> folderView;
        if (FAILED(view.As(&folderView)) || !folderView) return out;
        appendSelectionPaths(folderView.Get(), out);
        return out;
    }

    // 资源管理器 / 桌面当前选中的项目
    std::vector<std::wstring> explorerSelection()
    {
        std::vector<std::wstring> out;
        HWND fg = GetForegroundWindow();
        if (!fg) return out;
        wchar_t cls[64]{};
        if (!GetClassNameW(fg, cls, 64)) return out;
        // 桌面（壁纸 / 图标）也算一次「选中文件」的来源
        if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0
            || _wcsicmp(cls, L"SysListView32") == 0)
            return desktopSelection();
        if (_wcsicmp(cls, L"CabinetWClass") != 0 && _wcsicmp(cls, L"ExploreWClass") != 0)
            return out;

        Microsoft::WRL::ComPtr<IShellWindows> windows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(windows.GetAddressOf()))) || !windows) return out;
        long count = 0;
        windows->get_Count(&count);
        const HWND target = GetAncestor(fg, GA_ROOT);
        for (long i = 0; i < count; ++i) {
            VARIANT index{};
            index.vt = VT_I4;
            index.lVal = i;
            Microsoft::WRL::ComPtr<IDispatch> disp;
            if (FAILED(windows->Item(index, disp.GetAddressOf())) || !disp) continue;
            Microsoft::WRL::ComPtr<IServiceProvider> provider;
            if (FAILED(disp.As(&provider)) || !provider) continue;
            Microsoft::WRL::ComPtr<IShellBrowser> browser;
            if (FAILED(provider->QueryService(SID_STopLevelBrowser,
                IID_PPV_ARGS(browser.GetAddressOf()))) || !browser) continue;
            // 只认前台那一个浏览窗口：多开资源管理器时不能贴错窗口的选中项。
            // 两边都归一到顶层窗再比，兼容不同系统版本里浏览器窗口挂在不同层级的情况。
            Microsoft::WRL::ComPtr<IOleWindow> oleWindow;
            HWND viewHwnd{};
            if (FAILED(browser.As(&oleWindow)) || !oleWindow
                || FAILED(oleWindow->GetWindow(&viewHwnd)) || !viewHwnd)
                continue;
            if (GetAncestor(viewHwnd, GA_ROOT) != target) continue;
            Microsoft::WRL::ComPtr<IShellView> view;
            if (FAILED(browser->QueryActiveShellView(view.GetAddressOf())) || !view) continue;
            Microsoft::WRL::ComPtr<IFolderView> folderView;
            if (FAILED(view.As(&folderView)) || !folderView) continue;
            appendSelectionPaths(folderView.Get(), out);
            break;
        }
        return out;
    }

    // 前台窗口里拖选的文字：UIA TextPattern 的当前选区（不改动剪贴板）
    std::wstring foregroundSelectedText()
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return {};
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) return {};   // 自己的窗口不算
        Microsoft::WRL::ComPtr<IUIAutomation> automation;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(automation.GetAddressOf()))) || !automation) return {};
        // 个别程序的 UIA 提供者很慢：给跨进程调用设上限，别把热键回调卡住
        Microsoft::WRL::ComPtr<IUIAutomation2> automation2;
        if (SUCCEEDED(automation.As(&automation2)) && automation2) {
            automation2->put_ConnectionTimeout(200);
            automation2->put_TransactionTimeout(400);
        }

        // 从某个元素上读 TextPattern 选区；不支持就返回空
        auto selectionOf = [](IUIAutomationElement* element) -> std::wstring {
            if (!element) return {};
            Microsoft::WRL::ComPtr<IUnknown> patternUnknown;
            if (FAILED(element->GetCurrentPattern(UIA_TextPatternId,
                patternUnknown.GetAddressOf())) || !patternUnknown) return {};
            Microsoft::WRL::ComPtr<IUIAutomationTextPattern> pattern;
            if (FAILED(patternUnknown.As(&pattern)) || !pattern) return {};
            Microsoft::WRL::ComPtr<IUIAutomationTextRangeArray> ranges;
            if (FAILED(pattern->GetSelection(ranges.GetAddressOf())) || !ranges) return {};
            int n = 0;
            ranges->get_Length(&n);
            std::wstring text;
            for (int i = 0; i < n; ++i) {
                Microsoft::WRL::ComPtr<IUIAutomationTextRange> range;
                if (FAILED(ranges->GetElement(i, range.GetAddressOf())) || !range) continue;
                BSTR part = nullptr;
                if (SUCCEEDED(range->GetText(-1, &part)) && part) {
                    text += part;
                    SysFreeString(part);
                }
            }
            return text;
        };

        std::wstring text;
        // 先看焦点元素：文本控件 / 浏览器文档通常把 TextPattern 挂在焦点元素上
        Microsoft::WRL::ComPtr<IUIAutomationElement> focused;
        if (SUCCEEDED(automation->GetFocusedElement(focused.GetAddressOf())) && focused) {
            int elPid = 0;
            if (SUCCEEDED(focused->get_CurrentProcessId(&elPid)) && elPid == pid)
                text = selectionOf(focused.Get());
        }
        // 焦点元素不提供 TextPattern（比如焦点在整窗上）时退回窗口元素
        if (text.empty()) {
            Microsoft::WRL::ComPtr<IUIAutomationElement> windowElement;
            if (SUCCEEDED(automation->ElementFromHandle(fg, windowElement.GetAddressOf())) && windowElement)
                text = selectionOf(windowElement.Get());
        }
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        return text;
    }

    // 当前剪贴板内容（只读，不写历史索引）
    struct QuickPinPayload {
        std::wstring text;
        std::vector<std::wstring> files;
        int w{ 0 }, h{ 0 };
        std::vector<BYTE> pixels;
    };

    QuickPinPayload readClipboardForPin()
    {
        QuickPinPayload out;
        bool opened = false;
        for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
            if (OpenClipboard(g_clipWnd)) opened = true;
            else Sleep(5);
        }
        if (!opened) return out;
        if (HANDLE hf = GetClipboardData(CF_HDROP)) {
            HDROP drop = static_cast<HDROP>(hf);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len, L'\0');
                if (DragQueryFileW(drop, i, path.data(), len + 1) == 0) continue;
                out.files.push_back(std::move(path));
            }
        }
        else if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                out.text = p;
                GlobalUnlock(h);
            }
        }
        else if (HANDLE ha = GetClipboardData(CF_TEXT)) {
            if (const auto* p = static_cast<const char*>(GlobalLock(ha))) {
                const int len = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
                if (len > 1) {
                    std::wstring w((size_t)len - 1, L'\0');
                    MultiByteToWideChar(CP_ACP, 0, p, -1, w.data(), len);
                    out.text = std::move(w);
                }
                GlobalUnlock(ha);
            }
        }
        else {
            // 优先级与监听端一致：PNG → DIBV5/DIB → CF_BITMAP
            static const UINT pngFmt = RegisterClipboardFormatW(L"PNG");
            bool ok = false;
            if (HANDLE hp = GetClipboardData(pngFmt))
                ok = pngToBgra(hp, out.w, out.h, out.pixels);
            if (!ok) {
                HANDLE hb = GetClipboardData(CF_DIBV5);
                if (!hb) hb = GetClipboardData(CF_DIB);
                if (hb) ok = dibToBgra(hb, out.w, out.h, out.pixels);
            }
            if (!ok) {
                if (HBITMAP hbm = (HBITMAP)GetClipboardData(CF_BITMAP))
                    ok = hbitmapToBgra(hbm, out.w, out.h, out.pixels);
            }
            if (!ok) out.pixels.clear();
        }
        CloseClipboard();
        return out;
    }

    void quickPinFromContext()
    {
        // ① 资源管理器选中项（单击选中，不需要复制）
        auto files = explorerSelection();
        if (!files.empty()) { quickPinPaths(files); return; }
        // ② 前台窗口里拖选的文字
        const std::wstring selected = foregroundSelectedText();
        if (!selected.empty()) { ClipPin::showText(selected); return; }
        // ③ 默认：剪贴板内容
        auto payload = readClipboardForPin();
        if (!payload.files.empty()) { quickPinPaths(payload.files); return; }
        if (!payload.text.empty()) { ClipPin::showText(payload.text); return; }
        if (payload.w > 0 && payload.h > 0) quickPinImage(payload.w, payload.h, payload.pixels);
    }
}

// 一条全局热键要执行的动作。id 取自 kHotkeys。
void Setting::runHotkeyAction(const std::wstring& id)
{
    // 会话开着时，这条热键的键位若是会话里的「系统按键」，先交给会话执行。
    // 例：截图热键 F1 + 系统按键「贴图」F1 —— 第二次 F1 应该贴图；否则热键被 RegisterHotKey
    // 吃掉，窗口收不到按键，init() 又因为会话已开直接返回，表现就是"设了没用"。
    if (auto* cap = WinCap::get()) {
        const std::wstring chord = getShortcutKey(id);
        if (cap->commandFromChord(chord)) return;
    }
    if (id == L"capture") { WinCap::init(); return; }
    if (id == L"capture_repeat") { if (auto* app = App::get()) app->captureLastRegionSilent(); return; }
    if (id == L"capture_fullscreen") { if (auto* app = App::get()) app->captureFullScreenSilent(); return; }
    if (id == L"capture_focused_window") { if (auto* app = App::get()) app->captureFocusedWindowSilent(); return; }
    if (id == L"fullscreen_canvas") { WinCap::beginDemoCanvas(); return; }
    if (id == L"record") { WinCap::beginRecordPick(); return; }
    if (id == L"show_main") { WinSetting::toggle(); return; }
    if (id == L"quick_pin") { quickPinFromContext(); return; }
    if (id == L"rack_quick_hang") { PluginHost::instance().quickHang(); return; }
    if (id == L"rack_toggle") { PluginHost::instance().toggleRack(); return; }
    if (id == L"toggle_pins") { WinPin::toggleAllHidden(); return; }
    if (id == L"open_history") { if (!getDisableCaptureHistory()) WinSetting::togglePage(5); return; }
    if (id == L"open_screenshot_dir") {
        const auto dir = Util::outputDir();
        if (!dir.empty()) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
}

// 设置页开始录制热键前调用：临时注销全部系统热键、摘掉低级钩子，让按键能正常落到设置窗口。
void Setting::suspendShortcutKeys()
{
    if (auto* lingApp = Ling::App::get())
        for (const auto& hk : kHotkeys) lingApp->unRegHotKey(hk.msgId);
    HotkeyHook::uninstall();
    stopHotkeyRetry();
}

// 录制结束（保存或取消）后调用：按配置重新注册全部全局热键
void Setting::resumeShortcutKeys()
{
    applyHotkeys();
}

void Setting::startHotkeyRetry()
{
    if (hotkeyRetryTimer) return;
    hotkeyRetryTick = 0;
    // hwnd 传 NULL 的定时器：回调在装它的线程（UI 线程）的消息泵里跑，不需要窗口
    hotkeyRetryTimer = SetTimer(nullptr, kHotkeyRetryTimerId, 4000, &Setting::onHotkeyRetry);
}

void Setting::stopHotkeyRetry()
{
    if (!hotkeyRetryTimer) return;
    KillTimer(nullptr, hotkeyRetryTimer);
    hotkeyRetryTimer = 0;
    hotkeyRetryTick = 0;
}

void CALLBACK Setting::onHotkeyRetry(HWND, UINT, UINT_PTR id, DWORD)
{
    if (id != kHotkeyRetryTimerId) return;
    auto* self = get();
    if (!self) return;
    // 试满 ~1 分钟就停：钩子还在兜着，热键照样能用，不用一直占着定时器
    if (++self->hotkeyRetryTick > 15) {
        self->stopHotkeyRetry();
        return;
    }
    // 重新走一遍注册：让出组合键的会从钩子兜底切回干净的 RegisterHotKey，
    // 仍被占着的继续留在钩子里 —— 全好了时 applyHotkeys 会把定时器停掉
    self->applyHotkeys();
}

void Setting::initShortcutKeys()
{
    auto* lingApp = Ling::App::get();
    // 先挂派发，再注册：热键一注册就能命中回调，不留"注册好了但还没人接"的缝
    lingApp->onHotKey.add([this](UINT msg) {
        for (const auto& hk : kHotkeys) {
            if (msg == hk.msgId) { runHotkeyAction(hk.id); return; }
        }
    });
    // 已经有一个实例在托盘里时，再运行一次 exe：打开设置中心，而不是直接进截图模式。
    // （"打开就只挂托盘待命"这条规则的延伸：重新打开 = 把软件的窗口调出来）
    lingApp->onSecondInstance.add([this]() {
        WinSetting::init();
    });
    applyHotkeys();
}

// ================ 贴图边框外观 ================
bool Setting::getPinBorderDefaultEnabled() { return featBool(L"pinBorderDefaultEnabled", true); }
void Setting::setPinBorderDefaultEnabled(bool on) { setFeatBool(L"pinBorderDefaultEnabled", on); }
int Setting::getPinBorderWidth() { return featInt(L"pinBorderWidth", 2); }
void Setting::setPinBorderWidth(int v) { setFeatInt(L"pinBorderWidth", v); }
int Setting::getPinBorderRadius() { return featInt(L"pinBorderRadius", 6); }
void Setting::setPinBorderRadius(int v) { setFeatInt(L"pinBorderRadius", v); }
UINT32 Setting::getPinBorderColor()
{
    auto f = featuresObj();
    if (!f.HasKey(L"pinBorderColor")) return 0x34C759FF; // 默认主题绿
    return static_cast<UINT32>(f.GetNamedNumber(L"pinBorderColor"));
}
void Setting::setPinBorderColor(UINT32 argb)
{
    featuresObj().SetNamedValue(L"pinBorderColor", JsonValue::CreateNumberValue(static_cast<double>(argb)));
    save();
}

// ================ 托盘图标 ================
bool Setting::getTrayEnabled() { return featBool(L"trayEnabled", true); }
void Setting::setTrayEnabled(bool on) { setFeatBool(L"trayEnabled", on); }
int Setting::getTrayIconStyle()
{
    // 用新键 trayIconStyle2 存 4 档（0 跟随系统 / 1 主题 / 2 自定义颜色 / 3 自定义图标）。
    // 不能就地复用旧键：旧的 5 档里 暗色=2、亮色=3 与新方案的 2/3 撞号，会互相污染。
    auto f = featuresObj();
    if (f.HasKey(L"trayIconStyle2")) {
        const int v = static_cast<int>(f.GetNamedNumber(L"trayIconStyle2"));
        return (v < 0 || v > 3) ? 1 : v;
    }
    // 首次读取：从旧键一次性迁移（旧：0 跟随系统 / 1 主题 / 2 暗色 / 3 亮色 / 4 自定义）
    int old = 1;
    if (f.HasKey(L"trayIconStyle")) old = static_cast<int>(f.GetNamedNumber(L"trayIconStyle"));
    int nv = 1;
    if (old == 4) nv = 3;                   // 旧「自定义」→ 自定义图标
    else if (old == 2 || old == 3) nv = 1;  // 旧 暗色/亮色 → 主题
    else nv = (old == 0) ? 0 : 1;           // 0 跟随系统 / 其余默认主题
    setFeatInt(L"trayIconStyle2", nv);      // 迁移结果落到新键，之后只读新键
    return nv;
}
void Setting::setTrayIconStyle(int v)
{
    setFeatInt(L"trayIconStyle2", std::max(0, std::min(3, v)));
}
UINT32 Setting::getTrayCustomColor()
{
    auto f = featuresObj();
    if (!f.HasKey(L"trayCustomColor")) return 0x34C759FF;   // 默认主题绿
    return static_cast<UINT32>(f.GetNamedNumber(L"trayCustomColor"));
}
void Setting::setTrayCustomColor(UINT32 rgba)
{
    featuresObj().SetNamedValue(L"trayCustomColor", JsonValue::CreateNumberValue(static_cast<double>(rgba)));
    save();
}
std::wstring Setting::getTrayCustomIconPath() { return featStr(L"trayCustomIconPath", L""); }
void Setting::setTrayCustomIconPath(const std::wstring& path) { setFeatStr(L"trayCustomIconPath", path); }

// ================ 新增：浮动截图按钮 ================
bool Setting::getFloatingCaptureButtonShow() { return featBool(L"floatingCaptureButtonShow", true); }
void Setting::setFloatingCaptureButtonShow(bool on) { setFeatBool(L"floatingCaptureButtonShow", on); }
int Setting::getFloatingCaptureButtonClick() { return featInt(L"floatingCaptureButtonClick", 0); }
void Setting::setFloatingCaptureButtonClick(int v) { setFeatInt(L"floatingCaptureButtonClick", v); }
int Setting::getFloatingLongPressMs() { return featInt(L"floatingLongPressMs", 500); }
void Setting::setFloatingLongPressMs(int ms) { setFeatInt(L"floatingLongPressMs", ms); }

// ================ 新增：窗口黑名单 ================
std::vector<std::wstring> Setting::getWindowBlacklist()
{
    std::vector<std::wstring> list;
    for (auto const& v : featArray(L"windowBlacklist")) {
        if (v.ValueType() == JsonValueType::String)
            list.push_back(v.GetString().c_str());
    }
    return list;
}
void Setting::setWindowBlacklist(const std::vector<std::wstring>& titles)
{
    JsonArray arr;
    for (auto& t : titles) arr.Append(JsonValue::CreateStringValue(t));
    setFeatArray(L"windowBlacklist", arr);
}

// ================ 新增：History 目录 / 条目 / 过期清理 ================
static JsonObject historyStore(JsonObject& features)
{
    if (!features.HasKey(L"history") || features.GetNamedValue(L"history").ValueType() != JsonValueType::Object)
        features.SetNamedValue(L"history", JsonObject{});
    return features.GetNamedObject(L"history");
}

std::filesystem::path Setting::getHistoryThumbDir()
{
    // 只存录屏封面的小图，不是历史副本
    auto dir = dataPath;
    dir.append(L"ThumbCache");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) std::filesystem::create_directories(dir, ec);
    return dir;
}

static JsonObject historyItemToJson(const Setting::HistoryItem& it)
{
    JsonObject o;
    o.SetNamedValue(L"id", JsonValue::CreateStringValue(it.id));
    o.SetNamedValue(L"filePath", JsonValue::CreateStringValue(it.filePath));
    o.SetNamedValue(L"source", JsonValue::CreateStringValue(it.source));
    o.SetNamedValue(L"width", JsonValue::CreateNumberValue(it.width));
    o.SetNamedValue(L"height", JsonValue::CreateNumberValue(it.height));
    if (it.durationSec > 0) o.SetNamedValue(L"dur", JsonValue::CreateNumberValue(it.durationSec));
    o.SetNamedValue(L"createdAt", JsonValue::CreateNumberValue(static_cast<double>(it.createdAt)));
    return o;
}
static Setting::HistoryItem historyItemFromJson(const JsonObject& o)
{
    Setting::HistoryItem it;
    it.id = o.GetNamedString(L"id", L"").c_str();
    it.filePath = o.GetNamedString(L"filePath", L"").c_str();
    it.source = o.GetNamedString(L"source", L"capture").c_str();
    it.width = static_cast<int>(o.GetNamedNumber(L"width", 0));
    it.height = static_cast<int>(o.GetNamedNumber(L"height", 0));
    it.durationSec = static_cast<int>(o.GetNamedNumber(L"dur", 0));
    it.createdAt = static_cast<long long>(o.GetNamedNumber(L"createdAt", 0));
    return it;
}

std::vector<Setting::HistoryItem> Setting::getHistoryItems()
{
    auto features = featuresObj();
    auto store = historyStore(features);
    JsonArray arr{};
    if (store.HasKey(L"items") && store.GetNamedValue(L"items").ValueType() == JsonValueType::Array)
        arr = store.GetNamedArray(L"items");
    std::vector<HistoryItem> list;
    for (auto const& v : arr) {
        if (v.ValueType() != JsonValueType::Object) continue;
        auto it = historyItemFromJson(v.GetObjectW());
        // 文件被用户在资源管理器里删掉/移走后不再显示（索引保留，盘符恢复后还能回来）
        std::error_code ec;
        if (it.filePath.empty() || !std::filesystem::exists(it.filePath, ec)) continue;
        list.push_back(it);
    }
    // 按创建时间倒序 (新的在前)
    std::sort(list.begin(), list.end(), [](const HistoryItem& a, const HistoryItem& b) {
        return a.createdAt > b.createdAt;
    });
    return list;
}

namespace {
    bool historyExtMatch(const std::wstring& path, std::initializer_list<const wchar_t*> exts)
    {
        const auto dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring e = path.substr(dot + 1);
        for (auto& c : e) c = (wchar_t)towlower(c);
        for (auto x : exts) if (e == x) return true;
        return false;
    }
    std::wstring historyPathKey(const std::wstring& path)
    {
        std::wstring k = path;
        for (auto& c : k) c = (wchar_t)towlower(c);
        return k;
    }
    long long historyFileTimeMs(const std::filesystem::path& p)
    {
        std::error_code ec;
        const auto ft = std::filesystem::last_write_time(p, ec);
        if (ec) return 0;
        const auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() + (ft - std::filesystem::file_time_type::clock::now()));
        return (long long)sys.time_since_epoch().count();
    }
}

// 历史页用：索引 + 扫描「截图文件路径 / 录屏文件路径」里已有的媒体文件。
// 这些文件没走过登记流程（如旧版本、手动拷进来的）也照样显示，保证“可见即所得”。
std::vector<Setting::HistoryItem> Setting::getHistoryItemsForUi(bool forceRescan)
{
    const auto nowTick = GetTickCount64();
    if (!forceRescan && !historyUiCacheDirty_ && nowTick - historyUiCacheAt_ < 1500)
        return historyUiCache_;

    auto list = getHistoryItems();
    std::set<std::wstring> seen;
    for (const auto& it : list) seen.insert(historyPathKey(it.filePath));

    // 「删除只记录」隐藏起来的文件：仍在磁盘上，但不再出现在历史里
    std::set<std::wstring> hidden;
    for (auto const& v : featArray(L"hiddenHistory")) {
        if (v.ValueType() == JsonValueType::String)
            hidden.insert(historyPathKey(v.GetString().c_str()));
    }

    auto scanDir = [&](const std::filesystem::path& dir) {
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const auto path = it->path();
            const std::wstring ws = path.wstring();
            const bool video = historyExtMatch(ws, { L"mp4", L"gif", L"apng" });
            const bool image = historyExtMatch(ws, { L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"avif", L"jxl" });
            if (!video && !image) continue;
            const auto pathKey = historyPathKey(ws);
            if (hidden.count(pathKey)) continue;
            if (!seen.insert(pathKey).second) continue;
            HistoryItem item;
            item.id = ws;              // 目录里已有的文件没有索引 id，直接用路径
            item.filePath = ws;
            item.source = video ? L"record" : L"capture";
            item.createdAt = historyFileTimeMs(path);
            list.push_back(std::move(item));
        }
    };
    scanDir(Util::outputDir());
    if (const auto rec = getRecordDir(); !rec.empty()) scanDir(std::filesystem::path{ rec });

    // 文件名带「自动保存文件名格式」固定前缀的截图 → 标成自动保存。
    // 兼容旧版本：那时取消退出存下来的文件被登记成了 capture，现在按文件名纠正。
    std::wstring autoFmt = getAutoSaveFormat();
    if (autoFmt.empty()) autoFmt = Setting::defaultAutoSaveFormat();
    const auto brace = autoFmt.find(L"{{");
    std::wstring autoPrefix = brace == std::wstring::npos ? autoFmt : autoFmt.substr(0, brace);
    if (const auto slash = autoPrefix.find_last_of(L"/\\"); slash != std::wstring::npos)
        autoPrefix = autoPrefix.substr(slash + 1);
    const auto prefixKey = historyPathKey(autoPrefix);
    if (!prefixKey.empty()) {
        for (auto& it : list) {
            if (it.source != L"capture") continue;
            const auto name = historyPathKey(std::filesystem::path(it.filePath).filename().wstring());
            if (name.size() >= prefixKey.size() && name.compare(0, prefixKey.size(), prefixKey) == 0)
                it.source = L"autosave";
        }
    }

    std::sort(list.begin(), list.end(), [](const HistoryItem& a, const HistoryItem& b) {
        return a.createdAt > b.createdAt;
    });
    historyUiCache_ = list;
    historyUiCacheAt_ = nowTick;
    historyUiCacheDirty_ = false;
    return list;
}

// ================ 历史自定义标签（按文件路径） ================
namespace {
    JsonArray historyTagsArr(JsonObject& store)
    {
        if (store.HasKey(L"tags") && store.GetNamedValue(L"tags").ValueType() == JsonValueType::Array)
            return store.GetNamedArray(L"tags");
        return JsonArray{};
    }
}

std::vector<std::wstring> Setting::getHistoryTags(const std::wstring& filePath)
{
    std::vector<std::wstring> out;
    if (filePath.empty()) return out;
    auto features = featuresObj();
    auto store = historyStore(features);
    const auto key = historyPathKey(filePath);
    const auto all = historyTagsArr(store);
    if (!all) return out;
    for (auto const& v : all) {
        if (v.ValueType() != JsonValueType::Object) continue;
        auto o = v.GetObjectW();
        if (historyPathKey(o.GetNamedString(L"path", L"").c_str()) != key) continue;
        if (o.HasKey(L"tags") && o.GetNamedValue(L"tags").ValueType() == JsonValueType::Array) {
            for (auto const& t : o.GetNamedArray(L"tags")) {
                if (t.ValueType() != JsonValueType::String) continue;
                std::wstring s = t.GetString().c_str();
                if (!s.empty()) out.push_back(std::move(s));
            }
        }
        break;
    }
    return out;
}

void Setting::setHistoryTags(const std::wstring& filePath, const std::vector<std::wstring>& tags)
{
    if (filePath.empty()) return;
    auto features = featuresObj();
    auto store = historyStore(features);
    const auto key = historyPathKey(filePath);

    JsonArray arr;
    const auto old = historyTagsArr(store);
    if (old) {
        for (auto const& v : old) {
            if (v.ValueType() != JsonValueType::Object) continue;
            auto o = v.GetObjectW();
            if (historyPathKey(o.GetNamedString(L"path", L"").c_str()) == key) continue;   // 旧记录丢弃，稍后按新值重建
            arr.Append(v);
        }
    }
    if (!tags.empty()) {
        JsonObject o;
        o.SetNamedValue(L"path", JsonValue::CreateStringValue(filePath));
        JsonArray ts;
        for (auto& t : tags) ts.Append(JsonValue::CreateStringValue(t));
        o.SetNamedValue(L"tags", ts);
        arr.Append(o);
    }
    store.SetNamedValue(L"tags", arr);
    features.SetNamedValue(L"history", store);
    save();
    historyUiCacheDirty_ = true;
}

std::vector<std::wstring> Setting::getAllHistoryTags()
{
    std::set<std::wstring> uniq;
    auto features = featuresObj();
    auto store = historyStore(features);
    const auto all = historyTagsArr(store);
    if (!all) return {};
    for (auto const& v : all) {
        if (v.ValueType() != JsonValueType::Object) continue;
        auto o = v.GetObjectW();
        const std::wstring path = o.GetNamedString(L"path", L"").c_str();
        std::error_code ec;
        if (path.empty() || !std::filesystem::exists(path, ec)) continue;
        if (!o.HasKey(L"tags") || o.GetNamedValue(L"tags").ValueType() != JsonValueType::Array) continue;
        for (auto const& t : o.GetNamedArray(L"tags")) {
            if (t.ValueType() != JsonValueType::String) continue;
            std::wstring s = t.GetString().c_str();
            if (!s.empty()) uniq.insert(std::move(s));
        }
    }
    std::vector<std::wstring> out(uniq.begin(), uniq.end());
    return out;
}

// 登记一条已落盘的历史文件（截图 / 长图 / 静默截图 / 录屏）：
// 历史只记真实路径，不再复制一份图像。返回条目 id。
std::wstring Setting::addHistoryFile(const std::wstring& filePath, int width, int height,
    const std::wstring& source)
{
    if (filePath.empty()) return L"";
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) return L"";
    historyUiCacheDirty_ = true;

    static UINT64 seq = 0;
    const auto now = std::chrono::system_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    HistoryItem it;
    it.id = wfmt(L"h%lld-%d", (long long)nowMs, (int)++seq);
    it.filePath = filePath;
    it.source = source.empty() ? L"capture" : source;
    it.width = width;
    it.height = height;
    it.createdAt = (long long)nowMs;

    JsonArray arr;
    arr.Append(historyItemToJson(it));   // 新的排在前面（与 getHistoryItems 的排序一致）
    for (auto& old : getHistoryItems()) {
        if (old.filePath == filePath) continue;   // 同一文件只留一条
        arr.Append(historyItemToJson(old));
    }

    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
    PluginHost::instance().onCapture(filePath);
    return it.id;
}

std::wstring Setting::addHistoryMedia(const std::wstring& mediaPath, int width, int height,
    const std::wstring& source, int durationSec, const std::vector<BYTE>& coverBgra)
{
    if (mediaPath.empty()) return L"";
    historyUiCacheDirty_ = true;
    HistoryItem it;
    static UINT64 seq = 0;
    const auto now = std::chrono::system_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    it.id = wfmt(L"h%lld-%d", (long long)nowMs, (int)++seq);
    it.source = source.empty() ? L"record" : source;
    it.width = width;
    it.height = height;
    it.durationSec = durationSec > 0 ? durationSec : 0;
    it.createdAt = (long long)nowMs;
    it.filePath = mediaPath;          // 原视频路径：不复制，历史只记引用

    // 封面只是给网格用的缩略图，写进缓存目录；视频本体不复制
    if (!coverBgra.empty() && width > 0 && height > 0
        && (size_t)width * (size_t)height * 4 <= coverBgra.size()) {
        auto dir = getHistoryThumbDir();
        Util::saveToFile((dir / (it.id + L".png")).wstring(), width, height,
            const_cast<BYTE*>(coverBgra.data()));
    }

    JsonArray arr;
    arr.Append(historyItemToJson(it));   // 新的排在前面（与 getHistoryItems 的排序一致）
    for (auto& old : getHistoryItems()) {
        if (old.filePath == mediaPath) continue;   // 同一文件只留一条
        arr.Append(historyItemToJson(old));
    }

    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
    return it.id;
}

void Setting::pruneExpiredHistory()
{
    int days = getHistoryRetentionDays();
    if (days < 0) return; // -1 代表永久保留
    // 当前 ms 时间戳
    auto now = std::chrono::system_clock::now();
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    long long cutoff = nowMs - static_cast<long long>(days) * 24LL * 3600LL * 1000LL;

    historyUiCacheDirty_ = true;
    auto list = getHistoryItems();
    std::vector<HistoryItem> keep;
    for (auto& it : list) {
        // 超过保留天数的截图与录屏都会清理
        if (it.createdAt >= cutoff) {
            keep.push_back(it);
            continue;
        }
        std::error_code ec;
        if (!it.filePath.empty()) std::filesystem::remove(it.filePath, ec);
        std::filesystem::remove(getHistoryThumbDir() / (it.id + L".png"), ec);
    }
    // 写回
    JsonArray arr;
    for (auto& it : keep) arr.Append(historyItemToJson(it));
    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
}

void Setting::removeHistoryItem(const std::wstring& id, bool deleteFile)
{
    historyUiCacheDirty_ = true;
    auto list = getHistoryItems();
    std::vector<HistoryItem> keep;
    bool found = false;
    std::wstring targetPath = id;
    for (auto& it : list) {
        if (it.id == id) {
            found = true;
            targetPath = it.filePath;
            // 历史即真实文件：deleteFile=true 时删条目就删文件（走回收站，避免误删）
            std::error_code ec;
            if (deleteFile && !it.filePath.empty()) Util::moveToRecycleBin(it.filePath);
            std::filesystem::remove(getHistoryThumbDir() / (it.id + L".png"), ec);
        } else {
            keep.push_back(it);
        }
    }
    // 目录扫描出来的条目没有索引 id（id 就是文件路径）：deleteFile=true 时直接删文件
    if (!found && deleteFile) {
        std::error_code ec;
        if (std::filesystem::exists(id, ec)) Util::moveToRecycleBin(id);
    }
    // 只删记录：文件原地保留，但目录扫描会再次把它扫出来，所以路径要记进隐藏列表
    if (!deleteFile && !targetPath.empty()) {
        auto hiddenArr = featArray(L"hiddenHistory");
        const auto key = historyPathKey(targetPath);
        bool exists = false;
        for (auto const& v : hiddenArr) {
            if (v.ValueType() != JsonValueType::String) continue;
            if (historyPathKey(v.GetString().c_str()) == key) { exists = true; break; }
        }
        if (!exists) {
            hiddenArr.Append(JsonValue::CreateStringValue(targetPath));
            setFeatArray(L"hiddenHistory", hiddenArr);
        }
    }
    JsonArray arr;
    for (auto& it : keep) arr.Append(historyItemToJson(it));
    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
}

void Setting::clearAllHistory(int sourceTab)
{
    historyUiCacheDirty_ = true;
    // 只清「本软件登记过」的历史文件：索引条目的 id 是 h<时间>-<序号>，
    // 目录扫描出来的外部文件 id 就是路径本身，这类文件不删除、也不影响登记表。
    auto list = getHistoryItemsForUi(true);
    for (auto& it : list) {
        if (it.id == it.filePath) continue;   // 目录里用户自己放的文件，跳过
        const bool isRecord = (it.source == L"record");
        if (sourceTab == 1 && isRecord) continue;   // 只清截图
        if (sourceTab == 2 && !isRecord) continue;  // 只清录屏
        std::error_code ec;
        if (!it.filePath.empty()) Util::moveToRecycleBin(it.filePath);
        std::filesystem::remove(getHistoryThumbDir() / (it.id + L".png"), ec);
    }
    // 索引里保留没被清掉的分类
    JsonArray arr;
    for (auto& it : getHistoryItems()) {
        const bool isRecord = (it.source == L"record");
        if ((sourceTab == 1 && isRecord) || (sourceTab == 2 && !isRecord))
            arr.Append(historyItemToJson(it));
    }
    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
}

// 「其他」：目录扫描出来的条目（id 就是文件路径）是用户自己放进来的文件。
// 这里只清这些文件（走回收站），软件登记过的截图/录屏不动。
void Setting::clearOtherFiles()
{
    historyUiCacheDirty_ = true;
    auto list = getHistoryItemsForUi(true);
    for (auto& it : list) {
        if (it.id != it.filePath) continue;
        Util::moveToRecycleBin(it.filePath);
    }
    historyUiCacheDirty_ = true;
}

// ================= 剪切板历史 =================
// 不限条数：文本直接进索引，图片落盘到 dataPath\Clipboard（历史页用缩略图展示）。
namespace {
    JsonObject clipboardStore(JsonObject& features)
    {
        if (!features.HasKey(L"clipboard") || features.GetNamedValue(L"clipboard").ValueType() != JsonValueType::Object)
            features.SetNamedValue(L"clipboard", JsonObject{});
        return features.GetNamedObject(L"clipboard");
    }
    JsonArray clipboardArr(JsonObject& store)
    {
        if (store.HasKey(L"items") && store.GetNamedValue(L"items").ValueType() == JsonValueType::Array)
            return store.GetNamedArray(L"items");
        return JsonArray{};
    }
    unsigned clipboardHash(const std::vector<BYTE>& data)
    {
        unsigned h = 2166136261u;
        for (BYTE b : data) { h ^= b; h *= 16777619u; }
        return h;
    }
}

std::filesystem::path Setting::getClipboardDir()
{
    auto dir = dataPath;
    dir.append(L"Clipboard");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) std::filesystem::create_directories(dir, ec);
    return dir;
}

std::vector<Setting::ClipboardItem> Setting::getClipboardItems()
{
    std::vector<ClipboardItem> out;
    auto features = featuresObj();
    auto store = clipboardStore(features);
    const auto arr = clipboardArr(store);
    if (!arr) return out;
    for (auto const& v : arr) {
        if (v.ValueType() != JsonValueType::Object) continue;
        auto o = v.GetObjectW();
        ClipboardItem it;
        it.id = o.GetNamedString(L"id", L"").c_str();
        it.type = o.GetNamedString(L"type", L"text").c_str();
        it.text = o.GetNamedString(L"text", L"").c_str();
        it.filePath = o.GetNamedString(L"filePath", L"").c_str();
        it.width = (int)o.GetNamedNumber(L"width", 0);
        it.height = (int)o.GetNamedNumber(L"height", 0);
        it.createdAt = (long long)o.GetNamedNumber(L"createdAt", 0);
        it.hash = (unsigned)o.GetNamedNumber(L"hash", 0);
        if (o.HasKey(L"tags") && o.GetNamedValue(L"tags").ValueType() == JsonValueType::Array) {
            for (auto const& t : o.GetNamedArray(L"tags")) {
                if (t.ValueType() == JsonValueType::String) {
                    std::wstring s = t.GetString().c_str();
                    if (!s.empty()) it.tags.push_back(std::move(s));
                }
            }
        }
        if (o.HasKey(L"files") && o.GetNamedValue(L"files").ValueType() == JsonValueType::Array) {
            for (auto const& f : o.GetNamedArray(L"files")) {
                if (f.ValueType() == JsonValueType::String) {
                    std::wstring s = f.GetString().c_str();
                    if (!s.empty()) it.files.push_back(std::move(s));
                }
            }
        }
        if (it.type == L"image") {
            std::error_code ec;
            if (it.filePath.empty() || !std::filesystem::exists(it.filePath, ec)) continue;
        }
        if (it.type == L"files" && it.files.empty()) continue;
        if (it.type == L"text" && it.text.empty()) continue;
        out.push_back(std::move(it));
    }
    std::sort(out.begin(), out.end(), [](const ClipboardItem& a, const ClipboardItem& b) {
        return a.createdAt > b.createdAt;
    });
    return out;
}

std::vector<std::wstring> Setting::getClipboardTags(const std::wstring& id)
{
    if (id.empty()) return {};
    for (const auto& it : getClipboardItems()) {
        if (it.id == id) return it.tags;
    }
    return {};
}

void Setting::setClipboardTags(const std::wstring& id, const std::vector<std::wstring>& tags)
{
    if (id.empty()) return;
    auto items = getClipboardItems();
    for (auto& it : items) {
        if (it.id != id) continue;
        it.tags = tags;
        writeClipboardItems(items);
        return;
    }
}

// 写回索引：新条目在前（不做条数截断，历史由用户手动清理）
void Setting::writeClipboardItems(std::vector<ClipboardItem>& items)
{
    JsonArray arr;
    for (const auto& it : items) {
        JsonObject o;
        o.SetNamedValue(L"id", JsonValue::CreateStringValue(it.id));
        o.SetNamedValue(L"type", JsonValue::CreateStringValue(it.type));
        o.SetNamedValue(L"text", JsonValue::CreateStringValue(it.text));
        o.SetNamedValue(L"filePath", JsonValue::CreateStringValue(it.filePath));
        o.SetNamedValue(L"width", JsonValue::CreateNumberValue((double)it.width));
        o.SetNamedValue(L"height", JsonValue::CreateNumberValue((double)it.height));
        o.SetNamedValue(L"createdAt", JsonValue::CreateNumberValue((double)it.createdAt));
        o.SetNamedValue(L"hash", JsonValue::CreateNumberValue((double)it.hash));
        JsonArray ts;
        for (const auto& t : it.tags) ts.Append(JsonValue::CreateStringValue(t));
        o.SetNamedValue(L"tags", ts);
        JsonArray fs;
        for (const auto& f : it.files) fs.Append(JsonValue::CreateStringValue(f));
        o.SetNamedValue(L"files", fs);
        arr.Append(o);
    }
    auto features = featuresObj();
    auto store = clipboardStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"clipboard", store);
    save();
    if (clipboardChanged) clipboardChanged();
}

namespace {
    // 剪贴板内容去重：同一内容已经在历史里，就把它挪到最前并刷新时间（保留原 id 与标签），
    // 而不是再加一条。返回 -1 = 没有同内容；0 = 本来就在最前（不用动）；1 = 已挪到最前（需写回）
    template <typename Fn>
    int promoteClipboardItem(std::vector<Setting::ClipboardItem>& items, long long nowMs, Fn match)
    {
        for (size_t i = 0; i < items.size(); ++i) {
            if (!match(items[i])) continue;
            if (i == 0) return 0;
            auto hit = std::move(items[i]);
            items.erase(items.begin() + i);
            hit.createdAt = nowMs;
            items.insert(items.begin(), std::move(hit));
            return 1;
        }
        return -1;
    }
}

void Setting::addClipboardText(const std::wstring& text, bool dedup)
{
    std::wstring s = text;
    // 去掉末尾的 \0 / 空白换行，避免把同一个内容记成不同条目
    while (!s.empty() && (s.back() == L'\0' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    if (s.empty()) return;
    auto items = getClipboardItems();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 同内容只把它移到最前（复制历史里的文字回来也会走到这里）
    if (dedup) {
        const int dup = promoteClipboardItem(items, (long long)nowMs, [&](const ClipboardItem& it) {
            return it.type == L"text" && it.text == s;
        });
        if (dup >= 0) {
            if (dup > 0) writeClipboardItems(items);
            return;
        }
    }

    static unsigned long long seq = 0;
    ClipboardItem it;
    it.id = wfmt(L"c%lld-%d", (long long)nowMs, (int)++seq);
    it.type = L"text";
    it.text = std::move(s);
    it.createdAt = (long long)nowMs;
    items.insert(items.begin(), std::move(it));
    writeClipboardItems(items);
}

bool Setting::setClipboardText(const std::wstring& id, const std::wstring& text)
{
    if (id.empty()) return false;
    std::wstring s = text;
    while (!s.empty() && (s.back() == L'\0' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    auto items = getClipboardItems();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id != id) continue;
        if (items[i].type != L"text") return false;
        auto hit = std::move(items[i]);
        hit.text = std::move(s);
        hit.createdAt = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        items.erase(items.begin() + (std::ptrdiff_t)i);
        items.insert(items.begin(), std::move(hit));
        writeClipboardItems(items);
        return true;
    }
    return false;
}

void Setting::addClipboardImage(int w, int h, const std::vector<BYTE>& bgra)
{
    if (w <= 0 || h <= 0 || bgra.size() < (size_t)w * 4u * h) return;
    const unsigned hash = clipboardHash(bgra);
    auto items = getClipboardItems();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 同一张图只挪到最前，不重复存一份 PNG
    const int dup = promoteClipboardItem(items, (long long)nowMs, [&](const ClipboardItem& it) {
        return it.type == L"image" && it.hash == hash && it.width == w && it.height == h;
    });
    if (dup >= 0) {
        if (dup > 0) writeClipboardItems(items);
        return;
    }

    static unsigned long long seq = 0;
    ClipboardItem it;
    it.id = wfmt(L"c%lld-%d", (long long)nowMs, (int)++seq);
    it.type = L"image";
    it.filePath = (getClipboardDir() / (it.id + L".png")).wstring();
    it.width = w;
    it.height = h;
    it.createdAt = (long long)nowMs;
    it.hash = hash;
    if (it.filePath.empty()) return;
    std::vector<BYTE> copy = bgra;   // saveToFile 要非 const 指针
    if (!Util::saveToFile(it.filePath, w, h, copy.data())) return;
    items.insert(items.begin(), std::move(it));
    writeClipboardItems(items);
}

void Setting::addClipboardFiles(const std::vector<std::wstring>& files)
{
    if (files.empty()) return;
    // 去重用的签名：路径按小写拼接 + FNV-1a
    std::wstring sig;
    for (const auto& f : files) {
        for (wchar_t c : f) sig.push_back((wchar_t)towlower(c));
        sig.push_back(L'\n');
    }
    unsigned hash = 2166136261u;
    for (wchar_t c : sig) {
        hash ^= (unsigned)c;
        hash *= 16777619u;
    }
    auto items = getClipboardItems();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 同一组文件只挪到最前，不再加一条
    const int dup = promoteClipboardItem(items, (long long)nowMs, [&](const ClipboardItem& it) {
        return it.type == L"files" && it.hash == hash && it.files == files;
    });
    if (dup >= 0) {
        if (dup > 0) writeClipboardItems(items);
        return;
    }

    static unsigned long long seq = 0;
    ClipboardItem it;
    it.id = wfmt(L"c%lld-%d", (long long)nowMs, (int)++seq);
    it.type = L"files";
    it.files = files;
    it.createdAt = (long long)nowMs;
    it.hash = hash;
    items.insert(items.begin(), std::move(it));
    writeClipboardItems(items);
}

void Setting::removeClipboardItem(const std::wstring& id)
{
    auto items = getClipboardItems();
    std::vector<ClipboardItem> keep;
    for (auto& it : items) {
        if (it.id == id) {
            if (it.type == L"image") {
                std::error_code ec;
                std::filesystem::remove(it.filePath, ec);
            }
            continue;
        }
        keep.push_back(std::move(it));
    }
    writeClipboardItems(keep);
}

bool Setting::touchClipboardItem(const std::wstring& id)
{
    auto items = getClipboardItems();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id != id) continue;
        if (i == 0) return false;   // 已经在最前面
        ClipboardItem hit = std::move(items[i]);
        items.erase(items.begin() + (std::ptrdiff_t)i);
        hit.createdAt = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        items.insert(items.begin(), std::move(hit));
        writeClipboardItems(items);
        return true;
    }
    return false;
}

void Setting::clearClipboardHistory()
{
    auto items = getClipboardItems();
    for (auto& it : items) {
        if (it.type != L"image") continue;
        std::error_code ec;
        std::filesystem::remove(it.filePath, ec);
    }
    std::vector<ClipboardItem> empty;
    writeClipboardItems(empty);
}

void Setting::markOwnClipboardWrite()
{
    // 监听端靠序列号判断「有没有新内容」：自己的写入直接把序列号记下，
    // 随后的 WM_CLIPBOARDUPDATE / 定时轮询就会跳过，不会在剪切板历史里再生一条
    g_lastClipSeq = GetClipboardSequenceNumber();
}


