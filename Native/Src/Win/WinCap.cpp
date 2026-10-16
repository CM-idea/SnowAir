#include "pch.h"
#include <include/Ling.h>
#include "WinCap.h"
#include "WinPin.h"
#include "CutMask.h"
#include "../App.h"
#include "../Util.h"
#include "../Lang.h"
#include "../Update.h"
#include "CapLong.h"
#include "CapVideo.h"
#include "../Tool/ToolCap.h"
#include "../Tool/ToolSub.h"
#include "../Tool/ToolQrcode.h"
#include "../Tool/ToolOcr.h"
#include "../Ocr/OcrService.h"
#include "../Translate/TranslateService.h"
#include "../Translate/BergamotEngine.h"
#include "../CommandKeys.h"
#include "../Tool/ToolVideo.h"
#include "../Tool/ToolLong.h"
#include "HotkeyHook.h"
#include "../History.h"
#include "../Shape/ShapeText.h"
#include "../Tool/ToolbarTheme.h"
#include "../Tool/ToolbarChrome.h"
#include "CaptureEsc.h"
#include "CaptureShield.h"
#include "../Tip.h"
#include "../Setting.h"
#include "../App.h"
#include "../Lang.h"
#include "../Translate/TranslateTypes.h"
#include <sstream>
#include <algorithm>
#include <thread>
using namespace Microsoft::WRL;

namespace
{
	// 放大镜：120 画布 / 9 倍 / 绿十字 / 坐标+色值
	constexpr float kCanvas{ 120.f };
	constexpr float kScale{ 9.f };
	constexpr float kCursorGap{ 12.f };   // 外壳与光标的间距（不紧贴十字准星）
	constexpr float kPad{ 4.f };
	constexpr float kMetaGap{ 6.f };
	constexpr float kRowH{ 18.f };
	constexpr float kHintH{ 16.f };
	constexpr float kShellW{ kCanvas + kPad * 2.f };
	constexpr float kShellH{ kPad + kCanvas + kMetaGap + kRowH * 2.f + 4.f + kHintH + kPad };

	// OCR 临时缓存：识别是同步的（几百毫秒起），同一块选区反复点的时候没必要重跑。
	// 只在同一次截图会话里有效（重开截图 / 换了选区都作废）。
	OcrResult g_ocrCache;
	D2D1_RECT_F g_ocrCacheRect{};
	bool g_ocrCacheValid{ false };
	// 译文缓存：选区没动 + 识别出来的文字一致 + 翻译服务/方向一致 → 直接复用上次译文。
	// 只在 UI 线程读写（后台线程通过回投的 lambda 调用 WinCap::storeTranslateCache）。
	TranslateResult g_trCache;
	D2D1_RECT_F g_trCacheRect{};
	std::wstring g_trCacheKey;
	std::wstring g_trCacheText;
	bool g_trCacheValid{ false };

	// 译文缓存的"设置键"：换服务 / 换翻译方向 / 换识别引擎，缓存都要作废
	std::wstring translateCacheKey(const TranslateRequest& req)
	{
		return req.provider + L"|" + req.sourceLang + L"|" + req.targetLang + L"|"
			+ std::to_wstring(Setting::get()->getOcrEngine());
	}

	// 「显示光标」：会话开始那一刻鼠标下面那个**真实窗口**正在用的光标。
	// 不能只靠 GetCursorInfo —— 上一次截图退出时我们的框选十字光标（IDC_CROSS）可能还残留在
	// 全局光标状态里（鼠标不动就不会被刷新），于是这次会抓到"我们自己的截图框选光标"。
	// 趁还没有任何本程序窗口遮挡鼠标，先给鼠标下的窗口补发一次 WM_SETCURSOR 让它把光标设回来，
	// 再读 GetCursor()，交 captureCursorSnapshot 取用。
	HCURSOR g_cursorForCapture{ nullptr };

	void primeCursorForCapture()
	{
		POINT p{};
		if (!GetCursorPos(&p)) return;
		if (HWND under = WindowFromPoint(p)) {
			DWORD_PTR res = 0;
			SendMessageTimeoutW(under, WM_SETCURSOR, (WPARAM)under,
				MAKELPARAM(HTCLIENT, WM_MOUSEMOVE), SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &res);
		}
		g_cursorForCapture = GetCursor();
	}

	bool sameOcrRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
	{
		return std::abs(a.left - b.left) < 0.5f && std::abs(a.top - b.top) < 0.5f
			&& std::abs(a.right - b.right) < 0.5f && std::abs(a.bottom - b.bottom) < 0.5f;
	}

	// 「录制中显示按键」：低级键盘钩子。由装了消息泵的 UI 线程安装，回调也在该线程执行，
	// 所以可以直接回调 WinCap，不需要跨线程投递。
	HHOOK g_keyHook{ nullptr };

	std::wstring keyComboText(DWORD vk)
	{
		// 纯修饰键不单独显示（否则按住 Ctrl 时会先冒一个"Ctrl +"）
		switch (vk) {
		case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
		case VK_LSHIFT:   case VK_RSHIFT:   case VK_SHIFT:
		case VK_LMENU:    case VK_RMENU:    case VK_MENU:
		case VK_LWIN:     case VK_RWIN:
			return L"";
		default: break;
		}
		std::wstring s;
		if (GetAsyncKeyState(VK_CONTROL) & 0x8000) s += L"Ctrl + ";
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000) s += L"Shift + ";
		if (GetAsyncKeyState(VK_MENU) & 0x8000) s += L"Alt + ";
		if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) s += L"Win + ";
		UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
		LONG lp = (LONG)(sc << 16);
		wchar_t nameW[64]{};
		if (GetKeyNameTextW(lp, nameW, 64) > 0) {
			s += nameW;
			return s;
		}
		s += (wchar_t)vk;
		return s;
	}

	LRESULT CALLBACK keyHookProc(int code, WPARAM wp, LPARAM lp)
	{
		if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
			auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
			if (kb && !(kb->flags & LLKHF_INJECTED)) {
				std::wstring t = keyComboText(kb->vkCode);
				if (!t.empty()) {
					if (auto* cap = WinCap::get()) cap->onGlobalKeyCombo(t);
				}
			}
		}
		return CallNextHookEx(g_keyHook, code, wp, lp);
	}

	// 「录制中显示按键」也要能反映鼠标操作（做教程时点按比键盘更需要被看见）
	HHOOK g_mouseHook{ nullptr };

	LRESULT CALLBACK mouseHookProc(int code, WPARAM wp, LPARAM lp)
	{
		if (code == HC_ACTION) {
			auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
			if (ms && !(ms->flags & LLMHF_INJECTED)) {
				const wchar_t* t = nullptr;
				switch (wp) {
				case WM_LBUTTONDOWN: t = L"鼠标左键"; break;
				case WM_RBUTTONDOWN: t = L"鼠标右键"; break;
				case WM_MBUTTONDOWN: t = L"鼠标中键"; break;
				case WM_MOUSEWHEEL:  t = ((short)HIWORD(ms->mouseData) > 0) ? L"滚轮 ↑" : L"滚轮 ↓"; break;
				default: break;
				}
				if (t) {
					if (auto* cap = WinCap::get()) cap->onGlobalKeyCombo(t);
				}
			}
		}
		return CallNextHookEx(g_mouseHook, code, wp, lp);
	}
}

std::unique_ptr<WinCap> winCap;

WinCap::WinCap() : AnnotHost()
{
	setTitle(L"SnowAir 轻雪");
	// 覆盖窗还没建，此刻的前台窗口就是用户当时正在用的 App —— 留给 {{FOCUS_WINDOW_APP_NAME}}
	focusAppName_ = Util::foregroundAppName();
    auto [x1, y1, w1, h1] = App::get()->getScreenArea();
	this->x = x1;this->y = y1;this->w = (float)w1;this->h = (float)h1;
    onMouseDown.add([this](POINT pos, bool isRight) { this->onDown(pos, isRight); });
    onMouseMove.add([this](POINT pos) { this->onMove(pos); });
    onMouseUp.add([this](POINT pos, bool isRight) { this->onUp(pos, isRight); });
    onKeyDown.add([this](UINT key) { this->onKey(key); });
    onTimer.add([this](UINT id) {
		annotTimer(id);
		if (capLong) capLong->onTimerCB(id);
		if (cutMask) cutMask->onHoverDwellTimer(id);
		if (cutMask) cutMask->onHoverAnimTick(id);
		if (id == keyOverlayTimerId) onKeyOverlayTimer();
		if (id == recCursorTimerId) { updateSelFollowCursor(); updateMouseHighlight(); }
	});
    onMouseWheel.add([this](POINT pos, float space) { this->onWheel(pos, space); });
    onDestroy.add([this]() { this->onClosed(); });
    // DPI 变了（用户改了缩放比例）：系统会按新旧缩放比把窗口整体缩放一圈，但本窗口是铺满整个
    // 虚拟桌面的，缩放之后就盖不住桌面了，而且底图、选区（cutMask->maskRect）用的都是物理像素，
    // 窗口一变形它们全部错位，挂在选区上的工具条自然也跟着偏。改缩放不会改分辨率，
    // 桌面还是那么多像素，所以等系统把建议矩形应用完（紧随而来的 WM_SIZE）再把窗口掰回桌面大小。
    // 位置不能在 onDpiChanged 里改 —— 那个事件在建议矩形生效之前触发，改了马上被覆盖
    onDpiChanged.add([this]() { dpiChanged = true; });
    onSizeChanged.add([this]() {
        if (!dpiChanged) return;
        dpiChanged = false;
        auto [x1, y1, w1, h1] = App::get()->getScreenArea();
        this->x = x1; this->y = y1; this->w = (float)w1; this->h = (float)h1;
        SetWindowPos(hwnd, nullptr, x1, y1, (int)w1, (int)h1, SWP_NOZORDER | SWP_NOACTIVATE);
        relayoutTool();
    });
}

WinCap::~WinCap()
{
}

// 进入截图会话。
// 【为什么要拆两段】建窗 + 整屏抓屏（GDI BitBlt+GetDIBits）在整屏分辨率下要几十~几百毫秒；
// 如果整段都发生在热键回调里，系统会认为"应用正忙"，鼠标上会短暂闪一下"忙"光标。
// 这里把抓屏丢给工作线程、建窗推给调度队列的下一轮：热键回调立即返回、应用回到空闲态，
// 抓屏完成后再回 UI 线程建窗（抓屏发生在覆盖窗 show() 之前，底图不会带上本窗口）。
// 【盾牌也要晚铺】盾牌是置顶、可收鼠标的窗口；若在抓屏前就出现，会顶掉光标下目标窗口的悬停态
// （网页 tooltip / 菜单高亮等"要停悬才出现"的效果），抓到的底图就缺这些效果。故先抓屏、抓完再
// 铺盾牌 —— 盾牌仍覆盖"抓屏后到覆盖窗 show() 之间"的建窗空窗，拖动照旧不会漏给下层。
void WinCap::init(StartMode mode)
{
    // 双击托盘图标会连着来两下，已经开着就不再建第二个
    if (winCap) return;

    // 抢在铺盾牌 / 建覆盖窗之前，抓一次"鼠标下真实窗口此刻的光标"（见 primeCursorForCapture）
    primeCursorForCapture();

    auto* app = Ling::App::get();
    if (!app) {   // 无调度队列：退回同步（正常不会走到）
        CaptureShield::arm();
        initNow(mode, nullptr);
        return;
    }
    const auto [sx, sy, sw, sh] = App::get()->getScreenArea();
    auto grab = std::make_shared<std::vector<BYTE>>();
    std::thread([grab, sx, sy, sw, sh, app, mode]() {
        // 演示画布不用底图（实时桌面），省掉这次抓屏
        if (mode != StartMode::Demo) *grab = Util::captureScreen(sx, sy, sw, sh);
        app->dq.TryEnqueue([grab, mode]() {
            CaptureShield::arm();   // 抓屏完成后再铺盾牌：不遮挡目标窗口的悬停态
            WinCap::initNow(mode, grab);
        });
    }).detach();
}

void WinCap::initNow(StartMode mode, std::shared_ptr<std::vector<BYTE>> grab)
{
    if (winCap) return;   // 抓屏这段空窗里已经开过了
    auto ptr = new WinCap();
    winCap.reset(ptr);
    ptr->cutMask = std::make_unique<CutMask>(ptr);
    // 选区挪动 → 通知录制侧把裁剪框一起平移（拖拽/鼠标跟随/方向键/滚轮全走 CutMask 这一处）
    ptr->cutMask->onRectMoved = [p = ptr]() { p->onMaskRectChanged(); };
    ptr->pendingGrab_ = std::move(grab);
    if (mode == StartMode::Demo) {
        // 演示画布：不画静帧（实时桌面）、不压暗/不描边/不画控点
        ptr->demoMode = true;
        ptr->hideScreenImg = true;
        ptr->stage = CapStage::Adjust;
        // 标注引擎要求 hasRect() 才给画；选区铺满整屏，但一个像素都不画出来
        ptr->cutMask->maskRect = D2D1::RectF(0.f, 0.f, (float)ptr->w, (float)ptr->h);
        ptr->cutMask->showDim = false;
        ptr->cutMask->showBorder = false;
        ptr->cutMask->showHandles = false;
        ptr->cutMask->hideLabel = true;
    }
    ptr->createNativeWindow(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP);
    if (mode == StartMode::Demo) {
        ptr->refresh();
        ptr->makeDemoToolCap();
        // 「功能设置 → 演示 → 默认工具」：0 渐隐画笔（默认）/ 1 画笔 / 2 无
        const int tool = Setting::get()->getDemoDefaultTool();
        if (tool != 2) ptr->startPenSlot(tool == 0);
    }
    else if (mode == StartMode::Record) {
        ptr->pendingRecord = true;
        // 从托盘「屏幕录制」进来的：框选阶段起就按单块屏约束（框完直接开录，不能跨屏）
        if (ptr->cutMask) ptr->cutMask->setSingleMonitorMode(true);
    }
}

WinCap* WinCap::get()
{
    return winCap.get();
}

// 托盘「屏幕录制」：照常进截图态（找元素 / 框选），框完由 onUp 直接切到录屏
void WinCap::beginRecordPick()
{
    init(StartMode::Record);
}

// 托盘「演示画布 / 演示模式」：
//   · 画面是**实时的**：不画会话开始时抓的静帧，桌面直接透出来（只留标注）
//   · **没有框选**：不压暗、不描边、不画控点、不画尺寸标签，整屏就是画布
//   · 工具栏按 Fullscreen 布局，默认摆在屏幕顶部居中（拖动/贴边逻辑后续再补）
void WinCap::beginDemoCanvas()
{
    init(StartMode::Demo);
    // 画布起来后把「鼠标穿透」「隐藏/显示工具栏」挂到全局钩子上（穿透之后焦点就不在本软件了）
    syncGlobalCommandKeys(CommandKeys::SurfaceFullscreen);
}

void WinCap::makeDemoToolCap()
{
    if (!toolCap) {
        clearMainToolUserPlaced();
        toolCap = std::make_unique<ToolCap>(this);
        toolCap->applyDemoLayout();            // 必须在 createNativeWindow 之前换槽位表
        toolCap->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
        toolCap->bindOwner(hwnd);
    }
    layoutTool(toolCap.get());
}

// 「渐隐画笔」/「画笔」：工具栏上并列的一级槽位，底层工具都是 pen，区别只在 isPenFade。
// curId 用槽位 id（laser / pen），标注引擎经 resolveToolId 拿 pen。
// isPenFade 的设定放在 ToolCap::selectAnnotTool → ToolSub::setPenSlotMode，
// 这样截图栏 / 贴图栏 / 演示栏三条路径同一套逻辑（必须在建属性栏之前定好）。
void WinCap::startPenSlot(bool fade)
{
    ensureHistory();                           // 先有 toolSub，isPenFade 才能在建属性栏前定好
    // selectAnnotTool 会按槽位做排他高亮（旧的取消、新的选中）并把 curId 设为该槽位
    startAnnotate(fade ? L"laser" : L"pen");
    // 与旧路径一致：工具样式变了要通知一次（选中的滤镜/文字等按新属性重刷）
    if (toolSub) toolSub->notifyStyleChanged();
}

void WinCap::toggleDemoThrough()
{
    if (!demoMode) return;
    setMouseTransparent(!isMouseTransparent);
    if (toolCap) toolCap->setDemoThroughState(isMouseTransparent);
    refresh();
}

void WinCap::resetDemoCanvas()
{
    // 走 clearAllShapes：先断开 shapeHover/editingText 再释放，否则下一个工具栏点击
    // （onClick 里要读 hasSelectedAnnot）就会在已释放对象上做虚调用 → 闪退
    clearAllShapes();
    newShape = nullptr;
    refresh();
}

void WinCap::exitDemoCanvas()
{
    // 穿透状态下退出：先把命中样式恢复回来，再按安全顺序清标注并关窗
    setMouseTransparent(false);
    clearAllShapes();
    close();
}

void WinCap::selectFullScreenRect()
{
    if (!cutMask) return;
    cutMask->maskRect = D2D1::RectF(0.f, 0.f, (float)w, (float)h);
    cutMask->refreshLabel();
    stage = CapStage::Adjust;
    refresh();
    makeToolCap();
}

void WinCap::dispose()
{
    // 同 WinSetting::dispose：~WinBase 不销毁 HWND，退出前必须先关窗，否则残留空白窗
    if (winCap && winCap->hwnd) winCap->close();
    winCap.reset();
}

void WinCap::onCreated()
{
    // 新一次截图会话：底图换了，OCR / 译文临时缓存一并作废
    g_ocrCacheValid = false;
    g_trCacheValid = false;
    // 底图：优先用工作线程抓好的 BGRA（init 里已抓完，UI 线程只做 CreateBitmap）；
    // 失败/未就绪则兜底同步抓一次。演示画布不画静帧，直接跳过。
    if (pendingGrab_ && !pendingGrab_->empty()) {
        App::createBitmapFromBGRA((int)w, (int)h, *pendingGrab_, &screenImg);
    }
    else if (!hideScreenImg) {
        App::get()->takeScreenShot((int)x, (int)y, (int)w, (int)h, &screenImg);
    }
    pendingGrab_.reset();
    // 「显示光标」快照：必须在窗口显示前抓，否则抓到的是覆盖窗自己的光标
    captureCursorSnapshot();
	auto d2d = Ling::D2D::get();
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免调整选区时整帧闪烁
    canvas = body->makeChild<Ling::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    tip = std::make_unique<Tip>(this);
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.75f), brushMuted.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.45f), brushHint.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1E1E1F), brushBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0, 0.f, 0.f, 0.24f), brushShadow.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x34C759), crossBrush.GetAddressOf());
    POINT pos;
    GetCursorPos(&pos);
    ScreenToClient(hwnd, &pos);
    setPixPos(pos);
    show();
    CaptureEsc::claim(hwnd);
    // 窗口已经能收鼠标了：把空窗期被盾牌吃掉的那一下按下补进来（还按着就接着拖）
    CaptureShield::drain(hwnd);
}

void WinCap::layout()
{
    Ling::WinBase::layout();
    if (!canvas) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, (float)w, (float)h);
    if (!hideScreenImg) {
        ctx->DrawBitmap(screenImg.Get(), destRect);
    }
    // 标注在底图上、暗色遮罩下：选区外被压暗，洞内正常显示（与「全屏纸+导出裁切」一致）
    // 长图选区大图由 CapLong 后画，标注需叠在其上
    if (annotLive || (stage == CapStage::Video && hasAnnotShapes())) {
        if (stage != CapStage::Long) paintShapes(ctx);
    }
    if (annotLive && (sizePreviewHold || GetTickCount64() < sizePreviewPulseUntil))
		paintSizePreview(ctx);
    // 查找元素悬停：仅描边；已框选/标注中：截图选区控点一直显示
    // 演示模式不画遮罩的任何部件（不压暗、不描边、不控点、不标签）—— 整屏就是画布
    if (cutMask && !demoMode) {
        const bool findHover = (stage == CapStage::Select && !isPress);
        // 录屏相关阶段不压暗：框选录制区域时要能看清整屏（不然选到屏幕上的什么都看不准），
        // 录起来之后选区外也保持原样。pendingRecord = 托盘「屏幕录制」进来、还没框完这一段。
        const bool recording = pendingRecord || stage == CapStage::Video;
        cutMask->showDim = !findHover && !recording;
        // 拖选区时不画控点，少一圈 FillRectangle（对齐跟手）
        const bool editingSel = isPress && (stage == CapStage::Adjust || maskDragging
            || videoCanAdjust() || (stage == CapStage::Select));
		const bool longEdit = stage == CapStage::Long && capLong && capLong->isEditMode();
		// 长图编辑：对齐贴图——保留边框，隐藏四周调选区控点
        cutMask->showHandles = !editingSel && !longEdit
			&& (stage == CapStage::Adjust || stage == CapStage::Long || videoCanAdjust());
		cutMask->showBorder = true;
        cutMask->drawQrcodeFill = (toolQrcode != nullptr);
        cutMask->paint(ctx);
    }
    if (capLong) capLong->paint(ctx);
    if (stage == CapStage::Long && (annotLive || hasAnnotShapes()) && capLong) {
        D2D1_MATRIX_3X2_F oldXform{};
        ctx->GetTransform(&oldXform);
		if (capLong->beginLongPreviewAnnotPaint(ctx, oldXform)) {
			paintShapes(ctx, false);
			capLong->endLongAnnotPaint(ctx, oldXform);
		}
        if (capLong->beginLongAnnotPaint(ctx, oldXform)) {
            paintShapes(ctx);
            capLong->endLongAnnotPaint(ctx, oldXform);
        }
    }
    paintPix(ctx);
    paintMouseHighlight(ctx);
    paintCursor(ctx);
    paintKeyOverlay(ctx);
    canvas->finishPaint();
    // 识别浮层里的"标注镜像"跟着本窗口一起重画（标注一变就同步，避免两个窗口错帧）
    if (toolOcr) toolOcr->repaintOverlay();
}

void WinCap::applyMaskHitCursor(MaskHit hit)
{
	switch (hit) {
	case MaskHit::TopLeft:
	case MaskHit::BottomRight:
		SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
		break;
	case MaskHit::TopRight:
	case MaskHit::BottomLeft:
		SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
		break;
	case MaskHit::Top:
	case MaskHit::Bottom:
		SetCursor(LoadCursor(nullptr, IDC_SIZENS));
		break;
	case MaskHit::Left:
	case MaskHit::Right:
		SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
		break;
	case MaskHit::Inside:
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		break;
	default:
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		break;
	}
}

bool WinCap::hasAnnotShapes() const
{
	if (!history) return false;
	for (auto& s : history->shapes) {
		if (s && !s->isUndo) return true;
	}
	return false;
}

bool WinCap::beginMaskResize(POINT pos)
{
	if (!cutMask || !cutMask->hasRect()) return false;
	// 演示画布没有"选区"这个概念（铺满整屏、不描边不压暗）：不许拖/调边，
	// 否则一拖就把看不见的选区挪走、还会 setToolChromeHidden 把工具条藏掉
	if (demoMode) return false;
	const auto hit = cutMask->hitTest(pos);
	if (hit == MaskHit::None) return false;
	// 画幅已锁定（录屏开录 / 长图滚动）时：一律不许拖边、拖角。
	// 长图连整体挪动也禁止；录屏保留整体挪动，但要求未开鼠标穿透。
	const bool sizeLocked = maskSizeLocked();
	if (sizeLocked) {
		if (maskMoveLocked()) return false;
		if (hit != MaskHit::Inside || isMouseTransparent) return false;
	}
	if (hit == MaskHit::Inside) {
		// 区内平移：要求未选中标注工具；截图与长图还要求未绘制标注，录屏开录后不作要求
		if (!getCurToolId().empty()) return false;
		if (!sizeLocked && hasAnnotShapes()) return false;
	}
	maskDragging = true;
	isPress = true;
	SetCapture(hwnd);
	cutMask->startAdjust(pos);
	setPixPos(pos);
	setToolChromeHidden(true);
	refresh();
	return true;
}

void WinCap::setToolChromeHidden(bool hidden)
{
	// 按当前阶段只显隐「自己的」工具栏，截图栏与录屏栏互不串台
	if (stage == CapStage::Video) {
		if (capVideo) capVideo->setToolVisible(!hidden);
		return;
	}
	if (stage == CapStage::Long) {
		if (capLong) capLong->setToolVisible(!hidden);
		return;
	}
	if (hidden) {
		if (toolCap) {
			toolCap->hideHoverTip();
			toolCap->hide();
		}
		if (toolSub) toolSub->suspendChrome();
		return;
	}
	if (toolCap) toolCap->show();
	layoutTools();
	if (toolSub && toolSub->hasContent()) {
		RECT winRect{ x, y, x + (int)w, y + (int)h };
		toolSub->resumeChrome(ToolbarChrome::workAreaNear(winRect));
	}
}

BOOL WinCap::setCursor()
{
	POINT pos{};
	GetCursorPos(&pos);
	ScreenToClient(hwnd, &pos);

	if (annotLive) {
		if (stage == CapStage::Long && capLong
			&& (capLong->isPreviewPanning() || capLong->isSelViewportPanning()
				|| capLong->hitPreview(pos)
				|| (capLong->isEditMode() && !shapeHover
					&& capLong->hitSelViewport(pos)
					&& (getCurToolId().empty() || !hitShapeAt(
						(float)toImgPos(pos).x, (float)toImgPos(pos).y))))) {
			// 未选中标注：选区内示意可拖浏览（有工具且点在图形上时除外）
			if (capLong->isEditMode() && getCurToolId().empty()
				&& capLong->hitSelViewport(pos)) {
				capLong->setCursor();
				return TRUE;
			}
			if (capLong->isPreviewPanning() || capLong->isSelViewportPanning()
				|| capLong->hitPreview(pos)) {
				capLong->setCursor();
				return TRUE;
			}
		}
		if (maskDragging && cutMask) {
			const auto hit = cutMask->adjustingHit();
			applyMaskHitCursor(hit != MaskHit::None ? hit : cutMask->hitTest(pos));
			return TRUE;
		}
		const auto img = toImgPos(pos);
		const float ix = (float)img.x, iy = (float)img.y;

		// 1) 已选中标注：控点缩放 / 平移（优先于选区边）
		if (shapeHover && !shapeHover->isUndo && shapeHover != newShape
			&& !shapeHover->isViewportFilter()) {
			if (annotMouseDown) {
				shapeHover->setCursor();
				return TRUE;
			}
			shapeHover->mouseMove(ix, iy);
			if (shapeHover->hoverDraggerIndex >= 0) {
				shapeHover->setCursor();
				return TRUE;
			}
		}

		// 2) 截图选区边/角：悬停即 ↔ ↕（不必等到拖动）
		// 长图编辑态选区已定死，不提供调边光标；演示画布没有可调选区
		const bool longEdit = stage == CapStage::Long && capLong && capLong->isEditMode();
		// 画幅锁定后不再给缩放光标（避免可拖动的误导）；区内可整体挪动时给四向光标。
		// 这两项须与 beginMaskResize 的两道闸门一一对应，否则光标与实际可操作性不符。
		const bool selResizable = !maskSizeLocked();
		const bool selMovable = !maskMoveLocked() && getCurToolId().empty() && !isMouseTransparent
			&& (maskSizeLocked() || !hasAnnotShapes());
		if (!demoMode && cutMask && cutMask->hasRect() && !longEdit) {
			if (cutMask->hitInfoControl(pos) != InfoHit::None) {
				SetCursor(LoadCursor(nullptr, IDC_HAND));
				return TRUE;
			}
			const auto hit = cutMask->hitTest(pos);
			if (hit != MaskHit::None && hit != MaskHit::Inside && selResizable) {
				applyMaskHitCursor(hit);
				return TRUE;
			}
			if (hit == MaskHit::Inside && !selResizable && selMovable) {
				// 开录后只剩"整体挪动"这一种选区操作，光标给足提示
				SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
				return TRUE;
			}
		}
		// 长图编辑且未选中标注：空白处可拖浏览
		if (longEdit && !shapeHover && getCurToolId().empty()
			&& capLong->hitSelViewport(pos)) {
			capLong->setCursor();
			return TRUE;
		}

		// 3) 未选中标注悬停手型 / 工具光标 / 无标注时区内平移
		applyAnnotCursor();
		return TRUE;
	}
	// 演示画布：没开标注会话时画布不可交互，光标固定普通箭头
	if (demoMode) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return TRUE;
	}
	if (stage == CapStage::Select) {
		SetCursor(LoadCursor(nullptr, IDC_CROSS));
		return TRUE;
	}
	if (stage == CapStage::Long && capLong) {
		capLong->setCursor();
		return TRUE;
	}
	if (stage == CapStage::Adjust && cutMask) {
		if (!isPress && cutMask->hitInfoControl(pos) != InfoHit::None) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		if (isPress) {
			const auto hit = cutMask->adjustingHit();
			applyMaskHitCursor(hit != MaskHit::None ? hit : cutMask->hitTest(pos));
		}
		else {
			applyMaskHitCursor(cutMask->hitTest(pos));
		}
		return TRUE;
	}
	if (videoCanAdjust() && cutMask) {
		if (!isPress && cutMask->hitInfoControl(pos) != InfoHit::None) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		if (isPress) {
			const auto hit = cutMask->adjustingHit();
			applyMaskHitCursor(hit != MaskHit::None ? hit : cutMask->hitTest(pos));
		}
		else {
			applyMaskHitCursor(cutMask->hitTest(pos));
		}
		return TRUE;
	}
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
	return TRUE;
}

void WinCap::applyEmptyToolCursor()
{
	// 演示画布没选中工具 = 什么都不做：不该给出"可平移选区"的移动光标
	//（全屏画布"无工具时不拖选区、不重开框选"）
	if (demoMode) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return;
	}
	POINT pos{};
	GetCursorPos(&pos);
	ScreenToClient(hwnd, &pos);
	if (stage == CapStage::Long && capLong && capLong->isEditMode()
		&& capLong->hitSelViewport(pos)) {
		capLong->setCursor();
		return;
	}
	if (cutMask && cutMask->hasRect()) {
		const auto hit = cutMask->hitTest(pos);
		// 区内：无标注 → 可平移(SizeAll)；**有标注 → 普通箭头**
		//（用户要的：有标注 + 没选工具 = 正常鼠标样式；十字只属于"选了工具"）
		if (hit == MaskHit::Inside) {
			SetCursor(LoadCursor(nullptr, hasAnnotShapes() ? IDC_ARROW : IDC_SIZEALL));
			return;
		}
		if (hit != MaskHit::None) {
			applyMaskHitCursor(hit);
			return;
		}
	}
	// 框还没框 / 点在选区外：保持十字（拖框选、重开选区）
	SetCursor(LoadCursor(nullptr, IDC_CROSS));
}

void WinCap::setPixPos(POINT pos)
{
	// 外壳贴在鼠标**右下角**，与光标留 12 逻辑像素间距
	//（不紧贴十字准星），越界时夹回窗口内。pixFocus 仍是取样点（= 光标处）。
	const float gap = kCursorGap * dpi;
	const float shellW = kShellW * dpi;
	const float shellH = kShellH * dpi;
	pixPos.x = (LONG)std::clamp((float)pos.x + gap, 0.f, std::max(0.f, w - shellW));
	pixPos.y = (LONG)std::clamp((float)pos.y + gap, 0.f, std::max(0.f, h - shellH));
	pixFocus = pos;
}

void WinCap::samplePixColor()
{
	if (!screenImg) return;
	const int px = (int)std::clamp(pixFocus.x, 0L, (LONG)std::max(0.f, w - 1.f));
	const int py = (int)std::clamp(pixFocus.y, 0L, (LONG)std::max(0.f, h - 1.f));
	if (px == pixSampleX_ && py == pixSampleY_) return;
	auto d2d = Ling::D2D::get();
	if (!pixSampleBmp_) {
		D2D1_BITMAP_PROPERTIES1 prop{};
		// 必须与底图同格式：原先写死 PREMULTIPLIED，与底图(IGNORE)不一致会令
		// CopyFromBitmap 失败 → 色值恒为黑（"无法识别颜色"的根因）。
		prop.pixelFormat = screenImg->GetPixelFormat();
		prop.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
		screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
		d2d->deviceContext->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, &prop, pixSampleBmp_.GetAddressOf());
	}
	if (!pixSampleBmp_) return;
	D2D1_POINT_2U dest{ 0, 0 };
	D2D1_RECT_U src{ (UINT)px, (UINT)py, (UINT)px + 1, (UINT)py + 1 };
	if (FAILED(pixSampleBmp_->CopyFromBitmap(&dest, screenImg.Get(), &src))) return;
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(pixSampleBmp_->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return;
	const BYTE* p = mapped.bits;
	pixColor_ = RGB(p[2], p[1], p[0]);
	pixSampleBmp_->Unmap();
	// 只有真正读到值才记缓存：失败时下帧重试，不会被 -1/旧坐标误跳过
	pixSampleX_ = px;
	pixSampleY_ = py;
}

void WinCap::paintPix(ID2D1DeviceContext* ctx)
{
	// 放大镜显示时机：框选真正拖动 / 调四边四角 / **查找元素悬停**（Select 阶段未按下）
	if (!screenImg || !cutMask) return;
	const bool framing = (stage == CapStage::Select && isPress && selectDragging_);
	const bool resizing = cutMask->isResizingEdge()
		&& (maskDragging || ((stage == CapStage::Adjust || videoCanAdjust()) && isPress));
	// 查找元素阶段（未按下时悬停画布）：跟手显示放大镜
	const bool elementHover = stage == CapStage::Select && !isPress && !demoMode
		&& Setting::get()->getFindWindowElements();
	if (!framing && !resizing && !elementHover) return;

	const float pad = kPad * dpi;
	const float canvas = kCanvas * dpi;
	const float shellW = kShellW * dpi;
	const float shellH = kShellH * dpi;
	const float x0 = (float)pixPos.x, y0 = (float)pixPos.y;
	const float radius = 6.f * dpi;

	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0 + dpi, x0 + shellW, y0 + shellH + dpi), radius, radius), brushShadow.Get());
	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x0 + shellW, y0 + shellH), radius, radius), brushBg.Get());

	const D2D1_RECT_F canvasRect{ x0 + pad, y0 + pad, x0 + pad + canvas, y0 + pad + canvas };
	// 轴对齐裁剪代替每帧建圆角 Geometry，拖边更跟手
	ctx->PushAxisAlignedClip(canvasRect, D2D1_ANTIALIAS_MODE_ALIASED);
	ctx->FillRectangle(canvasRect, brushBg.Get());

	const float picker = canvas / kScale;
	const float half = picker * 0.5f;
	D2D1_RECT_F src{
		(float)pixFocus.x - half, (float)pixFocus.y - half,
		(float)pixFocus.x + half, (float)pixFocus.y + half
	};
	ctx->DrawBitmap(screenImg.Get(), canvasRect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src);

	const float cx = canvasRect.left + canvas * 0.5f;
	const float cy = canvasRect.top + canvas * 0.5f;
	const float stroke = 1.5f * dpi;
	ctx->DrawLine(D2D1::Point2F(cx, canvasRect.top), D2D1::Point2F(cx, canvasRect.bottom), crossBrush.Get(), stroke);
	ctx->DrawLine(D2D1::Point2F(canvasRect.left, cy), D2D1::Point2F(canvasRect.right, cy), crossBrush.Get(), stroke);
	ctx->PopAxisAlignedClip();

	// 色值从底图取样（避免 GetPixel 同步读屏拖垮帧率）
	samplePixColor();
	const auto hex = std::format(L"#{:02X}{:02X}{:02X}", GetRValue(pixColor_), GetGValue(pixColor_), GetBValue(pixColor_));
	const auto posText = std::format(L"{}, {}", pixFocus.x, pixFocus.y);

	const float metaLeft = x0 + pad + 2.f * dpi;
	const float metaW = canvas - 4.f * dpi;
	const float rowH = kRowH * dpi;
	const float fontSize = 12.f * dpi;
	const float hintSize = 11.f * dpi;
	float metaY = y0 + pad + canvas + kMetaGap * dpi;
	auto d2d = Ling::D2D::get();

	auto drawRow = [&](float y, const wchar_t* label, const std::wstring& value, ID2D1SolidColorBrush* valueBrush) {
		ComPtr<IDWriteTextLayout> left, right;
		d2d->dwriteFactory->CreateTextLayout(label, (UINT32)wcslen(label), d2d->baseTextFormat.Get(), metaW, rowH, left.GetAddressOf());
		d2d->dwriteFactory->CreateTextLayout(value.data(), (UINT32)value.size(), d2d->baseTextFormat.Get(), metaW, rowH, right.GetAddressOf());
		if (left) {
			left->SetFontSize(fontSize, { 0, UINT32_MAX });
			left->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			ctx->DrawTextLayout({ metaLeft, y }, left.Get(), brushMuted.Get());
		}
		if (right) {
			right->SetFontSize(fontSize, { 0, UINT32_MAX });
			right->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
			right->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			ctx->DrawTextLayout({ metaLeft, y }, right.Get(), valueBrush);
		}
	};
	drawRow(metaY, L"坐标", posText, brushText.Get());
	metaY += rowH + 2.f * dpi;
	drawRow(metaY, L"色值", hex, brushText.Get());
	metaY += rowH + 2.f * dpi;

	// 复制成功后提示行临时变绿显示「色值复制成功！」，1.5s 后自动恢复
	const bool copied = GetTickCount64() < pixCopiedUntil_;
	const wchar_t* hintStr = copied ? L"色值复制成功！" : L"按 Ctrl+C 复制色值";
	ComPtr<IDWriteTextLayout> hint;
	d2d->dwriteFactory->CreateTextLayout(hintStr, (UINT32)wcslen(hintStr), d2d->baseTextFormat.Get(), metaW, kHintH * dpi, hint.GetAddressOf());
	if (hint) {
		hint->SetFontSize(hintSize, { 0, UINT32_MAX });
		hint->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		ctx->DrawTextLayout({ metaLeft, metaY }, hint.Get(), copied ? crossBrush.Get() : brushHint.Get());
	}
}

unsigned WinCap::currentSurface() const
{
	return demoMode ? CommandKeys::SurfaceFullscreen
		: (stage == CapStage::Video ? CommandKeys::SurfaceRecord : CommandKeys::SurfaceCapture);
}

// 「系统按键」派发：按键 → 命令表 → 与工具栏点击同一批 WinCap 入口（不写第二套实现）。
// 「常规」组整组（关闭 / 完成 / 复制 / 存为文件 / 工具循环）也在此接管，语义与原 onKey 的
// 硬编码分支一致；该组在设置页为只读行 —— 不可改绑，但照旧派发。
bool WinCap::commandKey(UINT key)
{
	// 正在编辑标注文字：键盘归输入框，任何命令键都不许抢
	if (editingText) return false;
	const std::wstring id = CommandKeys::actionForKey(key, currentSurface());
	return id.empty() ? false : runCommand(id);
}

// 执行一条命令：窗口内按键（commandKey）与全局钩子（临时全局键）**共用这一段**
bool WinCap::runCommand(const std::wstring& id)
{
	if (editingText) return false;
	// 「删除标注」为只读行，scan 不会返回它，此处再兜一层
	if (id == L"cmd_delete_sel") return false;

	// —— 「常规」组中带阶段差别的四条 ——
	// 语义与原 onKey 的硬编码分支逐条一致（原分支已删除）：演示画布的 Enter 不结束会话、
	// Esc 先退穿透，以及长图与录屏阶段的存图路径都在此分流。
	if (id == L"cmd_close") {
		// 演示画布 + 穿透中：第一次先退出穿透，再按一次才关画布
		if (demoMode && isMouseTransparent) { toggleDemoThrough(); return true; }
		close();
		return true;
	}
	if (id == L"cmd_done") {
		// 画布：Enter 不做"完成复制"（只靠 ✕ / Esc 退出）；标注中 = 复制，否则按当前阶段收尾
		if (annotLive) { if (!demoMode) copyToClipboard(); return true; }
		copyCurrentStage();
		return true;
	}
	if (id == L"cmd_copy") {
		if (annotLive) { copyToClipboard(demoMode); return true; }
		if (stage == CapStage::Long && capLong && capLong->hasImage()) {
			longCopyToClipboard();
			close();
			return true;
		}
		if (stage == CapStage::Video && capVideo) { capVideo->onSaveKey(true); return true; }
		return true;
	}
	if (id == L"cmd_save") {
		if (annotLive) { saveToFile(demoMode); return true; }
		if (stage == CapStage::Long && capLong && capLong->hasImage()) {
			if (longSaveToFile()) close();   // 存失败就别关，让用户看到失败提示
			return true;
		}
		if (stage == CapStage::Video && capVideo) { capVideo->onSaveKey(false); return true; }
		return true;
	}

	// —— 工具栏动作：与"点那颗按钮"调同一批 WinCap 入口 ——
	//（动作 id 口径即工具栏槽位 id，映射见 CommandKeys 的 kActionMap）
	const std::wstring act = CommandKeys::toolbarActionForId(id);
	if (!act.empty()) {
		// 「鼠标穿透」两个工具栏上都有：录屏会话走录屏条那颗（它同时管按钮状态），
		// 全屏画布走演示画布自己的穿透
		if (act == L"mouse-through") {
			ToolVideo* tv = (stage == CapStage::Video && capVideo) ? capVideo->toolBar() : nullptr;
			if (tv) tv->cmdTogglePierce();
			else toggleDemoThrough();
			return true;
		}
		if (act == L"extra") { startVideo(); return true; }
		if (act == L"pin") { startPin(); return true; }
		if (act == L"ocr") { startOcr(); return true; }
		if (act == L"translate") { startTranslate(); return true; }
		if (act == L"long") { startLong(); return true; }
		// 绘图工具：包浆/水印/高亮按子工具直选。
		// 三个阶段各有自己的工具条与入口（录屏 = ToolVideo、长图 = ToolLong、其余 = 截图条），
		// 走各自入口才能同时更新选中态与属性栏锚点。
		const std::wstring sub = CommandKeys::subToolForId(id);
		const std::wstring tool = sub.empty() ? act : sub;
		if (stage == CapStage::Video && capVideo && capVideo->toolBar()) {
			capVideo->toolBar()->cmdAnnotTool(tool);
		}
		else if (stage == CapStage::Long && capLong && capLong->toolBar()) {
			capLong->toolBar()->cmdAnnotTool(tool);
		}
		else {
			startAnnotate(tool);
		}
		return true;
	}

	// —— 录屏组的四条：动作在录屏工具条上（与点按钮同一段实现）——
	// （它们同时是临时全局键，焦点不在本软件时由全局钩子那侧派发，见 HotkeyHook）
	if (stage == CapStage::Video && capVideo) {
		if (auto* tv = capVideo->toolBar()) {
			if (id == L"cmd_record_toggle") { tv->cmdToggleRecord(); return true; }
			if (id == L"cmd_toggle_gif") { tv->cmdToggleGif(); return true; }
			if (id == L"cmd_toggle_system_audio") { tv->cmdToggleSystemAudio(); return true; }
			if (id == L"cmd_toggle_mic") { tv->cmdToggleMic(); return true; }
		}
	}

	// —— 常规动作（工具栏上没有对应按钮的那些）——
	// 「工具循环」：在工具栏的绘图工具之间循环切换一个；同样按阶段作用在当前可见的工具条
	if (id == L"cmd_quick_switch") {
		if (stage == CapStage::Video && capVideo && capVideo->toolBar()) capVideo->toolBar()->cycleAnnotTool();
		else if (stage == CapStage::Long && capLong && capLong->toolBar()) capLong->toolBar()->cycleAnnotTool();
		else if (toolCap) toolCap->cycleTool();
		return true;
	}
	if (id == L"cmd_undo") { if (annotLive && history) history->undo(); return true; }
	if (id == L"cmd_redo") { if (annotLive && history) history->redo(); return true; }
	// 下面两条工具栏上**没有对应按钮**，只有命令键能触发
	if (id == L"cmd_clear") { if (history) history->clearAll(); return true; }
	if (id == L"cmd_toggle_toolbar") {
		toolHidden = !toolHidden;
		if (toolCap) { if (toolHidden) toolCap->hide(); else toolCap->show(); }
		return true;
	}
	if (id == L"cmd_show_cursor") { toggleShowCursor(); return true; }
	if (id == L"cmd_scan_qrcode") { startQrcode(); return true; }
	if (id == L"cmd_scroll_pause") { if (capLong) capLong->togglePaused(); return true; }
	return false;
}

void WinCap::forwardKey(UINT key)
{
	onKey(key);
}

// 会话期间把「系统按键」里 Scope::Global 的那几条**临时**挂到全局钩子上；surface=0 即撤。
// 为什么非挂不可：录屏时焦点在被录的程序上，而鼠标穿透 / 收起工具栏之后焦点也不在本软件上 ——
// 窗口内根本收不到这些键。其余命令键一律**不挂**（免得在任何程序里吞掉用户的键）。
void WinCap::syncGlobalCommandKeys(unsigned surface)
{
	std::vector<HotkeyHook::Binding> list;
	if (surface != 0) {
		for (const auto& gkey : CommandKeys::groupOrder()) {
			for (const auto& id : CommandKeys::idsInGroup(gkey)) {
				if (CommandKeys::scopeOf(id) != CommandKeys::Scope::Global) continue;
				// ⚠ 只挡「未实装」：只读 ≠ 不生效（见 CommandKeys::scan 的说明）
				if (!CommandKeys::implemented(id)) continue;
				if ((CommandKeys::surfaceMaskForId(id) & surface) == 0) continue;
				const std::wstring chord = CommandKeys::binding(id);
				if (chord.empty()) continue;   // 用户清空过 → 不绑
				list.push_back({ chord, [id]() {
					// 现取当前会话实例：钩子回调可能在会话销毁后才到，捕获 this 就是悬空指针
					if (auto* w = WinCap::get()) w->runCommand(id);
				} });
			}
		}
	}
	HotkeyHook::setSessionBindings(std::move(list));
}

void WinCap::onKey(UINT key)
{
    // 方向键：挪选区 / 挪光标（没有对应的命令，所以不走命令表）
    if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
        const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
        const int dx = (key == VK_LEFT) ? -step : (key == VK_RIGHT) ? step : 0;
        const int dy = (key == VK_UP) ? -step : (key == VK_DOWN) ? step : 0;
        if (nudgeSelectionByArrow(dx, dy)) return;
        // 查找元素阶段：方向键挪光标跟手高亮
        POINT pos;
        GetCursorPos(&pos);
        pos.x += dx;
        pos.y += dy;
        SetCursorPos(pos.x, pos.y);
        return;
    }
    // 放大镜可见时 Ctrl+C 为「取色值」，优先于「复制」命令本身。
    // !annotLive 不可省：标注中的 Ctrl+C 归「复制」（其语义已移至 runCommand），否则也会触发取色。
    if (!annotLive && key == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)
        && ((stage == CapStage::Select && isPress && selectDragging_)
            || (stage == CapStage::Select && !isPress && !demoMode
                && Setting::get()->getFindWindowElements())
            || (cutMask && cutMask->isResizingEdge()
                && (maskDragging || ((stage == CapStage::Adjust || videoCanAdjust()) && isPress))))) {
        samplePixColor();
        Ling::Util::setTextToClipboard(std::format(L"#{:02X}{:02X}{:02X}",
            GetRValue(pixColor_), GetGValue(pixColor_), GetBValue(pixColor_)));
        pixCopiedUntil_ = GetTickCount64() + 1500;   // 提示行临时变绿
        refresh();
        return;
    }
    // 「系统按键」：按当前界面查命令表，命中即执行并吃掉。关闭/完成/复制/存为文件/撤销/重做/
    // 清除/绘图工具等均在此表内，键位可在设置页配置（阶段分流见 runCommand）
    if (commandKey(key)) return;
    // 表外的键（Ctrl+Z/Y 兜底、Delete/Backspace 删标注、Esc 取消选中）归标注层
    if (annotLive) annotKey(key);
}

bool WinCap::nudgeSelectionByArrow(int dx, int dy)
{
	if (dx == 0 && dy == 0) return false;
	if (!cutMask || !cutMask->hasRect()) return false;
	// 演示画布没有可挪的选区（全屏画布方向键不微调选区）
	if (demoMode) return false;
	// 长图滚动中：方向键同样不挪选区（与鼠标拖动同一规则，见 maskMoveLocked）
	if (maskMoveLocked()) return false;
	if (stage != CapStage::Adjust && !annotLive && !videoCanAdjust()) return false;
	if (maskDragging || isPress) return false;
	// 已有标注时方向键不挪选区
	if (annotLive && hasAnnotShapes()) return false;
	cutMask->translateBy((float)dx, (float)dy);
	if (annotLive) syncViewportFilters();
	if (annotLive) layoutTools();
	else if (videoCanAdjust() && capVideo) capVideo->layoutTool();
	else if (toolCap) layoutTool(toolCap.get());
	syncQrcodePanel();
	syncOcrPanel();
	refresh();
	return true;
}

bool WinCap::nudgeSelectionByOutsideWheel(POINT pos, int dir)
{
	if (!cutMask || !cutMask->hasRect()) return false;
	if (stage != CapStage::Adjust && !annotLive && !videoCanAdjust()) return false;
	const auto& sel = cutMask->maskRect;
	const float px = (float)pos.x, py = (float)pos.y;
	const bool inColumn = px >= sel.left && px <= sel.right;
	const bool inRow = py >= sel.top && py <= sel.bottom;
	const bool isAbove = py < sel.top;
	const bool isBelow = py > sel.bottom;
	const bool isLeft = px < sel.left;
	const bool isRight = px > sel.right;
	const bool isVerticalSide = (isAbove || isBelow) && inColumn;
	const bool isHorizontalSide = (isLeft || isRight) && inRow;
	if (!isVerticalSide && !isHorizontalSide) return false;

	constexpr float kNudge = 2.f;
	float dx = 0.f, dy = 0.f;
	if (isVerticalSide) {
		const float sideSign = isAbove ? 1.f : -1.f;
		dy = sideSign * (float)dir * kNudge;
	}
	else {
		const float sideSign = isLeft ? 1.f : -1.f;
		dx = sideSign * (float)dir * kNudge;
	}
	if (!cutMask->translateBy(dx, dy)) return true;
	// 跟方向键一样：先跟栏再 refresh，否则只见工具栏动、选区画面不跟
	if (annotLive) syncViewportFilters();
	if (annotLive) layoutTools();
	else if (videoCanAdjust() && capVideo) capVideo->layoutTool();
	else if (toolCap) layoutTool(toolCap.get());
	syncQrcodePanel();
	syncOcrPanel();
	refresh();
	return true;
}

bool WinCap::sizePreviewOrigin(float& cx, float& cy, float& halfSpan) const
{
	if (!cutMask || !cutMask->hasRect()) return false;
	const auto& r = cutMask->maskRect;
	cx = (r.left + r.right) * 0.5f;
	cy = (r.top + r.bottom) * 0.5f;
	halfSpan = std::min(80.f, std::max(40.f, (r.right - r.left) * 0.14f));
	return true;
}

POINT WinCap::toImgPos(const POINT& pos) const
{
	if (stage == CapStage::Long && capLong && (capLong->isEditMode() || hasAnnotShapes()))
		return capLong->clientToLongImg(pos);
	return AnnotHost::toImgPos(pos);
}

D2D1_POINT_2F WinCap::annotToClientPt(float ax, float ay) const
{
	if (stage == CapStage::Long && capLong && (capLong->isEditMode() || hasAnnotShapes()))
		return capLong->longImgToClient(ax, ay);
	return AnnotHost::annotToClientPt(ax, ay);
}

D2D1_POINT_2F WinCap::clientToAnnotPt(float cx, float cy) const
{
	if (stage == CapStage::Long && capLong && (capLong->isEditMode() || hasAnnotShapes())) {
		const auto p = capLong->clientToLongImg(POINT{ (LONG)std::lround(cx), (LONG)std::lround(cy) });
		return D2D1::Point2F((float)p.x, (float)p.y);
	}
	return AnnotHost::clientToAnnotPt(cx, cy);
}

void WinCap::onWheel(POINT pos, float space)
{
	if (space == 0.f) return;
	const int dir = space > 0.f ? 1 : -1;

	// 长截图：预览区优先；编辑标注时其余走标注滚轮
	if (stage == CapStage::Long) {
		if (capLong && capLong->onWheel(pos, space)) return;
		if (annotLive && capLong && capLong->isEditMode()) {
			const auto img = toImgPos(pos);
			if (shapeHover && !shapeHover->isViewportFilter()) {
				shapeHover->mouseWheel((float)img.x, (float)img.y,
					dir > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
				pulseSizePreview();
				return;
			}
			if (cutMask && cutMask->hasRect()) {
				const auto& r = cutMask->maskRect;
				if (pos.x >= (int)r.left && pos.x <= (int)r.right
					&& pos.y >= (int)r.top && pos.y <= (int)r.bottom
					&& toolSub && !getCurToolId().empty()
					&& getCurToolId() != L"eraser") {
					toolSub->adjustPrimarySize(dir);
				}
			}
		}
		return;
	}

	if (annotLive) {
		const auto img = toImgPos(pos);
		// 单选标注：改该标注粗细
		if (shapeHover && !shapeHover->isViewportFilter()) {
			shapeHover->mouseWheel((float)img.x, (float)img.y,
				dir > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
			// 改粗/圆角会改像素：上层盖着的模糊/马赛克要重新取样
			refreshFiltersAbove(shapeHover);
			pulseSizePreview();
			return;
		}
		// 视窗外上/下/左/右：微调选区
		if (nudgeSelectionByOutsideWheel(pos, dir)) return;
		// 视窗内 + 有工具：改默认粗细 + 临时预览
		if (cutMask && cutMask->hasRect()) {
			const auto& r = cutMask->maskRect;
			if (pos.x >= (int)r.left && pos.x <= (int)r.right
				&& pos.y >= (int)r.top && pos.y <= (int)r.bottom
				&& toolSub && !getCurToolId().empty()
				&& getCurToolId() != L"eraser") {
				toolSub->adjustPrimarySize(dir);
				return;
			}
		}
		return;
	}

	// 框选 / 录屏开录前：区外滚轮微调位置
	if (stage == CapStage::Adjust || videoCanAdjust())
		nudgeSelectionByOutsideWheel(pos, dir);
}

void WinCap::copyCurrentStage()
{
    if (stage == CapStage::Adjust) {
        copyToClipboard();
    }
    else if (stage == CapStage::Long && capLong && capLong->hasImage()) {
        // 还没点"开始"滚动时一张图都没有（hasImage），此时不该收工
        longCopyToClipboard();
        close();
    }
    else if (stage == CapStage::Video && capVideo) {
        // 停录、存盘、关窗都在 ToolVideo 那边一条龙做完
        capVideo->onSaveKey(true);
    }
}

ComPtr<ID2D1Bitmap1> WinCap::getCutImg()
{
    ComPtr<ID2D1Bitmap1> cutImg;
    auto& maskRect = cutMask->maskRect;
    const UINT32 cw = (UINT32)(maskRect.right - maskRect.left);
    const UINT32 ch = (UINT32)(maskRect.bottom - maskRect.top);
    if (cw == 0 || ch == 0) return cutImg;
    D2D1_BITMAP_PROPERTIES1 prop{};
    prop.pixelFormat = screenImg->GetPixelFormat();
    prop.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
    Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(cw, ch), nullptr, 0, &prop, cutImg.GetAddressOf());
    auto start = D2D1::Point2U(0, 0);
    auto rect = D2D1::RectU((UINT32)maskRect.left, (UINT32)maskRect.top, (UINT32)maskRect.right, (UINT32)maskRect.bottom);
    cutImg->CopyFromBitmap(&start, screenImg.Get(), &rect);
    return cutImg;
}

LRESULT WinCap::onHitTest(const POINT pos)
{
    // 仅「正在录制」时整窗穿透；开录前还要拖选区
    if (isMouseTransparent) return HTTRANSPARENT;
    return HTCLIENT;
}

void WinCap::onDown(POINT pos, bool isRight)
{
    // 按下即撤查找元素的查询保护：查询期间遮罩可能挂着"临时点穿 + 临时捕获"，
    // 不先撤掉这一下按下会漏给下层窗口，也会把下面要 SetCapture 的拖框捕获弄丢
    if (cutMask) cutMask->cancelHoverQuery();
    if (isRight) {
        if (handleLayerCancel(false))
            requestExitAfterRightUp();
        return;
    }
    // 点遮罩易把全屏窗抬到前；侧栏是 owned 窗，再抬一次确保可点
    if (stage == CapStage::Long && capLong) {
		capLong->raiseTool();
		if (capLong->hitPreview(pos)) {
			capLong->onDown(pos, false);
			return;
		}
		if (annotLive && capLong->isEditMode()) {
			if (cutMask && cutMask->hasRect()) {
				const auto& r = cutMask->maskRect;
				if (pos.x >= (int)r.left && pos.x <= (int)r.right
					&& pos.y >= (int)r.top && pos.y <= (int)r.bottom) {
					annotDown(pos, isRight);
				}
			}
			return;
		}
		capLong->onDown(pos, false);
		return;
	}
    if (cutMask && !demoMode && cutMask->onInfoDown(pos)) {
        // 仅拖圆角滑条需要 Capture；点开关/色块立刻放开会抢工具栏鼠标
        if (cutMask->infoInteracting())
            SetCapture(hwnd);
        if (tip) { tip->hide(); infoTipHit_ = InfoHit::None; }
        return;
    }
    // 选区框好之后、且当前没有任何标注时，窗口里任意位置双击 = 点工具条上的「完成」（复制并退出）。
    // 双击判定自己做：Ling 的窗口类没带 CS_DBLCLKS，WM_LBUTTONDBLCLK 根本不会来，
    // 所以用系统双击间隔（控制面板里调的那个）+ 双击判定框来认。
    // ★ 必须放在 annotLive 分支之前：选区框好后 annotLive 恒为 true，
    //   原先那次判定写在 annotLive 之后 → 永远走不到，"双击完成"形同不存在。
    {
        const auto now = GetTickCount64();
        const bool dbl = (now - lastDownTime <= GetDoubleClickTime())
            && std::abs(pos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
            && std::abs(pos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
        lastDownTime = now;
        lastDownPos = pos;
        if (dbl && !demoMode && cutMask && cutMask->hasRect()
            && !hasAnnotShapes() && stage == CapStage::Adjust) {
            copyToClipboard();
            return;
        }
    }
    if (annotLive) {
        if (beginMaskResize(pos)) return;
        annotDown(pos, isRight);
        return;
    }
    // 演示画布：没开标注会话（默认工具=无）时画布不响应点击，别退化成"拖选区"
    if (demoMode) return;
    if (stage == CapStage::Select) {
        isPress = true;
        selectDragging_ = false;
        selectionFromElement_ = false;
        cutMask->startMakeRect(pos);
        refresh();
    }
    else if (stage == CapStage::Adjust) {
        isPress = true;
        cutMask->startAdjust(pos);
        setPixPos(pos);
        if (cutMask->adjustingHit() != MaskHit::None) {
            SetCapture(hwnd); // 拖边时结果窗盖在选区上，无 Capture 则抬起被抢走、isPress 卡死
            setToolChromeHidden(true);
        }
        refresh();
    }
    else if (videoCanAdjust()) {
        isPress = true;
        cutMask->startAdjust(pos);
        setPixPos(pos);
        if (cutMask->adjustingHit() != MaskHit::None) {
            SetCapture(hwnd);
            setToolChromeHidden(true);
        }
        refresh();
    }
}

void WinCap::updateInfoBarTip(POINT pos)
{
	if (!tip || !cutMask || cutMask->hideLabel) {
		if (tip && infoTipHit_ != InfoHit::None) {
			tip->hide();
			infoTipHit_ = InfoHit::None;
		}
		return;
	}
	const auto hit = cutMask->hitInfoControl(pos);
	const wchar_t* text = nullptr;
	switch (hit) {
	case InfoHit::Lock: text = L"锁定比例"; break;
	case InfoHit::Radius: text = L"圆角"; break;
	case InfoHit::Shadow: text = L"投影"; break;
	default: break;
	}
	if (!text) {
		if (infoTipHit_ != InfoHit::None) {
			tip->hide();
			infoTipHit_ = InfoHit::None;
		}
		return;
	}
	float sx = 0.f, sy = 0.f;
	if (!cutMask->infoTipAnchor(hit, sx, sy)) return;
	// 同一个控件就别每次鼠标移动都重挂一遍：Tip 的锚点在信息栏上是固定的，
	// 旧代码在信息栏上滑动时会把 showAt（重排 + 定位 + 显示）跑满每一个 WM_MOUSEMOVE
	if (hit == infoTipHit_) return;
	// Tip 用 Node* 做归属比较；信息栏无 Node，用稳定哨兵指针
	static char tipIds[3]{};
	Ling::Node* owner = nullptr;
	if (hit == InfoHit::Lock) owner = reinterpret_cast<Ling::Node*>(&tipIds[0]);
	else if (hit == InfoHit::Radius) owner = reinterpret_cast<Ling::Node*>(&tipIds[1]);
	else owner = reinterpret_cast<Ling::Node*>(&tipIds[2]);
	infoTipHit_ = hit;
	tip->showAt(owner, sx, sy, text);
}

void WinCap::onMove(POINT pos)
{
    if (cutMask && cutMask->infoInteracting()) {
        cutMask->onInfoMove(pos);
        return;
    }
    // 演示画布不画信息栏（也不该能命中它），别再弹那圈的悬停提示
    if (!demoMode) updateInfoBarTip(pos);
    // 长截图：预览交互优先于标注（编辑态也要能拖/悬停预览）
    if (stage == CapStage::Long && capLong) {
        capLong->onMove(pos);
        if (capLong->isPreviewPanning() || capLong->isSelViewportPanning()
			|| capLong->hitPreview(pos)) {
            setCursor();
            return;
        }
        if (annotLive && capLong->isEditMode()) {
            annotMove(pos);
            setCursor();
            return;
        }
        setCursor();
        return;
    }
    if (annotLive) {
        if (maskDragging && isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            cutMask->adjust(cur);
            if (cutMask->isResizingEdge()) setPixPos(cur);
            syncQrcodePanel();
            refresh();
            return;
        }
        annotMove(pos);
        setCursor();
        return;
    }
    if (stage == CapStage::Select) {
        if (isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            // 超过单击抖动阈值才算拖动 → 才出放大镜
            if (!selectDragging_) {
                const int moved = std::abs(cur.x - lastDownPos.x) + std::abs(cur.y - lastDownPos.y);
                if (moved > 5) selectDragging_ = true;
            }
            // 只有真正拖动后才改写选区：单击时保留「查找元素」悬停到的那个框。
            // 否则一点点手抖就会 makeRect 出一个极小的框（常是 0 宽或 0 高），hasRect() 判空后
            // onUp 直接 return —— 表现就是"单击没确认、要连点好几次才成"。
            if (selectDragging_) {
                cutMask->makeRect(cur);
                setPixPos(cur);
            }
            refresh();
        }
        else {
            cutMask->highlight(pos);
            // 查找元素阶段：悬停即显示放大镜并跟随光标
            if (Setting::get()->getFindWindowElements() && !demoMode) {
                setPixPos(pos);
                refresh();
            }
        }
        setCursor();
    }
    else if (stage == CapStage::Adjust) {
        if (isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            cutMask->adjust(cur);
            if (cutMask->isResizingEdge()) setPixPos(cur);
            syncQrcodePanel();
            refresh();
        }
        setCursor();
    }
    else if (videoCanAdjust()) {
        if (isPress) {
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            cutMask->adjust(cur);
            if (cutMask->isResizingEdge()) setPixPos(cur);
            syncQrcodePanel();
            refresh();
        }
        setCursor();
    }
}

void WinCap::onUp(POINT pos, bool isRight)
{
    if (isRight) {
        if (deferExitAfterRightUp) {
            deferExitAfterRightUp = false;
            ReleaseCapture();
            close();
        }
        return;
    }
    if (cutMask && cutMask->infoInteracting()) {
        cutMask->onInfoUp();
        ReleaseCapture();
        return;
    }
    if (stage == CapStage::Long && capLong) {
        if (capLong->isPreviewPanning()) {
            capLong->onUp(pos);
            return;
        }
        if (annotLive && capLong->isEditMode()) {
            annotUp(pos, isRight);
            return;
        }
        if (capLong->isSelViewportPanning()) {
            capLong->onUp(pos);
            ReleaseCapture();
            return;
        }
        capLong->onUp(pos);
        return;
    }
    if (annotLive) {
        if (maskDragging) {
            maskDragging = false;
            isPress = false;
            ReleaseCapture();
            if (cutMask) cutMask->endAdjust();
            syncViewportFilters();
            setToolChromeHidden(false);
            refresh();
            return;
        }
        annotUp(pos, isRight);
        return;
    }
    // 演示画布：没有标注会话时点击已在 onDown 被吃掉，这里不要再走"改选区收尾"
    if (demoMode) return;
    if (stage == CapStage::Select) {
        // 没有按下就来的抬起：托盘菜单项是在 mouse **down** 上触发命令的（Ling::Button::onDown），
        // 菜单随之销毁，用户那一下抬手就落到刚 show() 出来的本窗口上。而"查找元素"悬停
        // 已经把 maskRect 铺好了（CutMask::setHoverTarget / snapHoverTarget），旧代码只看
        // hasRect() 就会把它当成"框选完成"：截图会话直接带着悬停元素的框进 Adjust，
        // 托盘「屏幕录制」更是一路冲进录制 —— 表现就是"没有查找元素和框选，点了就没了"。
        // 所以这里必须要求"这一次确实按下过"。
        if (!isPress) return;
        // 没有拖拽、悬停停在窗口元素上 → 这次是"点选窗口"，按焦点窗口处理（文件名/历史来源）
        selectionFromElement_ = !selectDragging_ && cutMask->hoverPicked();
        isPress = false;
        selectDragging_ = false;
        if (!cutMask->hasRect()) return;
        // 托盘「屏幕录制」：点一下、抖一下不算框选，别拿 1 像素选区直接开录
        if (pendingRecord) {
            const auto& mr = cutMask->maskRect;
            if (mr.right - mr.left < 4.f || mr.bottom - mr.top < 4.f) return;
        }
        if (enterByArg()) return;
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            startPin();
            return;
        }
        stage = CapStage::Adjust;
        refresh();
        // 托盘「屏幕录制」进来的一次会话：框完直接进录屏，不再弹截图工具栏
        if (pendingRecord) {
            pendingRecord = false;
            startVideo();
            return;
        }
        makeToolCap();
    }
    else if (stage == CapStage::Adjust) {
        isPress = false;
        ReleaseCapture();
        if (cutMask) cutMask->endAdjust();
        setToolChromeHidden(false);
        syncQrcodePanel();
        refresh();
    }
    else if (videoCanAdjust()) {
        isPress = false;
        ReleaseCapture();
        if (cutMask) cutMask->endAdjust();
        setToolChromeHidden(false);
        syncQrcodePanel();
        refresh();
    }
}

// close() 里 DestroyWindow 之后同步触发 onDestroy，而这条路径通常是从某个工具条的按钮
// 回调里一路进来的（工具条是 WinCap / CapLong / CapVideo 的成员）。在这里直接
// winCap.reset() 就是 use-after-free，所以窗口句柄立即销毁，C++ 对象的释放推迟到下一轮消息循环。
void WinCap::onClosed()
{
    if (isClosed) return;
    isClosed = true;
    deferExitAfterRightUp = false;
    CaptureEsc::release(hwnd);
    killTimer(recCursorTimerId);
    annotLive = false;
    // 截图会话结束：把离线翻译引擎（模型几百 MB）还回去。它平时按语言对常驻换速度，
    // 但这一次截图已经收工，没有继续留着的理由 —— 下次要用再重新加载。
    // release() 内部加锁：若还有后台翻译在跑会等它收尾，不会把正在用的引擎抽掉。
    BergamotEngine::release();
    // 会话结束：把那几条临时全局键从钩子上撤掉（幂等，本来没挂也安全）
    syncGlobalCommandKeys(0);
    stopKeyOverlay();
    if (capVideo) capVideo->dispose();
    if (capLong) capLong->dispose();
    if (toolQrcode) { toolQrcode->close(); toolQrcode.reset(); }
    if (toolCap) toolCap->close();
    if (toolSub) toolSub->close();
    Ling::App::get()->dq.TryEnqueue([]() {
        winCap.reset();
        if (!WinPin::hasWindow()) {
            if (Ling::App::get()->args[L"--auto-quit"] == L"true") {
                Ling::App::get()->quit(0);
            }
            else {
                Update::checkLater();
            }
        }
    });
}

void WinCap::stopIfRecording()
{
    if (!winCap || !winCap->capVideo) return;
    // 正在录制：先停止编码线程，避免退出时线程与设备卡死
    winCap->capVideo->stop();
}

void WinCap::makeToolCap()
{
    if (toolCap) {
        layoutTool(toolCap.get());
        toolCap->show();
        return;
    }
    clearMainToolUserPlaced();
    toolCap = std::make_unique<ToolCap>(this);
    toolCap->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
    toolCap->bindOwner(hwnd);
    layoutTool(toolCap.get());
}

bool WinCap::enterByArg()
{
    auto& args = Ling::App::get()->args;
    auto it = args.find(L"--enter");
    if (it == args.end()) return false;
    auto& val = it->second;
    stage = CapStage::Adjust;
    refresh();
    if (val == L"long") startLong();
    else if (val == L"video") startVideo();
    else if (val == L"ocr") startOcr();
    else if (val == L"qr" || val == L"qrcode") startQrcode();
    else if (val == L"pin") startPin();
    else return false;
    return true;
}

void WinCap::relayoutTool()
{
    if (capLong) capLong->layoutTool();
    else if (capVideo) capVideo->layoutTool();
    else if (toolCap) layoutTool(toolCap.get());
}

void WinCap::layoutTool(Ling::WinBase* tool)
{
    if (!tool) return;
    if (isMainToolUserPlaced()) return;
    const int toolW = (int)(tool->w + 0.5f);
    const int toolH = (int)(tool->h + 0.5f);
    // 演示模式：工具栏固定在**屏幕顶部居中**（全屏画布工具栏的默认吸附位）
    if (demoMode) {
        const int gap = (int)(12.f * tool->dpi + 0.5f);
        const int toolX = x + (int)((w - toolW) * 0.5f);
        const int toolY = y + gap;
        if (tool->x != toolX || tool->y != toolY) tool->setPosition(toolX, toolY);
        return;
    }
    // shadowPad 曾为投影透明边；现为 0，工具窗即可视栏面
    const int pad = (int)(ToolbarTheme::shadowPad * tool->dpi + 0.5f);
    const int visualH = (std::max)(1, toolH - pad * 2);
    const int visualW = (std::max)(1, toolW - pad * 2);

    const int maskLeftScr = x + (int)cutMask->maskRect.left;
    const int maskTopScr = y + (int)cutMask->maskRect.top;
    const int maskRightScr = x + (int)cutMask->maskRect.right;
    const int maskBottomScr = y + (int)cutMask->maskRect.bottom;

    RECT maskScrRect{ maskLeftScr, maskTopScr, maskRightScr, maskBottomScr };
    HMONITOR hMon = MonitorFromRect(&maskScrRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(hMon, &mi);

    const int gap = (int)(cutMask->strokeWidth + 2.f * tool->dpi + 0.5f);
    const bool fitBelow = (maskBottomScr + gap + visualH) <= mi.rcWork.bottom;
    const bool fitAbove = (maskTopScr - gap - visualH) >= mi.rcWork.top;

    // 可视栏左缘对齐选区左缘（与左上角信息栏同侧）
    int toolX = maskLeftScr - pad;
    int toolY = 0;
    if (fitBelow) {
        toolY = maskBottomScr + gap - pad;
    }
    else if (fitAbove) {
        toolY = maskTopScr - gap - toolH + pad;
    }
    else {
        const int overlapPad = (int)(3.f * tool->dpi + 0.5f);
        toolX = maskLeftScr - pad + overlapPad;
        toolY = maskBottomScr - toolH + pad - overlapPad;
    }

    if (toolX < mi.rcWork.left) toolX = mi.rcWork.left;
    if (toolX + toolW > mi.rcWork.right) toolX = mi.rcWork.right - toolW;
    if (tool->x == toolX && tool->y == toolY) return;
    tool->setPosition(toolX, toolY);
}

void WinCap::enterLiveStage()
{
    // 底图是拖框那一刻的静态截图，自本阶段起不再绘制 ——
    // 否则录屏与滚动截图取到的都是这张静态底图。只保留遮罩，选区内部为透明区域。
    hideScreenImg = true;
    // 尺寸标签放不下时会折回选区内部（全屏必然如此），那就会被录进去 / 滚进长图。
    // 选区这时已经定死了，标签也没什么可看的，直接不画 —— 这样选区内就真的什么都不画了，
    // 不必再拿 WDA_EXCLUDEFROMCAPTURE 去摘整块屏幕大小的宿主窗口
    cutMask->hideLabel = true;
    if (toolCap) toolCap->hide();
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    refresh();
}

bool WinCap::videoCanAdjust() const
{
	return stage == CapStage::Video && capVideo && !capVideo->isRecording() && !isMouseTransparent;
}

bool WinCap::maskSizeLocked() const
{
	// 录屏已开录：画幅定死（整体挪动仍放行，见 maskMoveLocked）
	if (stage == CapStage::Video && !videoCanAdjust()) return true;
	// 长图已开始滚动（含暂停 / 编辑态）：画幅同样定死
	if (stage == CapStage::Long && capLong && capLong->capturing()) return true;
	return false;
}

bool WinCap::maskMoveLocked() const
{
	// 长图由同一块选区逐帧拼接，滚动中挪动会使已拼部分与新帧错位，故连整体挪动也禁止
	return stage == CapStage::Long && capLong && capLong->capturing();
}

// 「选框鼠标跟随」(视频录制)：录制中且处于穿透态时，光标贴近选区边缘就自动平移选区，
// 使操作始终留在录制画面内。穿透后本窗口收不到鼠标移动事件，只能按固定间隔轮询光标位置。
// 平移只改位置、不改画幅（与 maskSizeLocked 的"录制中不许改大小"一致）。
void WinCap::updateSelFollowCursor()
{
	if (stage != CapStage::Video) { killTimer(recCursorTimerId); return; }
	if (!cutMask || !cutMask->hasRect()) return;
	if (!capVideo || !capVideo->isRecording()) return;
	if (!isMouseTransparent) return;   // 只在穿透模式下跟随
	if (!Setting::get() || !Setting::get()->getSelFollowMouse()) return;

	POINT cur{};
	if (!GetCursorPos(&cur)) return;
	ScreenToClient(hwnd, &cur);        // 选区是客户区坐标（窗口铺满整个虚拟桌面）

	const auto& r = cutMask->maskRect;
	const float margin = selFollowMargin * dpi;
	// 光标越出"边缘内侧 margin"这条线时，按越出的距离整体平移，把它带回线内
	float dx = 0.f, dy = 0.f;
	if (cur.x < r.left + margin)        dx = (float)cur.x - (r.left + margin);
	else if (cur.x > r.right - margin)  dx = (float)cur.x - (r.right - margin);
	if (cur.y < r.top + margin)         dy = (float)cur.y - (r.top + margin);
	else if (cur.y > r.bottom - margin) dy = (float)cur.y - (r.bottom - margin);
	if (dx == 0.f && dy == 0.f) return;
	// 传光标作为"定屏依据"：鼠标一进另一块显示器，选区就整块贴过去（录制侧同步换采集目标），
	// 否则光标只是越出选区、选区却卡在原屏边缘不动
	if (!cutMask->translateBy(dx, dy, &cur)) return;   // 已贴到桌面边缘、挪不动了

	if (annotLive) syncViewportFilters();
	if (annotLive) layoutTools();
	else if (capVideo) capVideo->layoutTool();
	syncQrcodePanel();
	syncOcrPanel();
	refresh();
}

// 「鼠标高亮提醒」：录制中光标位置变化就重画一次（高亮圆由 paintMouseHighlight 画）
void WinCap::updateMouseHighlight()
{
	if (stage != CapStage::Video) { killTimer(recCursorTimerId); return; }
	if (!capVideo || !capVideo->isRecording()) return;
	if (!Setting::get() || !Setting::get()->getMouseHighlight()) return;

	POINT cur{};
	if (!GetCursorPos(&cur)) return;
	ScreenToClient(hwnd, &cur);
	// 圆心取光标本体中心而非热点（热点在指针尖角上，见 Util::cursorBodyOffset）
	float ox = 0.f, oy = 0.f;
	Util::cursorBodyOffset(ox, oy);
	const POINT c{ cur.x + (LONG)ox, cur.y + (LONG)oy };
	if (c.x == mouseHlPos_.x && c.y == mouseHlPos_.y) return;   // 没动就不重画
	mouseHlPos_ = c;
	refresh();
}

// 「鼠标高亮提醒」：在光标下垫一个半透明圆形色块。调用点在 paintCursor 之前 ——
// 视觉上位于系统光标之下，符合"色块叠在鼠标后面"。录制结束后残留的刷新不会画它（条件已不成立）。
void WinCap::paintMouseHighlight(ID2D1DeviceContext* ctx)
{
	if (!ctx) return;
	if (stage != CapStage::Video || !capVideo || !capVideo->isRecording()) return;
	if (!Setting::get() || !Setting::get()->getMouseHighlight()) return;

	POINT cur{};
	if (!GetCursorPos(&cur)) return;
	ScreenToClient(hwnd, &cur);
	const UINT32 rgba = Setting::get()->getMouseHighlightColor();
	// 颜色可改：变了就换笔刷（色号只存 RGB，A 固定 FF；这里按固定透明度画半透明色块）
	if (!mouseHlBrush_ || mouseHlColor_ != rgba) {
		mouseHlBrush_.Reset();
		Ling::D2D::get()->deviceContext->CreateSolidColorBrush(
			D2D1::ColorF(((rgba >> 24) & 0xFFu) / 255.f, ((rgba >> 16) & 0xFFu) / 255.f,
				((rgba >> 8) & 0xFFu) / 255.f, kMouseHlAlpha),
			mouseHlBrush_.GetAddressOf());
		mouseHlColor_ = rgba;
	}
	if (!mouseHlBrush_) return;
	// 圆心 = 热点 + 光标本体偏移（物理像素，与下面的半径同口径）
	float ox = 0.f, oy = 0.f;
	Util::cursorBodyOffset(ox, oy);
	const float r = mouseHlRadius * dpi;
	ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)cur.x + ox, (float)cur.y + oy), r, r),
		mouseHlBrush_.Get());
}

// 选区被挪动（拖拽 / 鼠标跟随 / 方向键 / 滚轮）后由 CutMask 回调。
// 录制中把平移同步给采集线程，否则屏幕上选区已经挪走、录出来的画面还是原来那一块。
void WinCap::onMaskRectChanged()
{
	if (stage != CapStage::Video || !capVideo || !capVideo->isRecording()) return;
	capVideo->syncCaptureMove();
}

bool WinCap::annotRightClick()
{
	if (handleLayerCancel(false))
		requestExitAfterRightUp();
	return true;
}

bool WinCap::annotSkipHitSelect() const
{
	// 录屏/长图编辑且已选标注工具（含橡皮）：不点选已有标注
	if (stage == CapStage::Video && !getCurToolId().empty()) return true;
	if (stage == CapStage::Long && capLong && capLong->isEditMode() && !getCurToolId().empty()) return true;
	return false;
}

bool WinCap::annotEmptyToolDrag(POINT pos)
{
	if (stage != CapStage::Long || !capLong || !capLong->isEditMode()) return false;
	if (!capLong->isSelViewportPanning()) {
		if (!capLong->beginSelViewportPan(annotPressPos)) return false;
	}
	capLong->onMove(pos);
	return true;
}

void WinCap::annotAfterEmptyToolUp()
{
	if (capLong && capLong->isSelViewportPanning()) {
		POINT pos{};
		GetCursorPos(&pos);
		ScreenToClient(hwnd, &pos);
		capLong->onUp(pos);
	}
}

void WinCap::requestExitAfterRightUp()
{
	deferExitAfterRightUp = true;
	if (hwnd) SetCapture(hwnd);
}

bool WinCap::handleLayerCancel(bool fromToolbar)
{
	// 右键分层取消（Esc 另走直接退出）
	// 返回 true = 应退出截图（由 onUp 再 close，避免桌面吃到右键抬起）
	if (stage == CapStage::Long) {
		if (capLong && capLong->isEditMode()) {
			if (editingText) {
				editingText->finishEdit();
				return false;
			}
			if (annotMouseDown && newShape && history) {
				history->removeShape(newShape);
				newShape = nullptr;
				shapeHover = nullptr;
				annotMouseDown = false;
				annotDragIndex = -1;
				ReleaseCapture();
				refresh();
				return false;
			}
			if (shapeHover) {
				clearAnnotSelection();
				return false;
			}
			if (!getCurToolId().empty()) {
				if (capLong) capLong->cancelAnnotSelect();
				else clearLongAnnotTool();
				return false;
			}
			// 退出编辑栏，回到长截图栏
			toggleLongEdit();
			return false;
		}
		return true;
	}
	// 录屏：先退标注层，再退选区拖动，最后退出
	if (stage == CapStage::Video) {
		if (editingText) {
			editingText->finishEdit();
			return false;
		}
		if (annotMouseDown && newShape && history) {
			history->removeShape(newShape);
			newShape = nullptr;
			shapeHover = nullptr;
			annotMouseDown = false;
			annotDragIndex = -1;
			ReleaseCapture();
			refresh();
			return false;
		}
		if (shapeHover) {
			clearAnnotSelection();
			return false;
		}
		if (!getCurToolId().empty()) {
			if (capVideo) capVideo->cancelAnnotSelect();
			else clearVideoAnnotTool();
			return false;
		}
		if (isPress && cutMask) {
			cutMask->cancelAdjust();
			isPress = false;
			ReleaseCapture();
			setToolChromeHidden(false);
			return false;
		}
		return true;
	}
	// 1) 文字编辑 → 完成并保留已输入
	if (editingText) {
		editingText->finishEdit();
		return false;
	}
	// 2) 落笔草稿未抬起 → 丢掉
	if (annotMouseDown && newShape && history) {
		history->removeShape(newShape);
		newShape = nullptr;
		shapeHover = nullptr;
		annotMouseDown = false;
		annotDragIndex = -1;
		ReleaseCapture();
		refresh();
		return false;
	}
	// 3) 选中标注 → 取消选中（先于取消工具）
	if (shapeHover) {
		clearAnnotSelection();
		return false;
	}
	// 4) 有标注工具 → 取消工具
	if (toolCap && !toolCap->curId.empty()) {
		toolCap->cancelSelect();
		refresh();
		return false;
	}
	// 工具栏上右键：最多退到未选工具，不重框选、不退出
	if (fromToolbar) return false;
	// 5) 改选区拖拽中 → 恢复原选区
	if (isPress && (stage == CapStage::Adjust || videoCanAdjust()) && cutMask) {
		cutMask->cancelAdjust();
		isPress = false;
		ReleaseCapture();
		setToolChromeHidden(false);
		return false;
	}
	// 演示画布：右键最多退到"取消选中/取消工具"（上面 1–5 层），
	// 不能再往下走 enterReselect —— 那会把全屏画布拆回"查找元素"框选态
	if (demoMode) return false;
	// 6) 已框选（Adjust/标注）→ 回到查找元素
	if (stage == CapStage::Adjust || annotLive) {
		enterReselect();
		return false;
	}
	// 7) 查找元素态 → 退出截图
	return true;
}

void WinCap::enterReselect()
{
	// 回到查找元素：清标注与选区
	annotLive = false;
	clearMainToolUserPlaced();
	clearAllShapes();
	if (toolCap) {
		toolCap->cancelSelect();
		toolCap->hide();
	}
	if (toolSub) toolSub->hideTools();
	if (cutMask) cutMask->clearRect();
	stage = CapStage::Select;
	isPress = false;
	selectDragging_ = false;
	selectionFromElement_ = false;
	refresh();
}

void WinCap::startAnnotate(const std::wstring& toolId)
{
	if (!cutMask->hasRect()) return;
	ensureHistory();
	if (toolSub) toolSub->bindOwner(hwnd);
	annotLive = true;
	// 标注阶段仍显示选区尺寸信息栏；长图/录屏才在 enterLiveStage 里隐藏
	cutMask->hideLabel = false;
	// 全屏同表面：不挖洞、不建选区 Pin；只换笔；保留已选标注
	if (toolCap) toolCap->selectAnnotTool(toolId);
	presentSelectedAnnotStyle();
	layoutTools();
	refresh();
}

void WinCap::startPin(const std::wstring& /*toolId*/)
{
	if (!cutMask->hasRect()) return;
	std::vector<BYTE> pixels;
	int cw = 0, ch = 0;
	if (!exportSelection(pixels, cw, ch)) return;
	const int px = x + (int)cutMask->maskRect.left;
	const int py = y + (int)cutMask->maskRect.top;
	annotLive = false;
	WinPin::initFromData(px, py, cw, ch, pixels);
	close();
}

void WinCap::layoutTools()
{
	if (stage == CapStage::Video && capVideo) {
		capVideo->layoutTool();
	}
	else if (stage == CapStage::Long && capLong) {
		capLong->layoutTool();
	}
	else if (toolCap) {
		layoutTool(toolCap.get());
	}
	if (!toolSub || !annotLive || !toolSub->hasContent() || getCurToolId().empty()) {
		if (toolSub && getCurToolId().empty()) toolSub->hideTools();
		return;
	}
	RECT winRect{ x, y, x + (int)w, y + (int)h };
	const RECT wa = ToolbarChrome::workAreaNear(winRect);
	// 演示模式同样跟着触发工具锚定（居中于按钮 + 箭头指向它）：先按主栏居中再改窗口 x
	// 会把 updatePosition 算好的 arrowX 甩掉，表现为「属性栏既不对准工具、箭头也乱指」
	toolSub->updatePosition(wa);
	HWND bar = nullptr;
	if (stage == CapStage::Video && capVideo)
		bar = capVideo->toolHwnd();
	else if (stage == CapStage::Long && capLong)
		bar = capLong->toolHwnd();
	else if (toolCap) bar = toolCap->hwnd;
	if (bar && toolSub->hwnd) {
		SetWindowPos(toolSub->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		SetWindowPos(bar, toolSub->hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void WinCap::onHistoryChanged()
{
	AnnotHost::onHistoryChanged();
	if (capVideo) capVideo->notifyHistoryChanged();
	if (capLong) capLong->notifyHistoryChanged();
}

bool WinCap::queryMainBarAnchor(MainBarAnchor& out) const
{
	if (stage == CapStage::Video && capVideo) {
		return capVideo->queryBarAnchor(out.x, out.y, out.w, out.h, out.btnCenterX, out.dpi);
	}
	if (stage == CapStage::Long && capLong) {
		return capLong->queryBarAnchor(out.x, out.y, out.w, out.h, out.btnCenterX, out.dpi);
	}
	return AnnotHost::queryMainBarAnchor(out);
}

void WinCap::startVideoAnnotate(const std::wstring& toolId)
{
	if (stage != CapStage::Video || toolId.empty()) return;
	setMouseTransparent(false);
	ensureHistory();
	if (toolSub) toolSub->bindOwner(hwnd);
	annotLive = true;
	// 保留已选标注：点所属工具应出属性栏，不应取消选中
	setForcedToolId(toolId);
	if (!toolSub) return;
	if (toolId == L"rect") toolSub->showRectTools();
	else if (toolId == L"arrow") toolSub->showArrowTools();
	else if (toolId == L"pen") toolSub->showPenTools();
	else if (toolId == L"text") toolSub->showTextTools();
	else if (toolId == L"number") toolSub->showNumberTools();
	else if (toolId == L"mosaic") toolSub->showMosaicTools();
	else if (toolId == L"eraser") toolSub->showEraserTools();
	else toolSub->hideTools();
	presentSelectedAnnotStyle();
	layoutTools();
	refresh();
}

void WinCap::clearVideoAnnotTool()
{
	clearForcedToolId();
	if (toolSub) toolSub->hideTools();
	// 取消工具后仍保留选中，方便继续改属性（点空白再清）
	refresh();
}

void WinCap::toggleLongEdit()
{
	if (stage != CapStage::Long || !capLong) return;
	const bool next = !capLong->isEditMode();
	if (next) {
		ensureHistory();
		if (toolSub) toolSub->bindOwner(hwnd);
	}
	else {
		clearLongAnnotTool();
		annotLive = false;
	}
	capLong->setEditMode(next);
	if (next) {
		// 进入编辑态先不选工具，点画笔等再 startLongAnnotate
		annotLive = true;
	}
	layoutTools();
	refresh();
}

void WinCap::startLongAnnotate(const std::wstring& toolId)
{
	if (stage != CapStage::Long || !capLong || toolId.empty()) return;
	if (!capLong->isEditMode()) capLong->setEditMode(true);
	ensureHistory();
	if (toolSub) toolSub->bindOwner(hwnd);
	annotLive = true;
	// 保留已选标注：点所属工具应出属性栏，不应取消选中
	setForcedToolId(toolId);
	if (!toolSub) return;
	if (toolId == L"rect") toolSub->showRectTools();
	else if (toolId == L"arrow") toolSub->showArrowTools();
	else if (toolId == L"pen") toolSub->showPenTools();
	else if (toolId == L"text") toolSub->showTextTools();
	else if (toolId == L"number") toolSub->showNumberTools();
	else if (toolId == L"mosaic") toolSub->showMosaicTools();
	else if (toolId == L"eraser") toolSub->showEraserTools();
	else toolSub->hideTools();
	presentSelectedAnnotStyle();
	layoutTools();
	refresh();
}

void WinCap::clearLongAnnotTool()
{
	clearForcedToolId();
	if (toolSub) toolSub->hideTools();
	// 取消工具后仍保留选中，方便继续改属性（点空白再清）
	refresh();
}

void WinCap::setRecordPaused(bool on)
{
	if (capVideo) capVideo->setPaused(on);
}

void WinCap::beginVideoCapture()
{
	hideScreenImg = true;
	if (cutMask) cutMask->hideLabel = true;
	// 录制期间轮询光标（「选框鼠标跟随」/「鼠标高亮提醒」任一开启即需要）
	if (Setting::get()->getSelFollowMouse() || Setting::get()->getMouseHighlight())
		setTimer(recCursorTickMs, recCursorTimerId);
	// 默认不穿透，便于边录边标；穿透按钮后续再开
	refresh();
}

void WinCap::startLong()
{
    if (stage != CapStage::Adjust || !cutMask->hasRect()) return;
    stage = CapStage::Long;
    enterLiveStage();
    ensureNoMousePierce(); // 长截图全程不允许鼠标穿透
    capLong = std::make_unique<CapLong>(this);
    capLong->makeTool();
}

void WinCap::startVideo()
{
    if (stage != CapStage::Adjust || !cutMask || !cutMask->hasRect()) return;

    if (annotLive) {
        annotLive = false;
        clearAnnotSelection();
        if (toolSub) toolSub->hideTools();
        if (toolCap) toolCap->cancelSelect();
    }

    stage = CapStage::Video;
    // 录制只能针对**单块显示器**采集（采集链路按屏建复制纹理，跨屏录不全）：
    // 先把现有选区收进单块屏（跨屏框取的选区在此被收小），之后拖边 / 整体平移同样受该约束
    if (cutMask) {
        cutMask->setSingleMonitorMode(true);
        cutMask->snapToSingleMonitor();   // 跨屏框取的选区在这里收进单块屏
    }
    // 与 startLong 同路径：收起截图栏 + live 遮罩；开录前不穿透，选区仍可调
    enterLiveStage();
    if (cutMask) cutMask->hideLabel = false;
    clearMainToolUserPlaced();
    capVideo = std::make_unique<CapVideo>(this);
    capVideo->makeTool();
    // 选区能否移到某块屏，交由录制侧判定（不同显卡的屏切换不过去 → 选区停留在原屏边缘）；
    // 采集侧切换失败后也会把该屏排除，见 CapVideo::canSwitchToMonitor。生命周期随本会话。
    if (cutMask) {
        cutMask->canSwitchToMonitor = [this](const RECT& m) {
            return capVideo && capVideo->canSwitchToMonitor(m);
        };
    }
    // 进入录屏阶段的这一刻即把「系统按键」中 Scope::Global 的命令镜像到键盘钩子，**不等到开录**：
    // 录制时焦点通常在被录的程序上，而「开始/暂停录制」是同一条命令键 —— 若只挂"开录之后"，
    // 用该键开始录制这一步永远无法命中（键位那时尚未上钩子）。会话结束时统一撤销（见 dtor）。
    syncGlobalCommandKeys(CommandKeys::SurfaceRecord);
}

void WinCap::startMp4(bool useSpeaker, bool useMic)
{
    beginVideoCapture();
    if (capVideo) capVideo->startMp4(useSpeaker, useMic);
    if (Setting::get()->getShowKeystrokes()) startKeyOverlay();
}

void WinCap::startGif()
{
    beginVideoCapture();
    if (capVideo) capVideo->startGif();
    if (Setting::get()->getShowKeystrokes()) startKeyOverlay();
}

std::wstring WinCap::stopRecord()
{
	// 工具栏已先 hide；属性栏是独立顶层窗，须在 join 编码线程前立刻收掉，否则会空挂一阵
	if (toolSub) toolSub->hideTools();
	clearForcedToolId();
	annotLive = false;
	stopKeyOverlay();
	return capVideo ? capVideo->stop() : L"";
}

void WinCap::layoutLongTool()
{
    if (capLong) capLong->layoutTool();
}

void WinCap::longToggle()
{
    if (capLong) capLong->toggleCapture();
}

void WinCap::longPreviewPanChanged()
{
	if (capLong) capLong->onPreviewPanChanged();
}

bool WinCap::longSaveToFile()
{
    return capLong ? capLong->saveToFile() : false;
}

void WinCap::longCopyToClipboard()
{
    if (capLong) capLong->copyToClipboard();
}

void WinCap::hollowWin()
{
    HRGN rgn1 = CreateRectRgn(0, 0, (int)w, (int)h);
    auto& r = cutMask->maskRect;
    HRGN rgn2 = CreateRectRgn((int)r.left, (int)r.top, (int)r.right, (int)r.bottom);
    CombineRgn(rgn1, rgn1, rgn2, RGN_DIFF);
    DeleteObject(rgn2);
    if (SetWindowRgn(hwnd, rgn1, TRUE) == 0) {
        DeleteObject(rgn1);
    }
}

void WinCap::restoreWin()
{
    SetWindowRgn(hwnd, NULL, TRUE);
}

void WinCap::ensureNoMousePierce()
{
	if (!hwnd) return;
	restoreWin();
	if (isMouseTransparent) setMouseTransparent(false);
	else {
		LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
		if (ex & WS_EX_TRANSPARENT) {
			ex &= ~WS_EX_TRANSPARENT;
			SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
			SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		}
	}
}

void WinCap::setMouseTransparent(bool transparent)
{
    isMouseTransparent = transparent;
    if (!hwnd) return;
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (transparent) {
        exStyle |= WS_EX_LAYERED;
        exStyle |= WS_EX_TRANSPARENT;
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        isPress = false;
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        // 穿透后焦点已不在本软件：`` ` ``（鼠标穿透）由会话期间镜像到低级键盘钩子的绑定收回
        //（见 syncGlobalCommandKeys），此处不再额外注册热键。
    }
    else {
        exStyle &= ~WS_EX_TRANSPARENT;
        exStyle &= ~WS_EX_LAYERED;
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (capVideo) capVideo->syncPierceBtn(transparent);
}

// 文字识别：进程内 OcrService + 选区浮层
void WinCap::startOcr()
{
	// 再点一次同一个按钮 = 取消：收起识别面板（结果留在临时缓存里，下次点同一块选区直接就出来，
	// 不用再等一次识别）。以前这里会再跑一遍识别 —— 同步的几百毫秒，看着就像点了没反应/失败。
	// 若当前开的是「翻译」面板则不收起，直接切到文字识别。
	if (toolOcr && !toolOcr->translateMode()) {
		clearOcrPanel();
		return;
	}
	startOcrInternal(false);
}

void WinCap::startTranslate()
{
	// 与识别按钮一致：再点一次 = 收起翻译面板；当前开的是识别面板则直接切到翻译。
	if (toolOcr && toolOcr->translateMode()) {
		clearOcrPanel();
		return;
	}
	startOcrInternal(true);
}

bool WinCap::isTranslatePanel() const { return toolOcr && toolOcr->translateMode(); }
bool WinCap::isOcrPanel() const { return toolOcr && !toolOcr->translateMode(); }

namespace {
	// —— 翻译前把 OCR 碎片并成整行 ——
	// OCR 有时把一行切成好几个框（按词/短语），逐框送去翻译会得到"逐词直译 + 按原框摆放"的散句。
	// 这里把纵向重叠的碎片按视觉行合并：翻译和展示都用合并后的区域；识别面板的原始分块不受影响
	// （缓存里仍是原始区域，翻译只作用在这一次的展示上）。
	bool isCjkW(wchar_t c)
	{
		return (c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF)
			|| (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF)
			|| (c >= 0xFF00 && c <= 0xFFEF);
	}

	// 拼接两段：两侧都不是 CJK 才补一个空格（"open"+"source"→"open source"，"手绘的"+"风格"→不加）
	void appendFragment(std::wstring& dst, const std::wstring& add)
	{
		if (add.empty()) return;
		if (!dst.empty() && !isCjkW(dst.back()) && !isCjkW(add.front())) dst += L' ';
		dst += add;
	}

	OcrResult ocrResultMergedToLines(const OcrResult& in)
	{
		OcrResult out = in;
		if (in.regions.size() < 2) return out;   // 本来就一行一块：不动

		struct Line { float x0, y0, x1, y1; std::vector<size_t> idx; };
		std::vector<Line> lines;
		// 判"同一视觉行"：纵向重叠超过较矮一方的一半；或几乎不重叠、但两条中线挨得够近
		// （同一行里基线轻微上下漂移的情况也算）。否则一行会被拆成好几块。
		auto overlaps = [](const Line& ln, const OcrRegion& r) {
			const float lh = ln.y1 - ln.y0;
			const float m = std::min(lh, r.h);
			const float ov = std::min(ln.y1, r.y + r.h) - std::max(ln.y0, r.y);
			if (ov > 0.5f * m) return true;
			const float d = (r.y + r.h * 0.5f) - (ln.y0 + lh * 0.5f);
			return d <= 0.5f * m && -d <= 0.5f * m;
		};
		for (size_t i = 0; i < in.regions.size(); i++) {
			const auto& r = in.regions[i];
			Line* hit = nullptr;
			for (auto& ln : lines) if (overlaps(ln, r)) { hit = &ln; break; }
			if (!hit) {
				lines.push_back({ r.x, r.y, r.x + r.w, r.y + r.h, { i } });
			}
			else {
				hit->x0 = std::min(hit->x0, r.x);
				hit->y0 = std::min(hit->y0, r.y);
				hit->x1 = std::max(hit->x1, r.x + r.w);
				hit->y1 = std::max(hit->y1, r.y + r.h);
				hit->idx.push_back(i);
			}
		}

		std::vector<OcrRegion> merged;
		std::wstring plain;
		for (auto& ln : lines) {
			std::sort(ln.idx.begin(), ln.idx.end(),
				[&](size_t a, size_t b) { return in.regions[a].x < in.regions[b].x; });
			std::wstring text;
			for (size_t k : ln.idx) appendFragment(text, in.regions[k].text);
			OcrRegion reg;
			reg.text = text;
			reg.displayText = text;
			reg.x = ln.x0;
			reg.y = ln.y0;
			reg.w = std::max(2.f, ln.x1 - ln.x0);
			reg.h = std::max(2.f, ln.y1 - ln.y0);
			merged.push_back(reg);
			if (!plain.empty()) plain += L'\n';
			plain += text;
		}
		out.regions = std::move(merged);
		out.text = plain;
		return out;
	}
}

void WinCap::startOcrInternal(bool translateMode)
{
	if (!cutMask || !cutMask->hasRect()) return;
	const D2D1_RECT_F sel = cutMask->maskRect;

	std::vector<BYTE> pixels;
	int cw{ 0 }, ch{ 0 };
	// 识别/翻译只看**原始截图内容**，不带上用户画的标注 —— 标注会盖住原文字、干扰识别。
	if (!exportSelection(pixels, cw, ch, false)) return;

	// 建面板（含建窗、标注镜像、对齐选区）；翻译模式会先建一次空壳，识别完再填内容
	auto makePanel = [&](OcrResult res, std::vector<BYTE> px) {
		toolQrcode.reset();
		toolOcr = std::make_unique<ToolOcr>(this, std::move(res), translateMode, std::move(px), cw, ch);
		toolOcr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
		SetWindowLongPtr(toolOcr->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hwnd));
		// 标注要盖在识别结果上：浮层是独立置顶窗口，标注画在本窗口里，两个窗口没法互相穿插 ——
		// 把本窗口的标注"镜像"到浮层顶层，视觉上就等于"后画的标注盖在识别结果上面"。
		// 之后再叠一次选框（边框/控点/尺寸标签）：选框必须始终压在最上层，否则会被标注或识别结果盖住。
		{
			WinCap* self = this;
			toolOcr->setAnnotPainter([self](ID2D1DeviceContext* ctx) {
				self->paintShapes(ctx, false);
				if (self->cutMask) self->cutMask->paint(ctx);
			});
		}
		syncOcrPanel();
		// 这里**不能**给结果面板加「排除出捕获」：用户常要用别的截图/录屏工具把这个识别结果
		// 拍下来（反馈问题、分享），一旦排除掉，对方的截图里就只剩"被盖住的底图"，看着就像结果没了。
		// 面板随截图会话结束而销毁，不会残留到下一次截图里，所以不排除不会造成"拍到上次结果"。
	};

	// 翻译模式：**识别与翻译全部放后台** —— 面板先摆出「正在翻译…」，从点击到出结果提示一直都在，
	// 中途不再有任何一步去动这个面板（识别结果也不摆到前台），就不会出现"提示没了、结果还没来"。
	// 顺带好处：识别（首次可能一两秒）不再卡住界面。
	if (translateMode) {
		OcrResult pending;
		pending.ok = true;   // ok=true 才不会由 rebuildEditors 判成「识别失败」
		makePanel(std::move(pending), std::move(pixels));
		if (toolOcr) {
			// 置忙：从这一刻起提示条不允许被任何面板重建收掉，直到 finishOcrTranslate 落地结果
			toolOcr->setBusy(Lang::get(L"ocr.translating"));
			toolOcr->repaint();   // 主动刷一次面板，别只刷截图窗
		}
		refresh();

		const bool cached = g_ocrCacheValid && sameOcrRect(g_ocrCacheRect, sel);
		const OcrResult cachedResult = cached ? g_ocrCache : OcrResult{};
		const std::vector<BYTE> ocrPixels = toolOcr ? toolOcr->srcImage() : std::vector<BYTE>{};

		TranslateRequest req;
		req.provider = Setting::get()->getTranslateProvider();
		req.sourceLang = Setting::get()->getTranslateSourceLang();
		req.targetLang = Setting::get()->getTranslateTargetLang();
		const std::wstring trKey = translateCacheKey(req);

		// 选区没动 + 识别出来的文字一致 + 服务/方向一致 → 直接复用上次译文：
		// 识别与翻译请求都省掉（框选没动，结果必然一样）。
		if (cached) {
			const OcrResult mergedNow = ocrResultMergedToLines(cachedResult);
			if (g_trCacheValid && sameOcrRect(g_trCacheRect, sel)
				&& g_trCacheKey == trKey && g_trCacheText == mergedNow.text) {
				const TranslateResult tr = g_trCache;
				HWND h = hwnd;
				Ling::App::get()->dq.TryEnqueue([h, mergedNow, tr]() {
					auto* cap = WinCap::get();
					if (!cap || cap->hwnd != h) return;
					cap->finishOcrTranslate(mergedNow, tr);
				});
				return;
			}
		}

		HWND host = hwnd;
		std::thread([host, sel, cached, cachedResult, ocrPixels, cw, ch, req, trKey]() {
			// 后台线程里会碰 WinRT（系统 OCR），先初始化 apartment；已初始化会抛，忽略即可
			try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}

			const OcrResult raw = cached ? cachedResult
				: OcrService::instance().recognize(cw, ch, ocrPixels.data());
			const OcrResult merged = ocrResultMergedToLines(raw);

			std::vector<std::wstring> segments;
			segments.reserve(merged.regions.size());
			for (auto& reg : merged.regions) segments.push_back(reg.text);

			// 回 UI 线程干活（截图会话可能已经结束 → 用 host 校验；识别缓存只在这边写，避免跨线程）
			auto toUi = [host](std::function<void(WinCap&)> fn) {
				Ling::App::get()->dq.TryEnqueue([host, fn]() {
					auto* cap = WinCap::get();
					if (!cap || cap->hwnd != host) return;
					fn(*cap);
				});
			};

			if (!merged.ok || segments.empty()) {
				toUi([raw, merged, sel](WinCap& cap) {
					cap.storeOcrCache(raw, sel);
					if (!cap.toolOcr) return;
					const std::wstring msg = (merged.ok || merged.error.empty())
						? Lang::get(L"ocr.fail") : merged.error;
					cap.toolOcr->clearBusy();   // 识别就失败了，别再顶着"正在翻译…"
					cap.toolOcr->showBanner(msg, true);
					cap.toolOcr->repaint();
				});
				return;
			}

			TranslateService::instance().translateSegments(segments, req,
				[raw, merged, sel, trKey, toUi](const TranslateResult& tr) {
					toUi([raw, merged, sel, trKey, tr](WinCap& cap) {
						cap.storeOcrCache(raw, sel);
						cap.storeTranslateCache(merged, sel, trKey, tr);
						cap.finishOcrTranslate(merged, tr);
					});
				});
		}).detach();
		return;
	}

	// —— 识别（非翻译）模式：同步识别 + 直接出结果 ——
	OcrResult result;
	if (g_ocrCacheValid && sameOcrRect(g_ocrCacheRect, sel)) {
		result = g_ocrCache;   // 同一块选区：识别结果直接用临时缓存（省掉识别那几百毫秒）
	}
	else {
		result = OcrService::instance().recognize(cw, ch, pixels.data());
		g_ocrCache = result;
		g_ocrCacheRect = sel;
		g_ocrCacheValid = true;
	}

	// 先把碎片区域并成视觉行：OCR 常把一整行切成好几块，逐块显示会让拖选断断续续、
	// 文字上下位置也飘（每块各自居中）；并成整行后每行一个框，选择/复制连续、排版也齐。
	const OcrResult merged = ocrResultMergedToLines(result);

	makePanel(merged, std::move(pixels));
	if (merged.ok && !merged.text.empty()) {
		Ling::Util::setTextToClipboard(merged.text);
	}
	refresh();
}

void WinCap::storeOcrCache(const OcrResult& r, const D2D1_RECT_F& rect)
{
	g_ocrCache = r;
	g_ocrCacheRect = rect;
	g_ocrCacheValid = true;
}

void WinCap::storeTranslateCache(const OcrResult& merged, const D2D1_RECT_F& rect,
	const std::wstring& key, const TranslateResult& tr)
{
	if (!tr.ok || tr.partial) return;   // 失败/部分失败的结果不缓存，下一次仍要重试
	g_trCache = tr;
	g_trCacheRect = rect;
	g_trCacheKey = key;
	g_trCacheText = merged.text;
	g_trCacheValid = true;
}

void WinCap::finishOcrTranslate(const OcrResult& merged, const TranslateResult& tr)
{
	if (!toolOcr) return;
	// 先解忙：结果落地这一刻起，"正在翻译…"不再强制顶着，由下面按结果收/换
	toolOcr->clearBusy();
	std::vector<std::wstring> lines;
	if (tr.ok) {
		std::wistringstream ss(tr.text);
		std::wstring line;
		while (std::getline(ss, line)) lines.push_back(line);
		if (lines.empty()) lines.push_back(tr.text);
	}
	if (!tr.ok || lines.empty()) {
		// 翻译失败：退而显示识别出来的原文，至少让用户拿到 OCR 结果
		toolOcr->setResult(merged);
		toolOcr->showBanner(
			std::wstring(L"翻译失败：") + (tr.error.empty() ? L"未知错误" : tr.error), true);
		toolOcr->repaint();
		return;
	}
	// 剪贴板先写：它最多重试 ~100ms，夹在"收提示条"和"出结果"之间只会拉长空档
	Ling::Util::setTextToClipboard(tr.text);
	// 装译文（重建编辑框）与"收条"在同一次调用里完成，中间不留空档
	toolOcr->setTranslatedResult(merged, lines);
	// 部分行失败：结果照出（失败行已保留原文），单给一条提示，别让用户以为全翻好了
	if (tr.partial) toolOcr->showBanner(Lang::get(L"ocr.partial"), false);
	else toolOcr->clearBanner();
	toolOcr->repaint();   // 译文与"收提示条"一次刷出来
}

void WinCap::syncOcrPanel()
{
	if (!toolOcr || !cutMask || !cutMask->hasRect() || !toolOcr->hwnd) return;
	toolOcr->syncToMask(cutMask->maskRect, x, y, dpi);
}

void WinCap::clearOcrPanel()
{
	toolOcr.reset();
	refresh();
}

void WinCap::syncQrcodePanel()
{
	if (!toolQrcode || !cutMask || !cutMask->hasRect() || !toolQrcode->hwnd) return;
	toolQrcode->syncToMask(cutMask->maskRect, x, y, dpi);
}

void WinCap::startQrcode()
{
	std::vector<BYTE> pixels;
	int cw{ 0 }, ch{ 0 };
	if (!exportSelection(pixels, cw, ch)) return;
	auto text = Util::decodeQrCode(cw, ch, pixels.data());
	// 结果直接盖在选区上，不弹系统框；主工具栏保持可见
	if (!cutMask || !cutMask->hasRect()) {
		close();
		return;
	}
	toolQrcode = std::make_unique<ToolQrcode>(this, std::move(text));
	const int left = (int)std::lround(cutMask->maskRect.left);
	const int top = (int)std::lround(cutMask->maskRect.top);
	const int right = (int)std::lround(cutMask->maskRect.right);
	const int bottom = (int)std::lround(cutMask->maskRect.bottom);
	const int pw = (std::max)(1, right - left);
	const int ph = (std::max)(1, bottom - top);
	toolQrcode->setSize((float)pw / dpi, (float)ph / dpi);
	toolQrcode->setPosition(x + left, y + top);
	toolQrcode->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	SetWindowLongPtr(toolQrcode->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hwnd));
	syncQrcodePanel();
	syncOcrPanel();
	refresh();
}

// 记住本次选区（屏幕坐标）→ 全局热键「重复截图」据此再抓一块。
// 只在真正的截图收工时调用（keepOpen 的预览 / 演示画布不算）。
void WinCap::rememberLastRegion()
{
    if (!cutMask || !cutMask->hasRect()) return;
    const int rx = x + (int)std::lround(cutMask->maskRect.left);
    const int ry = y + (int)std::lround(cutMask->maskRect.top);
    const int rw = (int)std::lround(cutMask->maskRect.right - cutMask->maskRect.left);
    const int rh = (int)std::lround(cutMask->maskRect.bottom - cutMask->maskRect.top);
    Setting::get()->setLastCaptureRect(rx, ry, rw, rh);
}

void WinCap::saveToFile(bool keepOpen)
{
    std::vector<BYTE> pixels;
    int cw{ 0 }, ch{ 0 };
    if (!exportSelection(pixels, cw, ch)) return;
    if (!keepOpen) rememberLastRegion();
    auto setToolTopmost = [this](bool topmost) {
        if (!toolCap || !toolCap->hwnd) return;
        SetWindowPos(toolCap->hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    setToolTopmost(false);
    // 「保存格式」决定扩展名；文件名默认给「手动保存 / 焦点窗口文件名格式」的末段
    const std::wstring saveExt = Util::extensionFromSaveFormat(Setting::get()->getSaveFormat());
    const std::wstring defFmt = selectionFromElement_ ? Setting::get()->getFocusedWindowFormat()
                                                      : Setting::get()->getManualSaveFormat();
    const std::wstring defFallback = selectionFromElement_ ? Setting::defaultFocusedWindowFormat()
                                                           : Setting::defaultManualSaveFormat();
    std::wstring defName = Util::previewFileName(defFmt, defFallback, focusAppName_);
    if (auto slash = defName.find_last_of(L'/'); slash != std::wstring::npos) defName = defName.substr(slash + 1);
    auto path = Util::getSaveFilePath(hwnd, saveExt, defName);
    if (path.empty()) {
        setToolTopmost(true);
        return;
    }
    if (Util::saveToFile(path, cw, ch, pixels.data())) {
        // 记一条截图历史（只在会话收工时；keepOpen 的中间态复制不算）
        if (!keepOpen) Setting::get()->addHistoryItem(pixels, cw, ch, selectionFromElement_ ? L"focused" : L"capture");
        if (!keepOpen) close();
    }
    else {
        setToolTopmost(true);
    }
}

void WinCap::copyToClipboard(bool keepOpen)
{
    // 文字识别面板开着时，「复制 / 完成」复制的是识别出来的文字，而不是截图图片
    if (toolOcr && toolOcr->result().ok && !toolOcr->result().text.empty()) {
        Ling::Util::setTextToClipboard(toolOcr->result().text);
        if (!keepOpen) close();
        return;
    }
    std::vector<BYTE> pixels;
    int cw{ 0 }, ch{ 0 };
    if (!exportSelection(pixels, cw, ch)) return;
    if (!keepOpen) rememberLastRegion();
    Util::saveToClipboard(cw, ch, pixels.data());
    // 「截图后自动保存」：收工时静默落盘（点选窗口按「焦点窗口文件名格式」命名）
    if (!keepOpen) Util::quietSaveCapture(pixels, cw, ch, selectionFromElement_, focusAppName_);
    // 记一条截图历史（只在会话收工时；演示画布 keepOpen=true 不算）
    if (!keepOpen) Setting::get()->addHistoryItem(pixels, cw, ch, selectionFromElement_ ? L"focused" : L"capture");
    if (!keepOpen) close();
}

bool WinCap::exportSelection(std::vector<BYTE>& pixels, int& cw, int& ch, bool includeAnnot)
{
    if (!cutMask->hasRect() || !screenImg) return false;
    D2D1_SIZE_U size{};
    if (includeAnnot && annotLive && history && !history->shapes.empty()) {
        if (!getAnnotPixels(pixels, size, &cutMask->maskRect)) return false;
        // 合成结果可能带预乘 alpha，导出当不透明图
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }
    else {
        auto& maskRect = cutMask->maskRect;
        const UINT32 cutW = (UINT32)(maskRect.right - maskRect.left);
        const UINT32 cutH = (UINT32)(maskRect.bottom - maskRect.top);
        if (cutW == 0 || cutH == 0) return false;
        D2D1_BITMAP_PROPERTIES1 prop{
            .pixelFormat{ screenImg->GetPixelFormat() },
            .dpiX{ 96.0f }, .dpiY{ 96.0f },
            .bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
        };
        ComPtr<ID2D1Bitmap1> cpuBmp;
        auto hr = Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(cutW, cutH), nullptr, 0, &prop, cpuBmp.GetAddressOf());
        if (FAILED(hr)) return false;
        auto start = D2D1::Point2U(0, 0);
        auto rect = D2D1::RectU((UINT32)maskRect.left, (UINT32)maskRect.top, (UINT32)maskRect.left + cutW, (UINT32)maskRect.top + cutH);
        if (FAILED(cpuBmp->CopyFromBitmap(&start, screenImg.Get(), &rect))) return false;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        const UINT32 rowBytes = cutW * 4;
        pixels.resize((size_t)rowBytes * cutH);
        for (UINT32 row = 0; row < cutH; row++) {
            auto dst = pixels.data() + (size_t)row * rowBytes;
            CopyMemory(dst, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
            for (UINT32 i = 3; i < rowBytes; i += 4) dst[i] = 255;
        }
        cpuBmp->Unmap();
        size = D2D1::SizeU(cutW, cutH);
    }
    cw = (int)size.width;
    ch = (int)size.height;
    if (showCursor && cursorValid && cursorW > 0 && cursorH > 0 && cw > 0 && ch > 0)
        compositeCursor(pixels, cw, ch, (int)cutMask->maskRect.left, (int)cutMask->maskRect.top);
    return cutMask->applyExportEffects(pixels, cw, ch);
}

// 「显示光标」：会话开始抓一次真实系统光标。老式光标只有 AND 掩码没 alpha，
// DrawIconEx 画进 32bpp DIB 时 alpha 全 0 —— 得自己按掩码补。
void WinCap::captureCursorSnapshot()
{
    cursorValid = false;
    cursorBmp.Reset();
    cursorPix.clear();
    cursorW = cursorH = 0;
    CURSORINFO ci{ sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    // 优先用会话开始前抓到的真实光标（见 primeCursorForCapture）；拿不到再退回当前光标
    HCURSOR useCur = g_cursorForCapture;
    g_cursorForCapture = nullptr;
    ICONINFO ii{};
    if (!useCur || !GetIconInfo(useCur, &ii)) {
        useCur = ci.hCursor;
        if (!GetIconInfo(useCur, &ii)) return;
    }
    auto cleanup = [&] {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    };

    int width = 32, height = 32;
    if (ii.hbmColor) {
        BITMAP bm{};
        if (GetObject(ii.hbmColor, sizeof(bm), &bm)) { width = bm.bmWidth; height = bm.bmHeight; }
    }
    else if (ii.hbmMask) {
        BITMAP bm{};
        if (GetObject(ii.hbmMask, sizeof(bm), &bm)) { width = bm.bmWidth; height = bm.bmHeight / 2; }
    }
    width = std::clamp(width, 1, 256);
    height = std::clamp(height, 1, 256);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        cleanup();
        return;
    }
    HGDIOBJ old = SelectObject(memDc, dib);
    memset(bits, 0, (size_t)width * (size_t)height * 4);
    DrawIconEx(memDc, 0, 0, useCur, width, height, 0, nullptr, DI_NORMAL);
    cursorPix.assign((size_t)width * (size_t)height * 4, 0);
    memcpy(cursorPix.data(), bits, cursorPix.size());

    // 补 alpha：掩码位 = 1 ⇒ 该点透明；掩码两种布局都试，用"本体必须不透明"挑对的
    bool anyAlpha = false;
    for (size_t i = 3; i < cursorPix.size(); i += 4) { if (cursorPix[i]) { anyAlpha = true; break; } }
    BITMAP bmMask{};
    const bool haveMask = ii.hbmMask && GetObject(ii.hbmMask, sizeof(bmMask), &bmMask)
        && bmMask.bmWidth >= width && bmMask.bmHeight >= height;
    if (!anyAlpha && haveMask) {
        BITMAPINFO mi{};
        mi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        mi.bmiHeader.biWidth = bmMask.bmWidth;
        mi.bmiHeader.biHeight = bmMask.bmHeight;   // bottom-up
        mi.bmiHeader.biPlanes = 1;
        mi.bmiHeader.biBitCount = 1;
        mi.bmiHeader.biCompression = BI_RGB;
        const int stride = ((bmMask.bmWidth + 31) / 32) * 4;
        std::vector<BYTE> maskBits((size_t)stride * bmMask.bmHeight, 0);
        if (GetDIBits(memDc, ii.hbmMask, 0, bmMask.bmHeight, maskBits.data(), &mi, DIB_RGB_COLORS) != 0) {
            const std::vector<BYTE> original = cursorPix;
            const int total = width * height;
            for (int cand = 0; cand < 2; ++cand) {
                const int firstRow = (cand == 0) ? 0 : (bmMask.bmHeight - height);
                cursorPix = original;
                int opaque = 0, mismatch = 0;
                for (int y = 0; y < height; ++y) {
                    const int br = firstRow + (height - 1 - y);
                    if (br < 0 || br >= bmMask.bmHeight) continue;
                    const BYTE* row = maskBits.data() + (size_t)br * stride;
                    BYTE* line = cursorPix.data() + (size_t)y * width * 4;
                    for (int x = 0; x < width; ++x) {
                        const bool transparent = (row[x >> 3] >> (7 - (x & 7))) & 1;
                        const bool hasRgb = (line[x * 4] | line[x * 4 + 1] | line[x * 4 + 2]) != 0;
                        if (transparent) {
                            line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = line[x * 4 + 3] = 0;
                            if (hasRgb) ++mismatch;
                        }
                        else {
                            line[x * 4 + 3] = 255;
                            ++opaque;
                        }
                    }
                }
                if (mismatch == 0 && opaque > 0 && opaque <= total * 3 / 4) break;
                cursorPix = original;
            }
        }
    }

    SelectObject(memDc, old);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    cleanup();

    cursorW = width;
    cursorH = height;
    cursorHotspot = POINT{ (LONG)ii.xHotspot, (LONG)ii.yHotspot };
    cursorScreenPos = ci.ptScreenPos;
    for (size_t i = 3; i < cursorPix.size(); i += 4) {
        if (cursorPix[i]) { cursorValid = true; break; }
    }
}

void WinCap::toggleShowCursor()
{
    showCursor = !showCursor;
    // 光标快照只在会话开始时抓一次（那时鼠标下面是真实窗口，见 primeCursorForCapture）。
    // 这里**不**补抓 —— 此刻鼠标下的已经是我们自己的覆盖窗，补抓只会抓到"我们的框选光标"。
    refresh();
}

void WinCap::paintCursor(ID2D1DeviceContext* ctx)
{
    if (!showCursor || !cursorValid || cursorW <= 0 || cursorH <= 0 || !ctx) return;
    if (!cursorBmp) {
        // D2D 用预乘 alpha：把 straight BGRA 转一份出来建位图
        std::vector<BYTE> pm(cursorPix.size());
        for (size_t i = 0; i + 3 < pm.size(); i += 4) {
            const BYTE a = cursorPix[i + 3];
            pm[i] = (BYTE)(cursorPix[i] * a / 255);
            pm[i + 1] = (BYTE)(cursorPix[i + 1] * a / 255);
            pm[i + 2] = (BYTE)(cursorPix[i + 2] * a / 255);
            pm[i + 3] = a;
        }
        D2D1_BITMAP_PROPERTIES1 prop{
            .pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            .dpiX{ 96.0f }, .dpiY{ 96.0f },
            .bitmapOptions{ D2D1_BITMAP_OPTIONS_NONE }
        };
        Ling::D2D::get()->deviceContext->CreateBitmap(
            D2D1::SizeU((UINT32)cursorW, (UINT32)cursorH), pm.data(),
            (UINT32)cursorW * 4, &prop, cursorBmp.GetAddressOf());
    }
    if (!cursorBmp) return;
    POINT local = cursorScreenPos;
    ScreenToClient(hwnd, &local);
    const float l = (float)(local.x - cursorHotspot.x);
    const float t = (float)(local.y - cursorHotspot.y);
    ctx->DrawBitmap(cursorBmp.Get(), D2D1::RectF(l, t, l + cursorW, t + cursorH),
        1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// ---- 「录制中显示按键」----
void WinCap::startKeyOverlay()
{
	auto mod = GetModuleHandleW(nullptr);
	// WH_KEYBOARD_LL / WH_MOUSE_LL 的钩子过程在本进程、装它的线程（UI 线程）上执行，不需要注入 DLL。
	// 键盘 + 鼠标都要：教程里"点了哪里/按了什么键"同等重要
	if (!g_keyHook) g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyHookProc, mod, 0);
	if (!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, mod, 0);
}

void WinCap::stopKeyOverlay()
{
	if (g_keyHook) {
		UnhookWindowsHookEx(g_keyHook);
		g_keyHook = nullptr;
	}
	if (g_mouseHook) {
		UnhookWindowsHookEx(g_mouseHook);
		g_mouseHook = nullptr;
	}
	keyOverlayText_.clear();
	keyOverlayUntil_ = 0;
	killTimer(keyOverlayTimerId);
}

void WinCap::onGlobalKeyCombo(const std::wstring& text)
{
	if (text.empty()) return;
	keyOverlayText_ = text;
	keyOverlayUntil_ = GetTickCount64() + 1500;
	setTimer(1500, keyOverlayTimerId);   // 每来一个键就续期，停手 1.5s 后消失
	refresh();
}

void WinCap::onKeyOverlayTimer()
{
	killTimer(keyOverlayTimerId);
	keyOverlayUntil_ = 0;
	keyOverlayText_.clear();
	refresh();
}

void WinCap::paintKeyOverlay(ID2D1DeviceContext* ctx)
{
	if (!ctx || keyOverlayText_.empty() || GetTickCount64() > keyOverlayUntil_) return;
	auto layout = Ling::D2D::makeTextLayout(keyOverlayText_, 18.f * dpi);
	if (!layout) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(layout->GetMetrics(&tm))) return;
	const float padX = 14.f * dpi, padY = 9.f * dpi;
	// 画在选区左下角内侧；没有选区时贴屏幕左下角
	float anchorLeft = 20.f * dpi;
	float anchorBottom = h - 20.f * dpi;
	if (cutMask && cutMask->hasRect()) {
		anchorLeft = cutMask->maskRect.left + 14.f * dpi;
		anchorBottom = cutMask->maskRect.bottom - 14.f * dpi;
	}
	D2D1_RECT_F rect{
		anchorLeft,
		anchorBottom - (tm.height + padY * 2.f),
		anchorLeft + tm.width + padX * 2.f,
		anchorBottom
	};
	const float r = 8.f * dpi;
	ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, r, r), brushBg.Get());
	ctx->DrawTextLayout(D2D1::Point2F(rect.left + padX, rect.top + padY), layout.Get(), brushText.Get());
}

void WinCap::compositeCursor(std::vector<BYTE>& pixels, int w, int h, int originX, int originY)
{
    if (!cursorValid || cursorW <= 0 || cursorH <= 0) return;
    POINT local = cursorScreenPos;
    ScreenToClient(hwnd, &local);
    const int x0 = local.x - cursorHotspot.x - originX;
    const int y0 = local.y - cursorHotspot.y - originY;
    for (int y = 0; y < cursorH; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= h) continue;
        for (int x = 0; x < cursorW; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= w) continue;
            const BYTE* s = cursorPix.data() + ((size_t)y * cursorW + x) * 4;
            const BYTE a = s[3];
            if (a == 0) continue;
            BYTE* d = pixels.data() + ((size_t)dy * w + dx) * 4;
            if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; continue; }
            const int inv = 255 - a;
            d[0] = (BYTE)((s[0] * a + d[0] * inv) / 255);
            d[1] = (BYTE)((s[1] * a + d[1] * inv) / 255);
            d[2] = (BYTE)((s[2] * a + d[2] * inv) / 255);
            d[3] = 255;
        }
    }
}
