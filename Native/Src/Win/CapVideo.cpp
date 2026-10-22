#include "pch.h"
#include <include/Ling.h>
#include "CapVideo.h"
#include "WinCap.h"
#include "CutMask.h"
#include "../Tool/ToolVideo.h"
#include "../App.h"
#include "../Setting.h"
#include "../Lang.h"
#include "../Util.h"
// VideoMp4.hpp / VideoGif.hpp 里用的是裸 ComPtr，本项目的 pch 没有这条 using，
// 在包含它们之前补上，头文件本身保持原样。
using namespace Microsoft::WRL;
#include "VideoMp4.hpp"
#include "VideoGif.hpp"
#include <algorithm>
#include <vector>

namespace {
    // 录制临时文件名直接用「视频文件名格式」的末段。
    // 录完点 ✓ 是把**这个临时文件**放进剪贴板，QQ/微信会原样带上文件名 ——
    // 以前固定叫 temp.mp4 / temp.gif，粘贴出去就是 "temp"；现在和设置里看到的名字一致。
    std::wstring recordTempPath(const std::wstring& ext)
    {
        auto* st = Setting::get();
        std::wstring name = Util::previewFileName(
            st->getVideoRecordFormat(), Setting::defaultVideoRecordFormat(), L"");
        if (auto slash = name.find_last_of(L'/'); slash != std::wstring::npos)
            name = name.substr(slash + 1);
        // 格式模板是用户可编辑的：把文件名里不允许的字符换成下划线
        for (auto& c : name) {
            if (c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"'
                || c == L'<' || c == L'>' || c == L'|' || c < 0x20) c = L'_';
        }
        while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
        if (name.empty()) name = L"SnowAir";
        return (st->getDataPath() / (name + L"." + ext)).wstring();
    }

    // MP4 录制失败以前是静默 return，用户那边就是"录完什么都没有"，连缓存文件都不生成。
    // 失败原因基本都在对方机器上（混合显卡笔记本把进程放在独显上跑，桌面复制就不成立），
    // 实测让用户在系统图形设置里给本程序选"节能"（把进程放回核显）能解决，
    // 所以不去区分错误码了，统一给这一句最有用的话。
    // 录制界面的窗口是 WS_EX_TOPMOST 的，弹框得跟上，不然会被压在后面看不见
    void showRecordError(const std::wstring& text, const std::wstring& title)
    {
        MessageBoxW(nullptr, text.data(), title.data(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }

    // 可能管着录制区域那块屏的 (适配器, 输出) 组合
    struct DupTarget
    {
        CComPtr<IDXGIAdapter1> ad;
        UINT output{ 0 };
        RECT mon{}; //这块屏在虚拟桌面里的位置
    };

    // 按 DXGI 枚举顺序收集所有报告了 rect 所在那块屏的 (适配器, 输出)。
    // 原来 dp.ad / dp.nOutput 全是 0，意思是"默认适配器的第 0 个输出"：双显卡笔记本上
    // 默认适配器可能是独显而内屏挂在核显上，独显的 EnumOutputs(0) 直接失败；
    // 多显示器时也永远只录得到枚举出来的第一块屏。
    // 收成一个列表而不是直接挑一个，是因为混合显卡笔记本上独显和核显会同时报告同一块屏，
    // 而 DuplicateOutput 只在真正拥有这块屏的那个适配器上成立，另一个返回
    // DXGI_ERROR_UNSUPPORTED(0x887A0004) —— 光看 desc 分不出来，只能一个个试
    void collectDupTargets(const RECT& rect, std::vector<DupTarget>& targets)
    {
        HMONITOR hMon = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
        if (!hMon) return;
        CComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) return;
        for (UINT i = 0;; ++i) {
            CComPtr<IDXGIAdapter1> ad;
            if (FAILED(factory->EnumAdapters1(i, &ad)) || !ad) break;
            for (UINT j = 0;; ++j) {
                CComPtr<IDXGIOutput> output;
                if (FAILED(ad->EnumOutputs(j, &output)) || !output) break;
                DXGI_OUTPUT_DESC desc{};
                if (FAILED(output->GetDesc(&desc)) || desc.Monitor != hMon) continue;
                targets.push_back({ ad, j, desc.DesktopCoordinates });
            }
        }
    }

    // 把桌面坐标的录制区域换算成某块屏的局部坐标 —— DesktopCapture 的裁剪是相对复制出来
    // 那张纹理算的，纹理原点是这块屏的左上角，不是虚拟桌面的原点。
    // 宽高的对齐要求见 startMp4 里的注释，平移之后得重新对一遍
    RECT toLocalRect(const RECT& rect, const RECT& mon)
    {
        long l = std::max(rect.left, mon.left) - mon.left;
        long t = std::max(rect.top, mon.top) - mon.top;
        long r = std::min(rect.right, mon.right) - mon.left;
        long b = std::min(rect.bottom, mon.bottom) - mon.top;
        l = (l + 3) & ~3l;
        t = (t + 1) & ~1l;
        return { l, t, l + std::max(4l, (r - l) & ~3l), t + std::max(2l, (b - t) & ~1l) };
    }
}

CapVideo::CapVideo(WinCap* win) : win(win)
{
	// 开录前不穿透：还要能拖/缩放选区；真正 startMp4/Gif 时再让出鼠标
}

CapVideo::~CapVideo()
{
}

void CapVideo::makeTool()
{
    tool = std::make_unique<ToolVideo>(win);
    layoutTool();
    tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
    // 挂到宿主下并抬到最前，避免被全屏遮罩盖住（开录前不穿透）
    SetWindowLongPtr(tool->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(win->hwnd));
    layoutTool();
    SetWindowPos(tool->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // 「录制时隐藏工具栏」= 把工具栏排除在录制画面之外（录制画面里看不到它），不是把工具栏藏起来
    if (Setting::get()->getHideToolbarInRecord()) App::excludeFromCapture(tool->hwnd);
}

void CapVideo::layoutTool()
{
    if (tool) win->layoutTool(tool.get());
}

void CapVideo::setToolVisible(bool visible)
{
	if (!tool || !tool->hwnd) return;
	if (visible) {
		layoutTool();
		tool->show();
	}
	else {
		tool->hide();
	}
}

void CapVideo::dispose()
{
    stop();
    if (tool) tool->close();
}

bool CapVideo::isRecording() const
{
    return mp4Param || gifParam;
}

void CapVideo::setPaused(bool on)
{
	paused = on;
	if (mp4Param) mp4Param->Pause = on;
	if (gifParam) gifParam->isPaused = on;
}

void CapVideo::notifyHistoryChanged()
{
	if (tool) tool->updateUndoEnabled();
}

void CapVideo::cancelAnnotSelect()
{
	if (tool) tool->cancelAnnotSelect();
}

void CapVideo::syncPierceBtn(bool on)
{
	if (tool) tool->syncPierce(on);
}

HWND CapVideo::toolHwnd() const
{
	return tool ? tool->hwnd : nullptr;
}

bool CapVideo::queryBarAnchor(float& x, float& y, float& w, float& h, float& btnCenterX, float& dpi) const
{
	if (!tool) return false;
	x = (float)tool->x;
	y = (float)tool->y;
	w = tool->w;
	h = tool->h;
	btnCenterX = tool->getBtnCenterX();
	dpi = tool->dpi;
	return true;
}

// 与 targetDesk 同适配器的显示器列表。采集侧的换屏只可能在**同一个 D3D 设备（= 同一适配器）**
// 下的输出之间发生（Prepare 是从设备反查适配器的），所以这里按 DXGI 的适配器分组给出候选。
// 不建 D3D 设备，只用工厂枚举，UI 线程上每次进录屏调用一次。
std::vector<RECT> CapVideo::reachableMonitors(const RECT& targetDesk)
{
    std::vector<RECT> out;
    CComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) return out;
    for (UINT i = 0;; ++i) {
        CComPtr<IDXGIAdapter1> ad;
        if (FAILED(factory->EnumAdapters1(i, &ad)) || !ad) break;
        std::vector<RECT> mine;
        bool hasTarget = false;
        for (UINT j = 0;; ++j) {
            CComPtr<IDXGIOutput> output;
            if (FAILED(ad->EnumOutputs(j, &output)) || !output) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            const RECT& r = desc.DesktopCoordinates;
            mine.push_back(r);
            if (r.left == targetDesk.left && r.top == targetDesk.top
                && r.right == targetDesk.right && r.bottom == targetDesk.bottom) hasTarget = true;
        }
        if (hasTarget) return mine;   // 同一块屏可能被多个适配器报告，取第一个命中的那组
    }
    return out;
}

bool CapVideo::canSwitchToMonitor(const RECT& mon) const
{
    if (gifParam || !mp4Param) return true;   // 动图走 GDI / 还没开录：不设限
    // 采集线程已经试过、换不过去的那块屏（混合显卡上"适配器报告了这块屏、实际复制不起来"）
    if (mp4Param->hasBlocked.load(std::memory_order_relaxed)) {
        const long long p = mp4Param->switchBlocked.load(std::memory_order_relaxed);
        if ((long)(int)(p >> 32) == mon.left && (long)(int)(p & 0xFFFFFFFFll) == mon.top) return false;
    }
    if (reachable_.empty()) return true;   // 拿不到显卡分组信息时不设限（照旧只靠采集侧的兜底）
    for (const auto& r : reachable_) {
        if (r.left == mon.left && r.top == mon.top && r.right == mon.right && r.bottom == mon.bottom) return true;
    }
    return false;
}

void CapVideo::startMp4(bool useSpeaker, bool useMic)
{
    auto& cutMask = win->cutMask;
    auto* st = Setting::get();
    mp4Param = std::make_unique<VideoMp4::DESKTOPCAPTUREPARAMS>();
    // 录制中选区可整体拖动：记下开录时的原点，之后每帧按位移平移裁剪框（见 syncCaptureMove）
    capBaseX = (int)std::lround((float)win->x + cutMask->maskRect.left);
    capBaseY = (int)std::lround((float)win->y + cutMask->maskRect.top);
    syncCaptureMove();   // 把跟随打开并报一次初始原点（采集线程据它算跟随基准）
    // 编码格式不在这里定，交给下面采集线程里那个"HEVC 不行就退 H.264"的循环
    // 录制区域先夹回桌面范围，再做对齐 —— 只会往里缩，不会越出桌面。
    // HEVC 编码器要求宽高是偶数，链路中间的 RGB32->NV12 转换还会按对齐后的 stride
    // 去读我们交出去的缓冲区，宽度不是 4 的倍数（stride 凑不满 16 字节）时，
    // 有的驱动编码器会读到缓冲区外面，首帧就崩在驱动里。
    // 所以宽度对齐到 4、高度对齐到 2，代价是最多少录右侧 3 像素、底部 1 像素。
    long rcLeft = std::max((long)win->x, (long)(win->x + cutMask->maskRect.left));
    long rcTop = std::max((long)win->y, (long)(win->y + cutMask->maskRect.top));
    long rcRight = std::min((long)(win->x + win->w), (long)(win->x + cutMask->maskRect.right));
    long rcBottom = std::min((long)(win->y + win->h), (long)(win->y + cutMask->maskRect.bottom));
    // 左上角往里取整，宽高往下取整，两头都不会超出上面夹好的范围
    rcLeft = (rcLeft + 3) & ~3l;
    rcTop = (rcTop + 1) & ~1l;
    // 框选不可能真的小到 4x2，这里只是别让宽高变成 0：rx 全 0 会被当成"录整屏"
    const long rcW = std::max(4l, (rcRight - rcLeft) & ~3l);
    const long rcH = std::max(2l, (rcBottom - rcTop) & ~1l);
    mp4Param->rx = { rcLeft, rcTop, rcLeft + rcW, rcTop + rcH };
    // 「能跨过去的那几块屏」：与开录目标同显卡适配器的那些（选区跨屏时据此决定跟不跟）
    {
        RECT deskRect{ rcLeft, rcTop, rcLeft + rcW, rcTop + rcH };
        MONITORINFO mi{ sizeof(MONITORINFO) };
        HMONITOR hMon = MonitorFromRect(&deskRect, MONITOR_DEFAULTTONEAREST);
        reachable_.clear();
        if (hMon && GetMonitorInfoW(hMon, &mi)) reachable_ = reachableMonitors(mi.rcMonitor);
    }
    mp4Param->f = recordTempPath(L"mp4");
    mp4Param->EndMS = 0;
    // 「功能设置 → 录制」：分辨率 / 帧率 / 码率质量 / 硬件加速
    static const int kMaxOutH[]{ 480, 720, 1080, 1440, 2160 };
    mp4Param->MaxOutHeight = kMaxOutH[std::clamp(st->getVideoQuality(), 0, 4)];
    // 「视频帧率」：0/1/2 保持原来的 24/30/60，后面追加低帧率（体积更小）
    static const int kFps[]{ 24, 30, 60, 15, 10, 5 };
    mp4Param->fps = kFps[std::clamp(st->getVideoFps(), 0, 5)];
    // 质量档（0~100）：录屏走 Quality 模式 —— 静止画面只花很少码率，动态画面优先保画质。
    // ★ 必须显式设 vbrm=2，否则 VideoMp4 里"按 vbrq 定质量"的分支不会执行，
    //   编码器会走驱动默认码控（经常是 CBR），体积既大又不听这个选项的。
    static const int kBitrate[]{ 2500, 6000, 12000 };   // 平均码率目标（部分 MFT 需要）
    static const int kQuality[]{ 40, 50, 60 };
    const int speed = std::clamp(st->getEncodeSpeed(), 0, 2);
    mp4Param->BR = kBitrate[speed];
    mp4Param->vbrq = kQuality[speed];
    mp4Param->vbrm = 2;                                  // 质量优先
    mp4Param->Qu = 50;
    mp4Param->HwTransforms = st->getHwAccel();
    mp4Param->MustEnd = false;
    // 麦克风：设置为「关闭」时，无视工具条上的麦克风开关
    const bool mic = useMic && st->getRecordMic() != 1;
    VideoMp4::setAudio(mp4Param.get(), useSpeaker, mic);
    // 编码器偏好：0 自动（HEVC 优先、失败退 H.264）/ 1 仅 H.264 / 2 仅 H.265
    std::vector<GUID> codecs;
    switch (st->getVideoEncoder()) {
    case 1: codecs = { MFVideoFormat_H264 }; break;
    case 2: codecs = { MFVideoFormat_HEVC }; break;
    default: codecs = { MFVideoFormat_HEVC, MFVideoFormat_H264 }; break;
    }
    // 文案在这里先取出来按值带进线程：langObj 是 WinRT 对象，不往采集线程里带
    auto errText = Lang::get(L"video.recordFailed");
    auto errTitle = Lang::get(L"about.sysTip");
    captureThread = std::jthread([this, errText, errTitle, codecs](std::stop_token st2) {
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            showRecordError(errText, errTitle);
            return;
        }
        hr = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            showRecordError(errText, errTitle);
            return;
        }
        // 逐个候选试到桌面复制能成立为止。Prepare 挂得很早（缓存文件都还没建），
        // 所以换个候选整个重来最省事，也不用为了预探测多建一次复制对象。
        // targets 得活到 DesktopCapture 返回：dp.ad 是裸指针，生命周期归我们管
        std::vector<DupTarget> targets;
        collectDupTargets(mp4Param->rx, targets);
        const RECT deskRx = mp4Param->rx; //每个候选都从桌面坐标重新换算
        int code = 0;
        // 编码格式也得按可用性退让。HEVC 文件小得多，但它的编码器不是每台机器都有 ——
        // 有的只带解码，有的靠显卡的硬件 MFT 而显卡没有。缺了的时候 AddStream 照样成功
        //（那一步只是记下输出类型），要到 SetInputMediaType 解析编码器时才失败(-10)。
        // 退回 H.264：它从 Win7 起就带软件编码器，没有装不上的道理，代价是同画质下文件更大
        for (auto codec : codecs) {
            mp4Param->VIDEO_ENCODING_FORMAT = codec;
            if (targets.empty()) {
                mp4Param->rx = deskRx;
                code = VideoMp4::DesktopCapture(*(mp4Param.get())); //一个都没匹配上，走库原来的默认行为
            }
            for (auto& t : targets) {
                if (mp4Param->MustEnd) break; //已经点了停止，别再起新的
                mp4Param->ad = t.ad;
                mp4Param->nOutput = t.output;
                mp4Param->rx = toLocalRect(deskRx, t.mon);
                code = VideoMp4::DesktopCapture(*(mp4Param.get()));
                if (code != -2) break; //成功了，或者失败在跟适配器无关的地方
            }
            if (mp4Param->MustEnd) break;
            // 只有这两个码是"这台机器认不了这个编码器"，换编码器才有意义
            if (code != -5 && code != -10) break;
        }
        if (code != 0) showRecordError(errText, errTitle);
        CoUninitialize();
        MFShutdown();
    });


}

void CapVideo::startGif()
{
    auto& cutMask = win->cutMask;
    gifParam = std::make_unique<VideoGif::GifParam>();
    // 同 MP4：记下开录时的原点，录制中整体拖动时按位移平移抓取区域
    capBaseX = (int)std::lround((float)win->x + cutMask->maskRect.left);
    capBaseY = (int)std::lround((float)win->y + cutMask->maskRect.top);
    gifParam->x = (int)(win->x + cutMask->maskRect.left);
    gifParam->y = (int)(win->y + cutMask->maskRect.top);
    gifParam->w = (int)(cutMask->maskRect.right - cutMask->maskRect.left);
    gifParam->h = (int)(cutMask->maskRect.bottom - cutMask->maskRect.top);
    // 「动图格式」：0 = GIF / 1 = WebP / 2 = APNG。
    // WebP 由内置编码器输出，不再依赖系统 WIC，无需降级
    int gifFmt = std::clamp(Setting::get()->getGifFormat(), 0, 2);
    static const wchar_t* kGifExt[]{ L"gif", L"webp", L"png" };
    gifParam->format = gifFmt;
    gifParam->path = recordTempPath(kGifExt[gifFmt]);
    // 「功能设置 → 录制 → 动图帧率」：10 / 15 / 24
    static const int kGifFps[]{ 10, 15, 24 };
    gifParam->fps = (UINT)kGifFps[std::clamp(Setting::get()->getGifFps(), 0, 2)];
    // 「动图分辨率」：480P / 720P / 1080P，只缩不放
    static const int kGifMaxH[]{ 480, 720, 1080 };
    const int gifMaxH = kGifMaxH[std::clamp(Setting::get()->getGifQuality(), 0, 2)];
    if (gifParam->h > gifMaxH) {
        const double f = (double)gifMaxH / gifParam->h;
        gifParam->outW = std::max(2, (int)(gifParam->w * f + 0.5) & ~1);
        gifParam->outH = gifMaxH & ~1;
        if (gifParam->outW > gifParam->w) gifParam->outW = gifParam->w;
        if (gifParam->outH > gifParam->h) gifParam->outH = gifParam->h;
    }
    captureThread = std::jthread([this](std::stop_token st) {
        VideoGif::createGif(gifParam.get());
    });
}

// 录制中选区被挪动：把「当前裁剪区左上角（虚拟桌面坐标）」交给采集线程。
// 采集线程据此平移裁剪框，落到别的显示器上还会换采集目标（见 VideoMp4 里的换屏跟随）。
// 只报原点、不报尺寸 —— 录制期间选区尺寸锁死，采集侧的输出分辨率在开录时也定死了。
void CapVideo::syncCaptureMove()
{
    auto& cm = win->cutMask;   // unique_ptr，取引用
    if (!cm) return;
    const long rawX = (long)std::lround((float)win->x + cm->maskRect.left);
    const long rawY = (long)std::lround((float)win->y + cm->maskRect.top);
    if (mp4Param) {
        // 与 startMp4 同一套取整口径（左上角往里取整），跟随开始那一下画面才不会整体跳几像素
        const long x = (rawX + 3) & ~3l;
        const long y = (rawY + 1) & ~1l;
        // 打包成一个原子值（高 32 位 x / 低 32 位 y）：跨线程读，不能分两次取
        const long long packed = ((long long)(int)x << 32) | (long long)(unsigned int)(int)y;
        mp4Param->wantOrigin.store(packed, std::memory_order_relaxed);
        mp4Param->followOn.store(true, std::memory_order_relaxed);
    }
    if (gifParam) {
        // 动图这条是 GDI 抓屏，天然支持整块虚拟桌面：直接给相对开录原点的平移量即可
        gifParam->followX.store((int)(rawX - capBaseX), std::memory_order_relaxed);
        gifParam->followY.store((int)(rawY - capBaseY), std::memory_order_relaxed);
    }
}

bool CapVideo::onSaveKey(bool toClipboard)
{
    return tool ? tool->onSaveKey(toClipboard) : false;
}

std::wstring CapVideo::stop(bool hideHost)
{
    if (!mp4Param && !gifParam) return L"";
    // 遮罩也别留在屏幕上，录完这一帧就该收工了（演示画布内录屏时不能隐藏宿主）
    if (hideHost) win->hide();
    std::wstring filePath;
    if (mp4Param) {
        mp4Param->MustEnd = true;
        filePath = mp4Param->f;
    }
    if (gifParam) {
        gifParam->isFinish = true;
        filePath = gifParam->path;
    }
    if (captureThread.joinable()) {
        captureThread.join();
    }
    // 刚开录就点了停止，一帧都没采到 —— 这时 Finalize 出来的是个 0 字节的空壳。
    // 别拿它去走存盘流程（那会白弹一次保存框，存出来还是个打不开的文件），
    // 直接删掉、回空路径，让整个流程无声收场
    if (mp4Param && mp4Param->framesWritten == 0) {
        DeleteFile(filePath.data());
        filePath.clear();
    }
    // 参数置空即"已停止"，stopIfRecording / dispose 再进来时不会重复 join
    mp4Param.reset();
    gifParam.reset();
    return filePath;
}
