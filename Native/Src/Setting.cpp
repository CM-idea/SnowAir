#include "pch.h"
#include <include/Ling.h>
#include <wincodec.h>
#include <ole2.h>
#include <chrono>
#include <fstream>
#include <mutex>
#include <atomic>
// 快速贴屏要读「资源管理器当前选中项」（Shell COM）与「前台窗口里拖选的文字」（UIA）
#include <servprov.h>
#include <shlguid.h>
#include <exdisp.h>
// 顺序有讲究：oleacc.h 必须在 UIAutomation*.h 之前，否则 ITextProvider 会重复定义
#include <oleacc.h>
#include <UIAutomationClient.h>
#include "Setting.h"
#include "ConfigMerge.h"
#include "Plugin/PluginHost.h"
#include "Util.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinPin.h"
#include "Win/ClipPin.h"
#include "Win/HotkeyHook.h"
#include "Win/WinSetting.h"
#include "Win/WinImageEdit.h"
#include "App.h"
#include <algorithm>
#include <set>
#include <cwctype>
#include <sddl.h>   // ConvertSidToStringSidW

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
        { L"make_collage",           113 },
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
    // 兜底模式下的自愈周期：只要还有热键落在钩子上，就按这个间隔重走一遍
    // 「注册 + 重装钩子」。为什么不试几次就停：钩子会被系统静默摘掉（回调超时）、
    // 会被别的程序后装的钩子抢在前面，一旦放弃重试，那几条热键就一直哑着，
    // 只能靠重启或开关一次截图会话救回来。
    constexpr UINT kHotkeyRetryMs{ 30000 };
    // 配置文件的默认内容。空文件、坏 JSON、缺键都拿它兜底，所以这里列出的每一项
    // 都是代码里会直接按名字取的（见 getLang / getAutoStart）。
    // 全局热键**不在这里**给默认值 —— 键位统一由 getShortcutKey 的内置默认表决定（defaultShortcutOf）
    constexpr std::wstring_view defaultConfig{ LR"""({"common":{"autoStart":true,"runAsAdmin":false,"language":"zh-CN","updateCheckFreq":0}})""" };

    // System32 下的系统工具全路径（schtasks 等）。取不到时只给文件名，让系统按 PATH 找。
    std::wstring systemTool(const wchar_t* name)
    {
        wchar_t dir[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return name;
        std::wstring p{ dir };
        if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
        p += name;
        return p;
    }

    // 静默跑一个程序（不弹控制台、不经过 cmd.exe），返回退出码；拉不起来返回 -1。
    // 为什么不拼 "cmd.exe /c ..."：cmd 会对命令行做第二次解析（引号、%、&、^ 都是它的元字符），
    // 它再把自己的理解拼给子进程 —— 我们精心转义过的引号会被它吃掉或改写。
    // 这里 exe 与参数分开传、lpApplicationName 显式给全路径：全程只有 CreateProcessW 一次解析，
    // 路径里的空格 / & / % 都可以原样放进去。
    int runHidden(const std::wstring& exePath, const std::wstring& args)
    {
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring full = exePath + L" " + args;
        std::vector<wchar_t> buf(full.begin(), full.end());
        buf.push_back(0);
        if (!CreateProcessW(exePath.c_str(), buf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return -1;
        }
        // schtasks 正常是秒级返回；真卡住也别把 UI 线程一直吊着
        const DWORD wait = WaitForSingleObject(pi.hProcess, 15000);
        DWORD code = 0;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return wait == WAIT_OBJECT_0 ? static_cast<int>(code) : -2;
    }

    // 当前登录用户的 SID 字符串。计划任务的 Principal / LogonTrigger 都按 SID 绑用户，
    // 这样任务只在该用户登录时触发。取不到返回空串。
    std::wstring currentUserSid()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buf(size);
        std::wstring sid;
        if (size != 0 && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
            LPWSTR text = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
                sid = text;
                LocalFree(text);
            }
        }
        CloseHandle(token);
        return sid;
    }

    std::wstring xmlEscape(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size() + 8);
        for (const wchar_t c : s) {
            switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default: out += c; break;
            }
        }
        return out;
    }

    // 注册「登录时以最高权限启动」的计划任务（同名任务会先被 /F 覆盖）。成功返回 true。
    //
    // 用 XML 注册，而不是拼 schtasks 的 /TR 命令行 —— 后者有两个坑：
    //   1) /TR 里要放的是一条「将来要执行的命令行」，路径带空格就得加引号；这层引号还要穿过
    //      cmd / CreateProcess / schtasks 三层解析，少转义一层就静默登记失败（"C:\Program Files"、
    //      带空格的中文目录全中招，开关看着是开的，重启后什么也没发生）。
    //      写进 <Command> 则完全不过命令行，空格 / & / % 原样保留。
    //   2) 命令行开关关不掉任务的两个默认设置，而它们正是「自启时灵时不灵」的根源：
    //        DisallowStartIfOnBatteries 默认开 —— 笔记本不插电时开机根本不启动；
    //        ExecutionTimeLimit 默认 PT72H —— 托盘常驻满三天被计划任务直接杀掉。
    bool registerLogonTask(const std::wstring& exePath, const std::wstring& schtasksExe)
    {
        const std::wstring sid = currentUserSid();
        if (sid.empty()) return false;

        std::wstring xml{ LR"XML(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>SnowAir 开机自启</Description>
    <URI>\SnowAir</URI>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
      <UserId>@SID@</UserId>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>@SID@</UserId>
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>false</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>@EXE@</Command>
      <Arguments>--auto-start</Arguments>
    </Exec>
  </Actions>
</Task>
)XML" };
        const std::wstring esid = xmlEscape(sid);
        const std::wstring eexe = xmlEscape(exePath);
        for (size_t pos = xml.find(L"@SID@"); pos != std::wstring::npos; pos = xml.find(L"@SID@", pos + esid.size())) {
            xml.replace(pos, 5, esid);
        }
        if (const size_t pos = xml.find(L"@EXE@"); pos != std::wstring::npos) xml.replace(pos, 5, eexe);

        wchar_t tmpDir[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, tmpDir) == 0) return false;
        wchar_t tmpFile[MAX_PATH]{};
        if (GetTempFileNameW(tmpDir, L"snow", 0, tmpFile) == 0) return false;
        // schtasks /XML 只认 Unicode 文件：UTF-16LE + BOM
        HANDLE h = CreateFileW(tmpFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(xml.size() * sizeof(wchar_t));
        const bool ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom)
            && WriteFile(h, xml.data(), bytes, &written, nullptr) && written == bytes;
        CloseHandle(h);
        if (!ok) {
            DeleteFileW(tmpFile);
            return false;
        }
        const int rc = runHidden(schtasksExe,
            L"/Create /TN \"SnowAir\" /XML \"" + std::wstring(tmpFile) + L"\" /F");
        DeleteFileW(tmpFile);
        return rc == 0;
    }
}


// —— config.json 的读取与落盘 ——
namespace {
    // 旧配置的 features 可能不是对象；GetNamedObject 遇到错误类型会崩溃。
    JsonObject getFeaturesObjectOrNull(const JsonObject& root)
    {
        if (!root || !root.HasKey(L"features")) return JsonObject{ nullptr };
        if (root.GetNamedValue(L"features").ValueType() != JsonValueType::Object)
            return JsonObject{ nullptr };
        return root.GetNamedObject(L"features");
    }

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

    // 启动读取配置时重试若干次：OneDrive 下载或杀毒软件扫描可能导致首次打开失败。
    // 若立即判定为读取失败，本次会话将进入只读状态，后续修改无法保存。
    bool readConfigContentRetry(const std::filesystem::path& path, std::wstring& out)
    {
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (readConfigContent(path, out)) return true;
            Sleep(80);
        }
        return false;
    }

    bool isOwnProcessForeground()
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    // 原子写：先写同目录临时文件，成功后再整体替换目标。
    // 旧实现 CreateFile(CREATE_ALWAYS) 直接截断目标文件，写到一半崩溃 / 被杀就留下半截 JSON，
    // 下次启动解析失败 —— 用户看到的「配置文件读取错误」就是这么来的。
    bool writeConfigAtomic(const std::filesystem::path& dst, const std::wstring& text)
    {
        // 数据目录是「用到才建」的（见 initDataPath）：配置落到 %AppData%\SnowAir 时这里补建，
        // 免得便携模式之外的用户第一次保存因为目录不存在而写不进去。
        if (!dst.parent_path().empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dst.parent_path(), ec);
        }
        // 临时文件名包含进程 ID 和序号，避免多实例或多线程写入时共用同一临时文件。
        // 固定名称在同步盘上可能导致创建失败：上一次的临时文件仍被同步引擎占用。
        static std::atomic<unsigned long long> tmpSeq{ 0 };
        const std::wstring tmp = dst.wstring() + L"." + std::to_wstring(GetCurrentProcessId())
            + L"." + std::to_wstring(++tmpSeq) + L".tmp";
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
        // 目标文件可能被 OneDrive、杀毒软件或另一实例短暂占用，替换失败时重试若干次
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            const DWORD err = GetLastError();
            if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION
                && err != ERROR_LOCK_VIOLATION) break;
            Sleep(20);
        }
        DeleteFileW(tmp.c_str());
        return false;
    }

    std::filesystem::path siblingOf(const std::filesystem::path& p, const wchar_t* suffix)
    {
        return std::filesystem::path{ p.wstring() + suffix };
    }

    // 定义见本机数据（local.json）实现。剪贴板、历史与状态共用此递归锁，
    // 保证读-改-写串行执行。
    std::recursive_mutex& localDataMutex();
}


Setting::Setting() :dataPath{ initDataPath() }, configPath{ initConfigPath() }
{
    if (std::filesystem::exists(configPath)) {
        std::wstring content;
        if (!readConfigContentRetry(configPath, content)) {
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
            // 记录已保存内容，供 save() 跳过无变化的写入（后续启动清理步骤也会调用 save()）
            lastSavedConfig = content;
            // 留一份可用配置：下次真解析失败时用它恢复，而不是把设置清成默认值
            backupConfigFile();
            // 若 OneDrive 冲突副本比主文件新，采用较新版本，避免设置回退
            adoptNewerConflictCopy();
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
    // 若 .bak 内容与当前配置相同则跳过写入，减少同步盘上的无效上传与冲突
    std::wstring old;
    if (readConfigContent(siblingOf(configPath, L".bak"), old) && old == text) return;
    writeConfigAtomic(siblingOf(configPath, L".bak"), text);
}

// 同步盘在检测到同一文件被多台电脑修改时，会保留两份并将其中一份命名为
// config-电脑名.json 或 config (1).json。启动时若该副本比 config.json 新，则采用该副本
// 原配置已由 backupConfigFile() 保存到 .bak。
void Setting::adoptNewerConflictCopy()
{
    if (configPath.empty()) return;
    std::error_code ec;
    const auto mainTime = std::filesystem::last_write_time(configPath, ec);
    if (ec) return;

    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    for (const auto& entry : std::filesystem::directory_iterator(configPath.parent_path(), ec)) {
        if (ec) break;
        std::error_code fec;
        if (!entry.is_regular_file(fec)) continue;
        const auto path = entry.path();
        if (_wcsicmp(path.extension().c_str(), L".json") != 0) continue;
        const std::wstring name = path.filename().wstring();
        // 仅匹配同步盘或资源管理器生成的冲突副本命名，避免误用用户自建的 config-xxx.json
        const bool conflictName = name.rfind(L"config-", 0) == 0
            || name.rfind(L"config (", 0) == 0
            || name.rfind(L"config - ", 0) == 0;
        if (!conflictName) continue;
        const auto t = entry.last_write_time(fec);
        if (fec || t <= mainTime) continue;
        if (best.empty() || t > bestTime) { best = path; bestTime = t; }
    }
    if (best.empty()) return;

    std::wstring text;
    if (!readConfigContentRetry(best, text) || isBlankConfig(text)) return;
    JsonObject obj{ nullptr };
    if (!JsonObject::TryParse(text, obj)) return;
    // 仅接受包含 common 节的文件，避免将同目录其他 JSON 或不完整文件当作配置，导致设置被清空
    if (!obj.HasKey(L"common")) return;

    OutputDebugStringW((L"[SnowAir] adopted newer config conflict copy: " + best.wstring() + L"\n").c_str());
    configObj = obj;
    lastSavedConfig.clear();   // 清除已保存记录，确保采用的配置写回 config.json
    save();
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
    bool g_oleReady{ false };
    constexpr UINT_PTR kClipPollTimer{ 1 };
    constexpr UINT_PTR kClipDebounceTimer{ 2 };
    constexpr UINT kClipPollMs{ 1000 };
    // 剪贴板与输入法、设计软件共用：延迟读取，事件和轮询走同一条路径，
    // 不在通知或轮询里直接 OpenClipboard。
    constexpr UINT kClipDebounceMs{ 300 };

    // 在 UI 线程初始化 OLE，复用 Ling 已创建的 STA。
    bool ensureOle()
    {
        if (!g_oleReady) g_oleReady = SUCCEEDED(OleInitialize(nullptr));
        return g_oleReady;
    }

    // OLE 数据对象：读取期间不长期独占系统剪贴板。
    bool oleGetData(IDataObject* obj, CLIPFORMAT format, DWORD tymed, STGMEDIUM& stm)
    {
        FORMATETC fe{ format, nullptr, DVASPECT_CONTENT, -1, tymed };
        return obj && SUCCEEDED(obj->GetData(&fe, &stm));
    }

    // 部分源只提供 IStream；转成 HGLOBAL 后复用现有解析逻辑。
    bool streamToHGlobal(IStream* stream, HGLOBAL& out)
    {
        if (!stream) return false;
        std::vector<BYTE> bytes;
        BYTE buf[64 * 1024];
        for (;;) {
            ULONG read = 0;
            const HRESULT hr = stream->Read(buf, sizeof(buf), &read);
            if (read > 0) bytes.insert(bytes.end(), buf, buf + read);
            if (FAILED(hr) || read == 0) break;
        }
        if (bytes.empty()) return false;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!h) return false;
        if (void* p = GlobalLock(h)) {
            memcpy(p, bytes.data(), bytes.size());
            GlobalUnlock(h);
            out = h;
            return true;
        }
        GlobalFree(h);
        return false;
    }

    template <typename Fn>
    bool withHGlobal(IDataObject* obj, CLIPFORMAT format, Fn&& fn)
    {
        STGMEDIUM stm{};
        if (oleGetData(obj, format, TYMED_HGLOBAL, stm)) {
            const bool ok = fn(stm.hGlobal);
            ReleaseStgMedium(&stm);
            return ok;
        }
        STGMEDIUM streamStm{};
        if (!oleGetData(obj, format, TYMED_ISTREAM, streamStm)) return false;
        bool ok = false;
        if (HGLOBAL h = nullptr; streamToHGlobal(streamStm.pstm, h)) {
            ok = fn(h);
            GlobalFree(h);
        }
        ReleaseStgMedium(&streamStm);
        return ok;
    }

    struct ClipboardSnapshot
    {
        std::vector<std::wstring> files;
        std::wstring text;
        std::vector<BYTE> pixels;
        int width{ 0 }, height{ 0 };
        bool hasFiles{ false }, hasText{ false }, hasImage{ false };
    };

    // 通过 OLE 数据对象读取，避免在读取期间独占系统剪贴板。
    bool readClipboardOle(ClipboardSnapshot& out)
    {
        if (!ensureOle()) return false;
        Microsoft::WRL::ComPtr<IDataObject> obj;
        if (FAILED(OleGetClipboard(obj.GetAddressOf())) || !obj) return false;

        out.hasFiles = withHGlobal(obj.Get(), CF_HDROP, [&](HGLOBAL h) {
            HDROP drop = static_cast<HDROP>(h);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len, L'\0');
                if (DragQueryFileW(drop, i, path.data(), len + 1) == 0) continue;
                out.files.push_back(std::move(path));
            }
            return !out.files.empty();
        });

        if (!out.hasFiles) {
            out.hasText = withHGlobal(obj.Get(), CF_UNICODETEXT, [&](HGLOBAL h) {
                if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    out.text = p;
                    GlobalUnlock(h);
                }
                return !out.text.empty();
            });
            if (!out.hasText) {
                out.hasText = withHGlobal(obj.Get(), CF_TEXT, [&](HGLOBAL h) {
                    if (const auto* p = static_cast<const char*>(GlobalLock(h))) {
                        const int len = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
                        if (len > 1) {
                            out.text.resize((size_t)len - 1);
                            MultiByteToWideChar(CP_ACP, 0, p, -1, out.text.data(), len);
                        }
                        GlobalUnlock(h);
                    }
                    return !out.text.empty();
                });
            }
        }

        if (!out.hasFiles && !out.hasText) {
            static const UINT pngFmt = RegisterClipboardFormatW(L"PNG");
            if (pngFmt)
                out.hasImage = withHGlobal(obj.Get(), pngFmt,
                    [&](HGLOBAL h) { return pngToBgra(h, out.width, out.height, out.pixels); });
            if (!out.hasImage)
                out.hasImage = withHGlobal(obj.Get(), CF_DIBV5,
                    [&](HGLOBAL h) { return dibToBgra(h, out.width, out.height, out.pixels); });
            if (!out.hasImage)
                out.hasImage = withHGlobal(obj.Get(), CF_DIB,
                    [&](HGLOBAL h) { return dibToBgra(h, out.width, out.height, out.pixels); });
            if (!out.hasImage) {
                STGMEDIUM stm{};
                if (oleGetData(obj.Get(), CF_BITMAP, TYMED_GDI, stm)) {
                    out.hasImage = hbitmapToBgra(stm.hBitmap, out.width, out.height, out.pixels);
                    ReleaseStgMedium(&stm);
                }
            }
        }

        return out.hasFiles || out.hasText || out.hasImage;
    }

    // 采集流程：WM_CLIPBOARDUPDATE -> 300ms 去抖 -> 单次 OLE 读取 -> 序号校验 -> 写历史。
    // OLE 在 UI 线程执行，复用 Ling 已初始化的 STA。
    std::wstring clipboardSourceTitle()
    {
        auto titleOf = [](HWND hwnd) -> std::wstring {
            if (!hwnd) return {};
            HWND root = GetAncestor(hwnd, GA_ROOT);
            if (root) hwnd = root;
            const int len = GetWindowTextLengthW(hwnd);
            if (len <= 0) return {};
            std::wstring title(static_cast<size_t>(len) + 1, L'\0');
            const int got = GetWindowTextW(hwnd, title.data(), len + 1);
            title.resize(got > 0 ? static_cast<size_t>(got) : 0);
            return title;
        };
        std::wstring title = titleOf(GetClipboardOwner());
        if (title.empty()) title = titleOf(GetForegroundWindow());
        return title;
    }

    void captureClipboardNow()
    {
        auto* s = Setting::get();
        if (!s || !g_clipWnd) return;
        const DWORD seq = GetClipboardSequenceNumber();
        if (seq == g_lastClipSeq) return;

        ClipboardSnapshot snap;
        const bool read = readClipboardOle(snap);
        // 读取期间内容又变了：本次快照作废，交给下一次通知。
        if (GetClipboardSequenceNumber() != seq) return;
        g_lastClipSeq = seq;
        if (!read) return;

        // 来源窗口在剪切板黑名单里：这次复制不写入历史。
        if (s->isWindowBlacklisted(clipboardSourceTitle())) return;

        if (snap.hasFiles) s->addClipboardFiles(snap.files);
        else if (snap.hasText) s->addClipboardText(snap.text);
        else if (snap.hasImage) s->addClipboardImage(snap.width, snap.height, snap.pixels);
    }

    LRESULT CALLBACK clipboardWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CLIPBOARDUPDATE) {
            // 延迟读取，先让写入方完成粘贴；此处不直接 OpenClipboard。
            SetTimer(hwnd, kClipDebounceTimer, kClipDebounceMs, nullptr);
            return 0;
        }
        if (msg == WM_TIMER && wp == kClipDebounceTimer) {
            KillTimer(hwnd, kClipDebounceTimer);
            captureClipboardNow();
            return 0;
        }
        if (msg == WM_TIMER && wp == kClipPollTimer) {
            // 轮询只发现变化，读取仍走延迟路径。
            if (GetClipboardSequenceNumber() != g_lastClipSeq)
                SetTimer(hwnd, kClipDebounceTimer, kClipDebounceMs, nullptr);
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
            // 兜底轮询（1s）：WM_CLIPBOARDUPDATE 偶发收不到时也能接住
            SetTimer(g_clipWnd, kClipPollTimer, kClipPollMs, nullptr);
            // 启动时不读当前剪贴板，只记序列号
            g_lastClipSeq = GetClipboardSequenceNumber();
        }
    }

    void uninstallClipboardWatch()
    {
        if (g_clipWnd) {
            KillTimer(g_clipWnd, kClipPollTimer);
            KillTimer(g_clipWnd, kClipDebounceTimer);
            RemoveClipboardFormatListener(g_clipWnd);
            DestroyWindow(g_clipWnd);
            g_clipWnd = nullptr;
            UnregisterClassW(kClipWndClass, GetModuleHandleW(nullptr));
        }
        if (g_oleReady) {
            OleUninitialize();
            g_oleReady = false;
        }
    }
}

void Setting::init()
{
    auto ptr = new Setting();
    setting.reset(ptr);
    // 旧配置的顶层节可能不是对象；进入业务代码前统一归一化。
    setting->normalizeConfigSections();
    // 将旧版本分散在 config.json、clipboard.json、history.json、state.json 中的本机数据
    // 迁移到 local.json。此后本机数据只写入 local.json，用户设置只写入 config.json。
    setting->migrateLocalDataToFile();
    // 启动时清理旧版本遗留的 AI 配置（AI 对话 / AI 视觉 OCR / AI 翻译）
    setting->purgeRemovedAiSettings();
    // 启动时清理旧程序（ScreenCapture）留下的历史键位
    setting->purgeLegacyShortcutKeys();
    // 视频/录屏文件已经不在了：对应的封面缓存也不再保留
    setting->pruneOrphanThumbCache();
    // 旧数据里同内容的剪贴板条目收敛成一条
    setting->dedupeClipboardItems();
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
    if (obj && obj.HasKey(type)
        && obj.GetNamedValue(type).ValueType() == JsonValueType::String) {
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

// 全局热键「重复截图」的上次选区（屏幕坐标）保存在 local.json 的 state.lastCaptureRect。
// 该状态与具体电脑相关，不写入同步盘上的 config。
bool Setting::getLastCaptureRect(int& x, int& y, int& w, int& h)
{
    std::wstring v;
    auto data = loadLocalData();
    if (data.HasKey(L"state") && data.GetNamedValue(L"state").ValueType() == JsonValueType::Object) {
        auto state = data.GetNamedObject(L"state");
        if (state.HasKey(L"lastCaptureRect")
            && state.GetNamedValue(L"lastCaptureRect").ValueType() == JsonValueType::String)
            v = state.GetNamedString(L"lastCaptureRect", L"").c_str();
    }
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
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    auto data = loadLocalData();
    JsonObject state{ nullptr };
    if (data.HasKey(L"state") && data.GetNamedValue(L"state").ValueType() == JsonValueType::Object)
        state = data.GetNamedObject(L"state");
    if (!state) {
        state = JsonObject{};
        data.SetNamedValue(L"state", state);
    }
    state.SetNamedValue(L"lastCaptureRect", JsonValue::CreateStringValue(wfmt(L"%d,%d,%d,%d", x, y, w, h)));
    saveLocalData(data);
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
        auto features = getFeaturesObjectOrNull(configObj);
        if (features) {
            for (auto* key : kRemovedFeatKeys) {
                if (features.HasKey(key)) {
                    features.Remove(key);
                    changed = true;
                }
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
    const std::wstring schtasks = systemTool(L"schtasks.exe");
    runHidden(schtasks, L"/Delete /TN \"SnowAir\" /F");

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
            registerLogonTask(selfPath, schtasks);
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
    // 自启登记指向哪个 exe 属于本机状态，写入 local.json
    setLocalStateString(L"autoStartExe", autoStart ? selfPath : L"");
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
    const std::wstring recorded = getLocalStateString(L"autoStartExe");
    if (!recorded.empty() && _wcsicmp(recorded.c_str(), cur.c_str()) == 0) return;
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
    // 这里**只算路径、不建目录**：配置可以放在 exe 旁（便携模式），插件目录也可能被指到
    // 别处，此时用户根本没用到 %AppData%\SnowAir 里的东西，启动就凭空建一个空文件夹只是
    // 留下垃圾。谁真要往里写（配置、剪贴板历史、缩略图缓存、插件），谁自己按需创建。
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
    std::wstring str{ configObj.Stringify() };
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(saveMutex);
        // 内容未变化时不重复写入。设置页与启动清理中存在大量值未变的保存调用，
        // 在同步盘上会造成不必要的上传。
        if (str == lastSavedConfig) return;

        // config.json 可能已被其他电脑修改。写入前先读取磁盘内容，将本进程相对
        // lastSavedConfig 的改动合并到最新配置，避免直接覆盖对方修改并触发 OneDrive 冲突副本。
        std::error_code ec;
        const bool exists = std::filesystem::exists(configPath, ec);
        if (exists) {
            std::wstring disk;
            if (!readConfigContent(configPath, disk)) {
                // 读取失败时暂不写入，避免覆盖未知内容；下次保存时重试
                failed = true;
            }
            else if (disk != lastSavedConfig) {
                JsonObject diskObj{ nullptr };
                if (JsonObject::TryParse(disk, diskObj)) {
                    JsonObject baseObj{ nullptr };
                    if (!lastSavedConfig.empty()) JsonObject::TryParse(lastSavedConfig, baseObj);
                    if (!baseObj) baseObj = JsonObject{};
                    ConfigMerge::MergeObjects(diskObj, baseObj, configObj);
                    configObj = diskObj;   // 合并结果同时更新内存中的配置
                    str = std::wstring{ configObj.Stringify() };
                    if (str == disk) {     // 合并结果与磁盘一致，无需重复写入
                        lastSavedConfig = disk;
                        return;
                    }
                }
                else {
                    // 磁盘文件损坏（例如同步中断导致内容不完整）：先备份到 .bad，再写入当前配置
                    std::error_code cec;
                    std::filesystem::copy_file(configPath, siblingOf(configPath, L".bad"),
                        std::filesystem::copy_options::overwrite_existing, cec);
                }
            }
        }

        if (!failed) {
            if (writeConfigAtomic(configPath, str)) {
                lastSavedConfig = str;
                return;
            }
            failed = true;
        }
    }
    // 写入失败时需提示用户，避免误认为已保存。后台运行时不弹窗，
    // 仅在前台操作时提示一次。
    // 提示框必须在 saveMutex 之外：MessageBox 会处理消息，可能重入 save()。
    if (failed && !configSaveWarned && isOwnProcessForeground()) {
        configSaveWarned = true;
        MessageBox(nullptr,
            L"配置未能写入 config.json（可能被 OneDrive / 杀毒软件临时占用）。\n"
            L"本次修改可能没有保存，稍后再改任意设置会重新尝试。\n\n"
            L"Could not write config.json (it may be temporarily locked by OneDrive or antivirus).",
            L"SnowAir", MB_OK | MB_ICONWARNING);
    }
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
    if (root.HasKey(tool) && root.GetNamedValue(tool).ValueType() != JsonValueType::Object)
        root.SetNamedValue(tool, JsonObject{});
    auto obj = root.GetNamedObject(tool, nullptr);
    if (!obj) {
        obj = JsonObject();
        root.SetNamedValue(tool, obj);
    }
    return obj;
}

bool Setting::getToolFlag(const std::wstring& tool, const std::wstring& key, bool def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key) || obj.GetNamedValue(key).ValueType() != JsonValueType::Boolean) return def;
    return obj.GetNamedValue(key).GetBoolean();
}

void Setting::setToolFlag(const std::wstring& tool, const std::wstring& key, bool val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateBooleanValue(val));
    save();
}

float Setting::getToolNum(const std::wstring& tool, const std::wstring& key, float def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key) || obj.GetNamedValue(key).ValueType() != JsonValueType::Number) return def;
    return static_cast<float>(obj.GetNamedValue(key).GetNumber());
}

void Setting::setToolNum(const std::wstring& tool, const std::wstring& key, float val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(val));
    save();
}

UINT32 Setting::getToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key) || obj.GetNamedValue(key).ValueType() != JsonValueType::Number) return def;
    return static_cast<UINT32>(obj.GetNamedValue(key).GetNumber());
}

void Setting::setToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(static_cast<double>(val)));
    save();
}

std::wstring Setting::getToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key) || obj.GetNamedValue(key).ValueType() != JsonValueType::String) return def;
    return std::wstring{ obj.GetNamedValue(key).GetString().c_str() };
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
    return getLocalStateInt(L"updateCheckDay", 0);
}

void Setting::setUpdateCheckDay(long long day)
{
    setLocalStateInt(L"updateCheckDay", day);
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

void Setting::normalizeConfigSections()
{
    if (!configObj) configObj = JsonObject{};
    // 顶层节按对象读取；类型异常时统一修正，避免 GetNamedObject 崩溃。
    for (const wchar_t* key : { L"common", L"features", L"toolPin", L"shortcutKey" }) {
        if (configObj.HasKey(key)
            && configObj.GetNamedValue(key).ValueType() != JsonValueType::Object) {
            configObj.SetNamedValue(key, JsonObject{});
        }
    }
    auto ensureType = [](JsonObject& obj, const wchar_t* key, JsonValueType type) {
        if (obj.HasKey(key) && obj.GetNamedValue(key).ValueType() != type) obj.Remove(key);
    };
    if (auto common = configObj.GetNamedObject(L"common", nullptr)) {
        ensureType(common, L"autoStart", JsonValueType::Boolean);
        ensureType(common, L"runAsAdmin", JsonValueType::Boolean);
        ensureType(common, L"language", JsonValueType::String);
        ensureType(common, L"pluginDir", JsonValueType::String);
        ensureType(common, L"autoStartExe", JsonValueType::String);
        ensureType(common, L"updateCheckFreq", JsonValueType::Number);
        ensureType(common, L"updateCheckDay", JsonValueType::Number);
    }
    if (auto features = configObj.GetNamedObject(L"features", nullptr)) {
        ensureType(features, L"lastCaptureRect", JsonValueType::String);
        ensureType(features, L"trayIconStyle", JsonValueType::Number);
        ensureType(features, L"trayIconStyle2", JsonValueType::Number);
        ensureType(features, L"trayCustomColor", JsonValueType::Number);
        ensureType(features, L"pinBorderColor", JsonValueType::Number);
        ensureType(features, L"mouseHighlightColor", JsonValueType::Number);
    }
}

JsonObject Setting::featuresObj()
{
	if (!configObj) configObj = JsonObject{};
	auto features = getFeaturesObjectOrNull(configObj);
	if (!features) {
		configObj.SetNamedValue(L"features", JsonObject{});
		return configObj.GetNamedObject(L"features");
	}
	return features;
}

bool Setting::featBool(const wchar_t* key, bool def)
{
	auto f = featuresObj();
	if (!f.HasKey(key) || f.GetNamedValue(key).ValueType() != JsonValueType::Boolean) return def;
	return f.GetNamedValue(key).GetBoolean();
}
void Setting::setFeatBool(const wchar_t* key, bool v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateBooleanValue(v));
	save();
}
int Setting::featInt(const wchar_t* key, int def)
{
	auto f = featuresObj();
	if (!f.HasKey(key) || f.GetNamedValue(key).ValueType() != JsonValueType::Number) return def;
	return static_cast<int>(f.GetNamedValue(key).GetNumber());
}
void Setting::setFeatInt(const wchar_t* key, int v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateNumberValue(v));
	save();
}
std::wstring Setting::featStr(const wchar_t* key, const wchar_t* def)
{
	auto f = featuresObj();
	if (!f.HasKey(key) || f.GetNamedValue(key).ValueType() != JsonValueType::String) return def;
	return std::wstring{ f.GetNamedValue(key).GetString().c_str() };
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
std::wstring Setting::getTranslateProvider() { return featStr(L"translateProvider", L"youdao"); }
void Setting::setTranslateProvider(const std::wstring& v) { setFeatStr(L"translateProvider", v); }
std::wstring Setting::getTranslateSourceLang() { return featStr(L"translateSourceLang", L"auto"); }
void Setting::setTranslateSourceLang(const std::wstring& v) { setFeatStr(L"translateSourceLang", v); }
// 默认目标语言为英语（即默认中译英）
std::wstring Setting::getTranslateTargetLang() { return featStr(L"translateTargetLang", L"en"); }
void Setting::setTranslateTargetLang(const std::wstring& v) { setFeatStr(L"translateTargetLang", v); }
bool Setting::getTranslateDictMode() { return featBool(L"translateDictMode", false); }
bool Setting::getQuickTranslateNewSession() { return featBool(L"quickTranslateNewSession", false); }
void Setting::setQuickTranslateNewSession(bool on) { setFeatBool(L"quickTranslateNewSession", on); }
// 微软翻译语气（仅微软在线翻译用）：standard 标准 / informal 非正式 / formal 正式
std::wstring Setting::getTranslateMsTone() { return featStr(L"translateMsTone", L"standard"); }
void Setting::setTranslateMsTone(const std::wstring& v) { setFeatStr(L"translateMsTone", v); }
void Setting::setTranslateDictMode(bool on) { setFeatBool(L"translateDictMode", on); }
bool Setting::getFeatureTips() { return featBool(L"featureTips", true); }
void Setting::setFeatureTips(bool on) { setFeatBool(L"featureTips", on); }
bool Setting::getHotkeyTip() { return featBool(L"hotkeyTip", false); }
void Setting::setHotkeyTip(bool on) { setFeatBool(L"hotkeyTip", on); }

bool Setting::getFindWindowElements() { return featBool(L"findWindowElements", true); }
void Setting::setFindWindowElements(bool on) { setFeatBool(L"findWindowElements", on); }
int Setting::getTrayClickAction() { return featInt(L"trayClickAction", 0); }
void Setting::setTrayClickAction(int v) { setFeatInt(L"trayClickAction", v); }
bool Setting::getPinZoomAtCursor() { return featBool(L"pinZoomAtCursor", true); }
void Setting::setPinZoomAtCursor(bool on) { setFeatBool(L"pinZoomAtCursor", on); }
int Setting::getCaptureToolbarAlign() { return featInt(L"captureToolbarAlign", 0); }
void Setting::setCaptureToolbarAlign(int v) { setFeatInt(L"captureToolbarAlign", v < 0 ? 0 : (v > 2 ? 2 : v)); }
int Setting::getToolbarSize() { return featInt(L"toolbarSize", 1); }
void Setting::setToolbarSize(int v)
{
    setFeatInt(L"toolbarSize", v < 0 ? 0 : (v > 2 ? 2 : v));
    for (auto& listener : toolbarSizeListeners) {
        if (listener) listener();
    }
}

void Setting::addToolbarSizeListener(std::function<void()> listener)
{
    if (listener) toolbarSizeListeners.push_back(std::move(listener));
}
bool Setting::getSilentCapture() { return featBool(L"silentCapture", true); }
void Setting::setSilentCapture(bool on) { setFeatBool(L"silentCapture", on); }
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
namespace {
	JsonObject toolLastRoot(JsonObject& features)
	{
		if (!features.HasKey(L"toolLast")
			|| features.GetNamedValue(L"toolLast").ValueType() != JsonValueType::Object)
			features.SetNamedValue(L"toolLast", JsonObject{});
		return features.GetNamedObject(L"toolLast");
	}
}
int Setting::getToolLastInt(const std::wstring& toolId, const std::wstring& key, int def)
{
	if (toolId.empty() || key.empty()) return def;
	auto features = featuresObj();
	if (!features.HasKey(L"toolLast")
		|| features.GetNamedValue(L"toolLast").ValueType() != JsonValueType::Object)
		return def;
	auto last = features.GetNamedObject(L"toolLast");
	if (!last.HasKey(toolId)) return def;
	auto one = last.GetNamedObject(toolId, nullptr);
	if (!one || !one.HasKey(key)) return def;
	return (int)one.GetNamedNumber(key, (double)def);
}
void Setting::setToolLastInt(const std::wstring& toolId, const std::wstring& key, int value)
{
	if (toolId.empty() || key.empty()) return;
	auto features = featuresObj();
	auto last = toolLastRoot(features);
	JsonObject one = last.HasKey(toolId) ? last.GetNamedObject(toolId, JsonObject{}) : JsonObject{};
	one.SetNamedValue(key, JsonValue::CreateNumberValue((double)value));
	last.SetNamedValue(toolId, one);
	features.SetNamedValue(L"toolLast", last);
	save();
}
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
int Setting::getCaptureFinishAction()
{
	// 老配置只有布尔 copyAsFile：true = 以文件复制，false = 复制图片。
	// 迁移成多档时保持它们原来的行为，之后就以 captureFinishAction 为准。
	const int v = featInt(L"captureFinishAction", -1);
	if (v >= 0 && v <= 3) return v;
	return featBool(L"copyAsFile", false) ? 1 : 0;
}
void Setting::setCaptureFinishAction(int v) { setFeatInt(L"captureFinishAction", v < 0 ? 0 : (v > 3 ? 3 : v)); }
bool Setting::getCancelConfirmDialog() { return featBool(L"cancelConfirmDialog", false); }
void Setting::setCancelConfirmDialog(bool on) { setFeatBool(L"cancelConfirmDialog", on); }

void Setting::restoreFunctionScreenshotDefaults()
{
	setAutoStart(true);
	setRunAsAdmin(false);
	setFindWindowElements(true);
	setCancelConfirmDialog(false);
	setAutoSaveAfterCapture(false);
	setCaptureFinishAction(0);
	setSaveFormat(L"PNG (*.png)");
	setManualSaveFormat(L"");
	setAutoSaveFormat(L"");
	setVideoRecordFormat(L"");
	setTrayClickAction(0);
	setDemoDefaultTool(0);
	setDemoEdgeBehavior(0);
	setPinZoomAtCursor(true);
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
	setKeyOverlayPos(kKeyOverlayNone);
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
	setTranslateProvider(L"youdao");
	setTranslateDictMode(false);
	setTranslateMsTone(L"standard");
	setQuickTranslateNewSession(false);
}

// 其他功能：功能提示开、快捷键提示关、记忆工具样式关、悬停预览关、静默截图开
void Setting::restoreFunctionOtherDefaults()
{
	setFeatureTips(true);
	setHotkeyTip(false);
	setRememberToolStyle(false);
	setHoverPreview(false);
	setSilentCapture(true);
	setCaptureToolbarAlign(0);
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
		&& getCaptureFinishAction() == 0 && getSaveFormat() == L"PNG (*.png)"
		&& getManualSaveFormat().empty() && getAutoSaveFormat().empty()
		&& getVideoRecordFormat().empty() && getTrayClickAction() == 0
		&& getDemoDefaultTool() == 0 && getDemoEdgeBehavior() == 0
		&& getPinZoomAtCursor() && !getPinAutoOcr() && getPinAutoFit()
		&& getPinDoubleClickAction() == 0 && getPinOpacity() == 100;
}

bool Setting::functionRecordAtDefaults()
{
	return getVideoQuality() == 4 && getVideoFps() == 0 && getGifQuality() == 1
		&& getGifFps() == 1 && getGifFormat() == 0 && getRecordMic() == 0
		&& getVideoEncoder() == 0 && getEncodeSpeed() == 1 && getHwAccel()
		&& getHideToolbarInRecord() && getKeyOverlayPos() == kKeyOverlayNone && !getSelFollowMouse()
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
	return getTranslateProvider() == L"youdao" && !getTranslateDictMode()
		&& getTranslateMsTone() == L"standard" && !getQuickTranslateNewSession();
}

bool Setting::functionOtherAtDefaults()
{
	return getFeatureTips() && !getHotkeyTip()
		&& !getRememberToolStyle() && !getHoverPreview()
		&& getSilentCapture() && getCaptureToolbarAlign() == 0;
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
int Setting::getKeyOverlayPos()
{
	const int v = featInt(L"keyOverlayPos", -1);
	if (v >= kKeyOverlayLeft && v <= kKeyOverlayNone) return v;
	// 老配置只有布尔 showKeystrokes（开/关）：开过就按「视窗左侧」显示，没开过就是关闭
	return featBool(L"showKeystrokes", false) ? kKeyOverlayLeft : kKeyOverlayNone;
}
void Setting::setKeyOverlayPos(int pos)
{
	setFeatInt(L"keyOverlayPos", std::clamp(pos, kKeyOverlayLeft, kKeyOverlayNone));
}
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
	if (!f.HasKey(L"mouseHighlightColor")
		|| f.GetNamedValue(L"mouseHighlightColor").ValueType() != JsonValueType::Number)
		return 0x0FDC78FF;   // 默认品牌绿
	return static_cast<UINT32>(f.GetNamedValue(L"mouseHighlightColor").GetNumber());
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
    /* 快速贴屏上绳开关 + 插件在场时，快速贴屏整体交给晾衣架接管。
       图片仍走插件原有的 on_quick_hang；文字和文件走新的可选导出。 */
    bool rackQuickHangEnabled()
    {
        auto* s = Setting::get();
        return s && s->getRackFlag(L"rack.quick_pin_hang", false) &&
               PluginHost::instance().loaded();
    }

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
        if (rackQuickHangEnabled() &&
            PluginHost::instance().hangQuickPinPaths(paths)) {
            return;
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
        if (!selected.empty()) {
            if (rackQuickHangEnabled() &&
                PluginHost::instance().hangQuickPinText(selected)) {
                return;
            }
            ClipPin::showText(selected);
            return;
        }
        // ③ 默认：剪贴板内容
        auto payload = readClipboardForPin();
        if (!payload.files.empty()) { quickPinPaths(payload.files); return; }
        if (!payload.text.empty()) {
            if (rackQuickHangEnabled() &&
                PluginHost::instance().hangQuickPinText(payload.text)) {
                return;
            }
            ClipPin::showText(payload.text);
            return;
        }
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
        // ⚠ 「屏幕录制」这条全局热键只负责**打开录屏功能**（进录屏会话），不兼职
        // 「开始/暂停录制」。后者是「系统按键」里的 cmd_record_toggle，默认也是 F2，
        // 但用户改过键位后必须按新的生效 —— 两者语义不同，不能在这里按 id 兜底转派。
    }
    if (id == L"capture") { WinCap::init(); return; }
    if (id == L"capture_repeat") { if (auto* app = App::get()) app->captureLastRegionSilent(); return; }
    if (id == L"capture_fullscreen") { if (auto* app = App::get()) app->captureFullScreenSilent(); return; }
    if (id == L"capture_focused_window") { if (auto* app = App::get()) app->captureFocusedWindowSilent(); return; }
    if (id == L"fullscreen_canvas") { WinCap::beginDemoCanvas(); return; }
    if (id == L"record") { WinCap::beginRecordPick(); return; }
    if (id == L"show_main") { WinSetting::toggle(); return; }
    if (id == L"make_collage") { WinImageEdit::init(); return; }
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
    hotkeyRetryTimer = SetTimer(nullptr, kHotkeyRetryTimerId, kHotkeyRetryMs, &Setting::onHotkeyRetry);
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
    ++self->hotkeyRetryTick;
    // ⚠ **不设次数上限**。以前试满 1 分钟就停，于是：开机时某条键被别人占了一下 →
    // 掉到钩子上 → 对方放开了也没人再抢 → 钩子再被系统摘掉 / 被别的程序抢在前面 →
    // 这条热键就永久哑了（实测现象就是"按了几次没反应，托盘点一次截图之后又好了"）。
    // 现在只要还有键落在钩子上就每 30 秒重来一遍，这一遍同时干三件事：
    //   ① 再抢一次 RegisterHotKey —— 对方放开了就立刻回到干净路径，并把钩子摘掉；
    //   ② 重建钩子 —— 被系统摘掉 / 上次没装成功的钩子在这里被重新拉起来（自愈）；
    //   ③ 清掉过期钩子线程 —— 换代后没退干净的旧线程会自己卸钩退出（见 HotkeyHook）。
    // 全都回到 RegisterHotKey 时，applyHotkeys 里会顺手 stopHotkeyRetry()，定时器自己停。
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
    if (!f.HasKey(L"pinBorderColor")
        || f.GetNamedValue(L"pinBorderColor").ValueType() != JsonValueType::Number)
        return 0x34C759FF; // 默认主题绿
    return static_cast<UINT32>(f.GetNamedValue(L"pinBorderColor").GetNumber());
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
    if (f.HasKey(L"trayIconStyle2")
        && f.GetNamedValue(L"trayIconStyle2").ValueType() == JsonValueType::Number) {
        const int v = static_cast<int>(f.GetNamedValue(L"trayIconStyle2").GetNumber());
        return (v < 0 || v > 3) ? 1 : v;
    }
    // 首次读取：从旧键一次性迁移（旧：0 跟随系统 / 1 主题 / 2 暗色 / 3 亮色 / 4 自定义）
    int old = 1;
    if (f.HasKey(L"trayIconStyle")
        && f.GetNamedValue(L"trayIconStyle").ValueType() == JsonValueType::Number)
        old = static_cast<int>(f.GetNamedValue(L"trayIconStyle").GetNumber());
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
    if (!f.HasKey(L"trayCustomColor")
        || f.GetNamedValue(L"trayCustomColor").ValueType() != JsonValueType::Number)
        return 0x34C759FF;   // 默认主题绿
    return static_cast<UINT32>(f.GetNamedValue(L"trayCustomColor").GetNumber());
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

bool Setting::isWindowBlacklisted(const std::wstring& windowTitle)
{
    if (windowTitle.empty()) return false;
    std::wstring title = windowTitle;
    for (auto& c : title) c = static_cast<wchar_t>(towlower(c));
    for (auto& entry : getWindowBlacklist()) {
        if (entry.empty()) continue;
        std::wstring needle = entry;
        for (auto& c : needle) c = static_cast<wchar_t>(towlower(c));
        if (title.find(needle) != std::wstring::npos) return true;
    }
    return false;
}

// ================ 本机数据：local.json ================
// 程序使用两个数据文件：
//   config.json = 用户设置（可能位于 OneDrive 或便携目录）
//   local.json  = 本机数据（剪贴板历史、截图/录屏历史、上次选区）
// 本机数据包含仅对本机有效的绝对路径和剪贴板内容，不随设置同步；
// 写入仅发生在本地磁盘，不会在 OneDrive 上产生同步冲突。
namespace {
std::recursive_mutex& localDataMutex()
{
    static std::recursive_mutex m;
    return m;
}

std::vector<std::wstring> splitLocalKey(const std::wstring& key)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        const size_t dot = key.find(L'.', start);
        const std::wstring part = key.substr(start, dot == std::wstring::npos ? std::wstring::npos : dot - start);
        if (!part.empty()) parts.push_back(part);
        if (dot == std::wstring::npos) break;
        start = dot + 1;
    }
    return parts;
}
}

std::filesystem::path Setting::getLocalDataPath()
{
    auto dir = dataPath;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) std::filesystem::create_directories(dir, ec);
    dir.append(L"local.json");
    return dir;
}

JsonObject Setting::loadLocalData()
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    const auto path = getLocalDataPath();
    std::wstring text;
    if (!readConfigContent(path, text) || isBlankConfig(text)) return JsonObject{};
    JsonObject obj{ nullptr };
    if (JsonObject::TryParse(text, obj)) return obj;
    // 文件损坏：备份为 .bad，本次按空数据读取；后续写入会重建文件
    std::error_code ec;
    std::filesystem::copy_file(path, siblingOf(path, L".bad"),
        std::filesystem::copy_options::overwrite_existing, ec);
    return JsonObject{};
}

bool Setting::saveLocalData(const JsonObject& data)
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    return writeConfigAtomic(getLocalDataPath(), std::wstring{ data.Stringify() });
}

std::wstring Setting::getLocalStateString(const wchar_t* key)
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    auto data = loadLocalData();
    if (!data.HasKey(L"state") || data.GetNamedValue(L"state").ValueType() != JsonValueType::Object)
        return {};
    auto state = data.GetNamedObject(L"state");
    if (!state.HasKey(key) || state.GetNamedValue(key).ValueType() != JsonValueType::String)
        return {};
    return std::wstring{ state.GetNamedString(key, L"") };
}

void Setting::setLocalStateString(const wchar_t* key, const std::wstring& value)
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    auto data = loadLocalData();
    JsonObject state{ nullptr };
    if (data.HasKey(L"state") && data.GetNamedValue(L"state").ValueType() == JsonValueType::Object)
        state = data.GetNamedObject(L"state");
    if (!state) state = JsonObject{};
    state.SetNamedValue(key, JsonValue::CreateStringValue(value));
    data.SetNamedValue(L"state", state);
    saveLocalData(data);
}

long long Setting::getLocalStateInt(const wchar_t* key, long long def)
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    auto data = loadLocalData();
    if (!data.HasKey(L"state") || data.GetNamedValue(L"state").ValueType() != JsonValueType::Object)
        return def;
    auto state = data.GetNamedObject(L"state");
    if (!state.HasKey(key) || state.GetNamedValue(key).ValueType() != JsonValueType::Number)
        return def;
    return static_cast<long long>(state.GetNamedValue(key).GetNumber());
}

void Setting::setLocalStateInt(const wchar_t* key, long long value)
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    auto data = loadLocalData();
    JsonObject state{ nullptr };
    if (data.HasKey(L"state") && data.GetNamedValue(L"state").ValueType() == JsonValueType::Object)
        state = data.GetNamedObject(L"state");
    if (!state) state = JsonObject{};
    state.SetNamedValue(key, JsonValue::CreateNumberValue(static_cast<double>(value)));
    data.SetNamedValue(L"state", state);
    saveLocalData(data);
}

std::wstring Setting::getLocalDataText(const std::wstring& key)
{
    const auto parts = splitLocalKey(key);
    if (parts.empty()) return {};
    JsonObject cur = loadLocalData();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!cur || !cur.HasKey(parts[i])) return {};
        const JsonValue value = cur.GetNamedValue(parts[i]);
        if (i + 1 == parts.size())
            return value.ValueType() == JsonValueType::Object ? std::wstring{ value.Stringify() } : std::wstring{};
        if (value.ValueType() != JsonValueType::Object) return {};
        cur = value.GetObjectW();
    }
    return {};
}

bool Setting::setLocalDataText(const std::wstring& key, const std::wstring& json)
{
    const auto parts = splitLocalKey(key);
    if (parts.empty()) return false;
    JsonObject value{ nullptr };
    if (!JsonObject::TryParse(json, value) || !value) return false;
    // 插件启动时可能重复保存同一份数据，内容没变就不写盘。
    if (getLocalDataText(key) == std::wstring{ value.Stringify() }) return true;

    JsonObject data = loadLocalData();
    if (!data) data = JsonObject();
    JsonObject cur = data;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (cur.HasKey(parts[i]) && cur.GetNamedValue(parts[i]).ValueType() == JsonValueType::Object) {
            cur = cur.GetNamedValue(parts[i]).GetObjectW();
            continue;
        }
        JsonObject next;
        cur.SetNamedValue(parts[i], next);
        cur = next;
    }
    cur.SetNamedValue(parts.back(), value);
    return saveLocalData(data);
}

// 将旧版本分散的本机数据迁移到 local.json：
//   - 旧版本拆分出的 clipboard.json / history.json / state.json
//   - 更早版本直接写在 config.json 的 features.clipboard / features.history / features.lastCaptureRect
// 迁移成功后再删除旧文件和 config 中的旧键；任一步骤失败则保留原数据，下次启动重试。
void Setting::migrateLocalDataToFile()
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    const auto path = getLocalDataPath();

    std::wstring existing;
    JsonObject data{ nullptr };
    bool localReady = false;
    if (readConfigContent(path, existing) && !isBlankConfig(existing)) {
        JsonObject parsed{ nullptr };
        const bool parsedOk = JsonObject::TryParse(existing, parsed);
        if (parsedOk && (parsed.HasKey(L"clipboard") || parsed.HasKey(L"history") || parsed.HasKey(L"state"))) {
            data = parsed;
            localReady = true;
        }
        else if (!parsedOk) {
            // 文件损坏：备份为 .bad，避免迁移直接覆盖
            std::error_code ec;
            std::filesystem::copy_file(path, siblingOf(path, L".bad"),
                std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    bool tookClipboard = false, tookHistory = false, tookState = false;
    if (!localReady) {
        data = JsonObject{};

        auto takeFile = [&](const wchar_t* name, const wchar_t* section) -> bool {
            std::wstring text;
            if (!readConfigContent(dataPath / name, text) || isBlankConfig(text)) return false;
            JsonObject obj{ nullptr };
            if (!JsonObject::TryParse(text, obj)) return false;   // 解析失败则保留原文件，不参与迁移
            data.SetNamedValue(section, obj);
            return true;
        };
        tookClipboard = takeFile(L"clipboard.json", L"clipboard");
        tookHistory = takeFile(L"history.json", L"history");
        tookState = takeFile(L"state.json", L"state");

        auto features = getFeaturesObjectOrNull(configObj);
        if (features) {
            if (!data.HasKey(L"clipboard") && features.HasKey(L"clipboard")
                && features.GetNamedValue(L"clipboard").ValueType() == JsonValueType::Object) {
                data.SetNamedValue(L"clipboard", features.GetNamedValue(L"clipboard").GetObject());
            }
            if (!data.HasKey(L"history") && features.HasKey(L"history")
                && features.GetNamedValue(L"history").ValueType() == JsonValueType::Object) {
                data.SetNamedValue(L"history", features.GetNamedValue(L"history").GetObject());
            }
            if (!data.HasKey(L"state") && features.HasKey(L"lastCaptureRect")
                && features.GetNamedValue(L"lastCaptureRect").ValueType() == JsonValueType::String) {
                JsonObject state{ nullptr };
                state.SetNamedValue(L"lastCaptureRect",
                    JsonValue::CreateStringValue(features.GetNamedString(L"lastCaptureRect", L"")));
                data.SetNamedValue(L"state", state);
            }
        }

        bool hasAny = data.HasKey(L"clipboard") || data.HasKey(L"history") || data.HasKey(L"state");
        if (hasAny && !saveLocalData(data)) return;
    }

    // 清理旧位置：仅删除已成功迁移的文件，损坏文件保留
    auto removeLegacy = [&](const wchar_t* name, bool taken) {
        if (!taken && !localReady) return;
        std::error_code ec;
        std::filesystem::remove(dataPath / name, ec);
    };
    // 旧文件保留：新版本以 local.json 为准，但回退旧版本时仍要能读到 clipboard/history/state。
    (void)removeLegacy;

    // 旧字段保留在 config.json：新版本以 local.json 为准，但旧版本仍要能读同一份配置。
    // 不再删除 clipboard / history / lastCaptureRect，避免回退旧版本后数据消失。
}

void Setting::purgeNonUserConfig()
{
    // 保留为兼容入口：旧版本可能仍读取这些字段，新版本只把 local.json 当权威数据源，
    // 不再从 config.json 删除历史字段。后续确认旧版本完全退场后再启用清理。
}

JsonObject Setting::loadHistoryStore()
{
    auto data = loadLocalData();
    if (data.HasKey(L"history") && data.GetNamedValue(L"history").ValueType() == JsonValueType::Object)
        return data.GetNamedObject(L"history");
    return JsonObject{};
}

bool Setting::saveHistoryStore(const JsonObject& store)
{
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    auto data = loadLocalData();
    data.SetNamedValue(L"history", store);
    return saveLocalData(data);
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
    auto store = loadHistoryStore();
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
bool Setting::isOwnHistoryFile(const std::wstring& filePath)
{
    if (filePath.empty()) return false;
    std::wstring name = std::filesystem::path(filePath).filename().wstring();
    for (auto& c : name) c = (wchar_t)towlower(c);
    auto prefixOf = [](std::wstring fmt) {
        if (fmt.empty()) return std::wstring{};
        const auto brace = fmt.find(L"{{");
        std::wstring p = brace == std::wstring::npos ? fmt : fmt.substr(0, brace);
        if (const auto slash = p.find_last_of(L"/\\"); slash != std::wstring::npos)
            p = p.substr(slash + 1);
        for (auto& c : p) c = (wchar_t)towlower(c);
        return p;
    };
    std::wstring manualFmt = getManualSaveFormat();
    if (manualFmt.empty()) manualFmt = defaultManualSaveFormat();
    std::wstring autoFmt = getAutoSaveFormat();
    if (autoFmt.empty()) autoFmt = defaultAutoSaveFormat();
    auto startsWith = [&](const std::wstring& p) {
        return !p.empty() && name.size() >= p.size() && name.compare(0, p.size(), p) == 0;
    };
    return startsWith(prefixOf(manualFmt)) || startsWith(prefixOf(autoFmt));
}

std::vector<Setting::HistoryItem> Setting::getHistoryItemsForUi(bool forceRescan)
{
    const auto nowTick = GetTickCount64();
    if (!forceRescan && !historyUiCacheDirty_ && nowTick - historyUiCacheAt_ < 1500)
        return historyUiCache_;

    auto list = getHistoryItems();
    std::set<std::wstring> seen;
    for (const auto& it : list) seen.insert(historyPathKey(it.filePath));

    // 历史页 = 索引 + 目录扫描（可见即所得）：删记录不等于删文件，
    // 文件还在目录里就照常显示，不再用隐藏列表把它压掉。
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
    auto store = loadHistoryStore();
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
    auto store = loadHistoryStore();
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
    saveHistoryStore(store);
    historyUiCacheDirty_ = true;
}

std::vector<std::wstring> Setting::getAllHistoryTags()
{
    std::set<std::wstring> uniq;
    auto store = loadHistoryStore();
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

    auto store = loadHistoryStore();
    store.SetNamedValue(L"items", arr);
    saveHistoryStore(store);
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

    auto store = loadHistoryStore();
    store.SetNamedValue(L"items", arr);
    saveHistoryStore(store);
    return it.id;
}

namespace {
bool isUnderDir(const std::wstring& file, const std::filesystem::path& dir)
{
    const std::wstring d = dir.lexically_normal().wstring();
    if (file.size() <= d.size()) return false;
    if (_wcsicmp(file.substr(0, d.size()).c_str(), d.c_str()) != 0) return false;
    const wchar_t sep = file[d.size()];
    return sep == L'\\' || sep == L'/';
}

// 只有明确的缓存目录才永久删除；截图/录屏等用户文件仍一律进回收站。
bool isCachePath(const std::wstring& path)
{
    auto* s = Setting::get();
    if (!s || path.empty()) return false;
    const std::wstring file = std::filesystem::path(path).lexically_normal().wstring();
    const auto data = s->getDataPath();
    return isUnderDir(file, data / L"Clipboard") || isUnderDir(file, data / L"ThumbCache") ||
           isUnderDir(file, data / L"RackQuick");
}

void deleteHistoryFile(const std::wstring& path)
{
    if (isCachePath(path)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    } else {
        Util::moveToRecycleBin(path);
    }
}
} // namespace

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
        if (!it.filePath.empty()) deleteHistoryFile(it.filePath);
        std::filesystem::remove(getHistoryThumbDir() / (it.id + L".png"), ec);
    }
    // 写回
    JsonArray arr;
    for (auto& it : keep) arr.Append(historyItemToJson(it));
    auto store = loadHistoryStore();
    store.SetNamedValue(L"items", arr);
    saveHistoryStore(store);
}

void Setting::pruneOrphanThumbCache()
{
    auto dir = getHistoryThumbDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;

    std::vector<std::wstring> keep;
    for (const auto& it : getHistoryItems()) {
        if (it.source != L"record" || it.filePath.empty()) continue;
        std::error_code fileEc;
        if (!std::filesystem::exists(it.filePath, fileEc)) continue;
        keep.push_back(it.id + L".png");
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        if (_wcsicmp(entry.path().extension().c_str(), L".png") != 0) continue;
        const std::wstring name = entry.path().filename().wstring();
        bool referenced = false;
        for (const auto& kept : keep) {
            if (_wcsicmp(name.c_str(), kept.c_str()) == 0) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            std::error_code removeEc;
            std::filesystem::remove(entry.path(), removeEc);
        }
    }
}

void Setting::removeHistoryItem(const std::wstring& id, bool deleteFile)
{
    historyUiCacheDirty_ = true;
    auto list = getHistoryItems();
    std::vector<HistoryItem> keep;
    bool found = false;
    for (auto& it : list) {
        if (it.id == id) {
            found = true;
            // 历史即真实文件：deleteFile=true 时删条目就删文件（缓存直接删，用户文件走回收站）
            std::error_code ec;
            if (deleteFile && !it.filePath.empty()) deleteHistoryFile(it.filePath);
            std::filesystem::remove(getHistoryThumbDir() / (it.id + L".png"), ec);
        } else {
            keep.push_back(it);
        }
    }
    // 目录扫描出来的条目没有索引 id（id 就是文件路径）：deleteFile=true 时直接删文件
    if (!found && deleteFile) {
        std::error_code ec;
        if (std::filesystem::exists(id, ec)) deleteHistoryFile(id);
    }
    // 只删记录：文件原地保留，目录扫描会继续把它显示出来（删记录 != 删文件）
    JsonArray arr;
    for (auto& it : keep) arr.Append(historyItemToJson(it));
    auto store = loadHistoryStore();
    store.SetNamedValue(L"items", arr);
    saveHistoryStore(store);
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
        if (!it.filePath.empty()) deleteHistoryFile(it.filePath);
        std::filesystem::remove(getHistoryThumbDir() / (it.id + L".png"), ec);
    }
    // 索引里保留没被清掉的分类
    JsonArray arr;
    for (auto& it : getHistoryItems()) {
        const bool isRecord = (it.source == L"record");
        if ((sourceTab == 1 && isRecord) || (sourceTab == 2 && !isRecord))
            arr.Append(historyItemToJson(it));
    }
    auto store = loadHistoryStore();
    store.SetNamedValue(L"items", arr);
    saveHistoryStore(store);
}

// 「其他」：目录扫描出来的条目（id 就是文件路径）是用户自己放进来的文件。
// 这里只清这些文件（走回收站），软件登记过的截图/录屏不动。
void Setting::clearOtherFiles()
{
    historyUiCacheDirty_ = true;
    auto list = getHistoryItemsForUi(true);
    for (auto& it : list) {
        if (it.id != it.filePath) continue;
        deleteHistoryFile(it.filePath);
    }
    historyUiCacheDirty_ = true;
}

// ================= 剪切板历史 =================
// 剪贴板历史保存在 local.json 的 clipboard 节，不写入 config.json：
//  - 每次复制都会重写配置，在同步盘上会持续产生冲突；
//  - 剪贴板内容（可能包含密码、验证码）不应随配置上传云端；
//  - 本机剪贴板历史对其他电脑没有意义。
// 不限条数：文本直接进索引，图片落盘到 dataPath\Clipboard（历史页用缩略图展示）。
namespace {
    unsigned clipboardHash(const std::vector<BYTE>& data)
    {
        unsigned h = 2166136261u;
        for (BYTE b : data) { h ^= b; h *= 16777619u; }
        return h;
    }

    std::vector<Setting::ClipboardItem> clipboardItemsFromArray(const JsonArray& arr)
    {
        std::vector<Setting::ClipboardItem> out;
        if (!arr) return out;
        for (auto const& v : arr) {
            if (v.ValueType() != JsonValueType::Object) continue;
            auto o = v.GetObjectW();
            Setting::ClipboardItem it;
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
            if (it.type == L"files") {
                if (it.files.empty()) continue;
                // 文件条目只保存路径，不复制内容：源文件全部不存在时（例如微信临时文件被清理），
                // 与图片条目一致，不再显示这条失效记录；文件恢复后会自动重新出现。
                bool anyExists = false;
                for (const auto& f : it.files) {
                    std::error_code ec;
                    if (std::filesystem::exists(f, ec)) { anyExists = true; break; }
                }
                if (!anyExists) continue;
            }
            if (it.type == L"text" && it.text.empty()) continue;
            out.push_back(std::move(it));
        }
        return out;
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
    auto data = loadLocalData();
    if (!data.HasKey(L"clipboard") || data.GetNamedValue(L"clipboard").ValueType() != JsonValueType::Object)
        return out;
    auto store = data.GetNamedObject(L"clipboard");
    if (!store.HasKey(L"items") || store.GetNamedValue(L"items").ValueType() != JsonValueType::Array)
        return out;
    out = clipboardItemsFromArray(store.GetNamedArray(L"items"));
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
bool Setting::writeClipboardItemsToFile(const std::vector<ClipboardItem>& items)
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
    JsonObject clipboard;
    clipboard.SetNamedValue(L"items", arr);

    // 本地数据集中在一个文件：剪贴板、历史与状态分别修改各自节点，读-改-写必须串行，
    // 避免并发写入互相覆盖。
    std::lock_guard<std::recursive_mutex> lock(localDataMutex());
    auto data = loadLocalData();
    data.SetNamedValue(L"clipboard", clipboard);
    return saveLocalData(data);
}

void Setting::writeClipboardItems(std::vector<ClipboardItem>& items)
{
    if (!writeClipboardItemsToFile(items)) {
        OutputDebugStringW(L"[SnowAir] failed to write local.json (clipboard)\n");
        return;   // 写入失败时不通知界面刷新，避免显示未持久化的内容
    }
    if (clipboardChanged) clipboardChanged();
}

namespace {
    std::wstring normalizedClipboardText(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            wchar_t c = text[i];
            if (c == L'\r') {
                if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
                out.push_back(L'\n');
            } else {
                out.push_back(c);
            }
        }
        while (!out.empty() &&
               (out.back() == L' ' || out.back() == L'\t' || out.back() == L'\n')) {
            out.pop_back();
        }
        return out;
    }

    std::wstring normalizedClipboardPath(const std::wstring& path)
    {
        std::wstring s = std::filesystem::path(path).lexically_normal().wstring();
        for (auto& c : s) c = (wchar_t)towlower(c);
        return s;
    }

    std::wstring clipboardFilesKey(const std::vector<std::wstring>& files)
    {
        std::vector<std::wstring> normalized;
        normalized.reserve(files.size());
        for (const auto& f : files) {
            if (!f.empty()) normalized.push_back(normalizedClipboardPath(f));
        }
        std::sort(normalized.begin(), normalized.end());
        std::wstring key;
        for (const auto& f : normalized) {
            key += f;
            key.push_back(L'\n');
        }
        return key;
    }

    bool sameClipboardContent(const Setting::ClipboardItem& a, const Setting::ClipboardItem& b)
    {
        if (a.type == L"text" && b.type == L"text") {
            return normalizedClipboardText(a.text) == normalizedClipboardText(b.text);
        }
        if (a.type == L"image" && b.type == L"image") {
            return a.hash == b.hash && a.width == b.width && a.height == b.height;
        }
        if (a.type == L"files" && b.type == L"files") {
            return clipboardFilesKey(a.files) == clipboardFilesKey(b.files);
        }
        // 同一个图片文件既可能以 image 记一条，也可能被外部程序当文件复制成 files；
        // 两条指向同一路径时视为同一内容。
        if (a.type == L"image" && b.type == L"files" && b.files.size() == 1) {
            return !a.filePath.empty() &&
                   normalizedClipboardPath(a.filePath) == normalizedClipboardPath(b.files.front());
        }
        if (a.type == L"files" && b.type == L"image" && a.files.size() == 1) {
            return !b.filePath.empty() &&
                   normalizedClipboardPath(b.filePath) == normalizedClipboardPath(a.files.front());
        }
        return false;
    }

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
        const std::wstring key = normalizedClipboardText(s);
        const int dup = promoteClipboardItem(items, (long long)nowMs, [&](const ClipboardItem& it) {
            return it.type == L"text" && normalizedClipboardText(it.text) == key;
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
    const std::wstring filesKey = clipboardFilesKey(files);
    unsigned hash = 2166136261u;
    for (wchar_t c : filesKey) {
        hash ^= (unsigned)c;
        hash *= 16777619u;
    }
    auto items = getClipboardItems();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 同一组文件只挪到最前，不再加一条；如果是某个已存在的图片缓存文件，也并回图片条目。
    ClipboardItem probe;
    probe.type = L"files";
    probe.files = files;
    const int dup = promoteClipboardItem(items, (long long)nowMs, [&](const ClipboardItem& it) {
        return sameClipboardContent(it, probe);
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

void Setting::dedupeClipboardItems()
{
    auto items = getClipboardItems();
    std::vector<ClipboardItem> out;
    bool changed = false;
    for (auto& it : items) {
        size_t duplicate = static_cast<size_t>(-1);
        for (size_t i = 0; i < out.size(); ++i) {
            if (sameClipboardContent(out[i], it)) {
                duplicate = i;
                break;
            }
        }
        if (duplicate == static_cast<size_t>(-1)) {
            out.push_back(std::move(it));
            continue;
        }

        changed = true;
        ClipboardItem& kept = out[duplicate];
        for (const auto& tag : it.tags) {
            if (std::find(kept.tags.begin(), kept.tags.end(), tag) == kept.tags.end()) {
                kept.tags.push_back(tag);
            }
        }
        if (it.type == L"image" && !it.filePath.empty()) {
            // 只有确认没有别的条目还引用这个 PNG 时才删缓存。
            bool referenced = false;
            if (kept.type == L"image" && !kept.filePath.empty() &&
                normalizedClipboardPath(kept.filePath) == normalizedClipboardPath(it.filePath)) {
                referenced = true;
            }
            for (const auto& f : kept.files) {
                if (normalizedClipboardPath(f) == normalizedClipboardPath(it.filePath)) {
                    referenced = true;
                    break;
                }
            }
            if (!referenced) {
                std::error_code ec;
                std::filesystem::remove(it.filePath, ec);
            }
        }
    }
    if (changed) writeClipboardItems(out);
}

void Setting::removeClipboardItem(const std::wstring& id)
{
    auto items = getClipboardItems();
    std::vector<ClipboardItem> keep;
    for (auto& it : items) {
        if (it.id == id) {
            std::error_code ec;
            if (it.type == L"image" && !it.filePath.empty()) {
                std::filesystem::remove(it.filePath, ec);
            } else if (it.type == L"files") {
                for (const auto& f : it.files) {
                    if (isCachePath(f)) std::filesystem::remove(f, ec);
                }
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
        std::error_code ec;
        if (it.type == L"image" && !it.filePath.empty()) {
            std::filesystem::remove(it.filePath, ec);
        } else if (it.type == L"files") {
            for (const auto& f : it.files) {
                if (isCachePath(f)) std::filesystem::remove(f, ec);
            }
        }
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


