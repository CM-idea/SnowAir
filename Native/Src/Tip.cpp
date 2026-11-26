#include "pch.h"
#include <CommCtrl.h>
#include <cmath>
#include "Tip.h"
#include "App.h"
#include "Tool/DpiUtil.h"

Tip::Tip(Ling::WinBase* win) :win(win)
{
	// 整个进程只需要注册一次 tooltip 控件类
	static bool inited{ false };
	if (!inited) {
		INITCOMMONCONTROLSEX iccex{ sizeof(iccex), ICC_BAR_CLASSES };
		InitCommonControlsEx(&iccex);
		inited = true;
	}
	auto hIns = GetModuleHandle(nullptr);
	// 创建即带 WS_EX_LAYERED：showNow 里那次“不可见排版测量”靠 alpha 0 隐身；
	// 运行期再加分层样式不生效
	tipHwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED, TOOLTIPS_CLASS, nullptr,
		WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, win->hwnd, nullptr, hIns, nullptr);
	SetLayeredWindowAttributes(tipHwnd, 0, 255, LWA_ALPHA);   // 常态不透明
	TOOLINFOW ti{};
	ti.cbSize = TTTOOLINFOW_V2_SIZE;
	// TTF_TRACK：不让控件自己跟着鼠标跑，显示时机和位置全由我们控制
	// TTF_ABSOLUTE：TTM_TRACKPOSITION 给的坐标就是提示窗口左上角，系统不再另行偏移
	ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
	ti.hwnd = win->hwnd;
	ti.hinst = hIns;
	ti.uId = 0;
	ti.lpszText = const_cast<LPWSTR>(L" ");
	SendMessage(tipHwnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
	// 延时由我们自己的定时器管，控件这边的初始延时清零
	SendMessage(tipHwnd, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELONG(0, 0));
	// 字体和排版都要等知道锚点落在哪块屏上才能定，统一推迟到 showNow。
	timerTok = win->onTimer.add([this](UINT id) {
		if (id != timerId) return;
		this->win->killTimer(timerId);
		// 等待期间提示被取消（鼠标已经离开）时 owner 是空的
		if (!owner) return;
		showNow();
	});
}

Tip::~Tip()
{
	if (tipFont_) { DeleteObject(tipFont_); tipFont_ = nullptr; }
	win->onTimer.remove(timerTok);
	if (owner) win->killTimer(timerId);
	if (tipHwnd && IsWindow(tipHwnd)) {
		DestroyWindow(tipHwnd);
	}
}

void Tip::bind(Ling::Button* btn, const std::wstring& text)
{
	// btn 挂在宿主窗口的节点树上，与 Tip 同生死（都随窗口销毁），捕获裸指针安全
	btn->onEnter.add([this, text](Ling::Button* b) {
		if (suppressed && suppressed()) return;
		showAbove(b, text);
	});
	btn->onLeave.add([this](Ling::Button* b) { hide(b); });
}

void Tip::showAbove(Ling::Node* owner, const std::wstring& text)
{
	// Node 的 x/y/w/h 是布局算完的窗口内绝对坐标（物理像素），加上窗口位置就是屏幕坐标。
	// 但滚动区只平移内容视觉、不改子节点的布局坐标，所以要把祖先 ScrollerBox 的 scrollY 扣回来，
	// 否则滚动后提示会留在原来的（未滚动）位置。
	float scrolled = 0.f;
	for (Ling::Node* p = owner->parent; p; p = p->parent) {
		if (auto* sb = dynamic_cast<Ling::ScrollerBox*>(p))
			scrolled += sb->getScrollY();
	}
	const auto sx = win->x + owner->x + owner->w / 2.f;
	const auto top = win->y + owner->y - scrolled;
	// 内缩量也按锚点所在显示器缩放，避免在 300% 屏上只缩进 1/3。
	POINT anchorPt{ (LONG)std::lround(sx), (LONG)std::lround(top) };
	const auto sy = top + anchorInset * DpiUtil::dpiForPoint(anchorPt);
	// 控件底边：上方放不下时提示翻到它下面
	showAtInternal(owner, sx, sy, top + owner->h, text);
}

void Tip::showAt(Ling::Node* owner, float screenX, float screenY, const std::wstring& text)
{
	// 调用方只给了锚点，没有控件底边；翻到下方时按锚点下再让一点算
	const POINT anchorPt{ (LONG)std::lround(screenX), (LONG)std::lround(screenY) };
	const auto bottomY = screenY + 2.f * anchorInset * DpiUtil::dpiForPoint(anchorPt);
	showAtInternal(owner, screenX, screenY, bottomY, text);
}

void Tip::showAtInternal(Ling::Node* owner, float screenX, float screenY, float bottomY,
	const std::wstring& text)
{
	if (this->owner == owner) {
		// 同一个归属：只更新内容与位置。已经显示出来的就立刻跟上（滑块提示跟着鼠标走靠这条），
		// 还在等那 1 秒的则保持计时不重置，到点后按最新位置显示。
		this->text = text;
		anchorX = screenX;
		anchorY = screenY;
		anchorBottomY = bottomY;
		if (visible) showNow();
		return;
	}
	// 换归属：旧提示立刻收掉，1 秒重新计时
	hide();
	this->owner = owner;
	this->text = text;
	anchorX = screenX;
	anchorY = screenY;
	anchorBottomY = bottomY;
	win->setTimer(delayMs, timerId);
}

void Tip::hide(Ling::Node* owner)
{
	if (this->owner == owner) hide();
}

void Tip::hide()
{
	if (owner) {
		win->killTimer(timerId);
		owner = nullptr;
	}
	text.clear();
	// 立刻从屏上拿掉：仅 TRACKACTIVATE 有时仍会闪一下淡出
	if (tipHwnd && IsWindow(tipHwnd)) {
		if (visible) {
			TOOLINFOW ti{};
			ti.cbSize = TTTOOLINFOW_V2_SIZE;
			ti.hwnd = win->hwnd;
			ti.uId = 0;
			SendMessage(tipHwnd, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
			SendMessage(tipHwnd, TTM_ACTIVATE, FALSE, 0);
		}
		ShowWindow(tipHwnd, SW_HIDE);
	}
	visible = false;
}

void Tip::excludeFromCapture()
{
	App::excludeFromCapture(tipHwnd);
}

SIZE Tip::bubbleSize()
{
	// 控件自己按当前字体、文字、内边距算出来的尺寸。必须已经激活过（哪怕是 alpha 0 的激活），
	// 否则 comctl32 内部状态还没建好。
	SIZE sz{};
	if (SendMessage(tipHwnd, TTM_GETBUBBLESIZE, 0, (LPARAM)&sz) && sz.cx > 0 && sz.cy > 0)
		return sz;
	// 兜底：窗口刚按同样的字体/文字排过版，矩形就是它的真实尺寸
	RECT rc{};
	if (GetWindowRect(tipHwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top)
		return SIZE{ rc.right - rc.left, rc.bottom - rc.top };
	return SIZE{ 0, 0 };
}

void Tip::applyDpiForPoint(float screenX, float screenY)
{
	const POINT pt{ (LONG)std::lround(screenX), (LONG)std::lround(screenY) };
	const float dpi = DpiUtil::dpiForPoint(pt);
	if (std::fabs(dpi - tipDpi_) < 0.01f && tipFont_) return;
	tipDpi_ = dpi;
	if (tipFont_) { DeleteObject(tipFont_); tipFont_ = nullptr; }
	LOGFONTW lf{};
	NONCLIENTMETRICSW ncm{ sizeof(ncm) };
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
		lf = ncm.lfStatusFont;
	}
	else {
		lf.lfWeight = FW_NORMAL;
		wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
	}
	// 标准 tooltip 字号是 9pt；按锚点所在显示器的 DPI 换算出像素高度。
	lf.lfHeight = -MulDiv(9, (int)std::lround(dpi * 96.f), 72);
	tipFont_ = CreateFontIndirectW(&lf);
	if (tipFont_ && tipHwnd) SendMessageW(tipHwnd, WM_SETFONT, (WPARAM)tipFont_, TRUE);
}

void Tip::place(int w, int h, int& outX, int& outY) const
{
	const POINT anchorPt{ (LONG)std::lround(anchorX), (LONG)std::lround(anchorY) };
	RECT wa{};
	MONITORINFO mi{ sizeof(mi) };
	if (GetMonitorInfoW(MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST), &mi)) {
		wa = mi.rcWork;
	}
	else {
		wa = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
	}

	const int ax = (int)std::lround(anchorX);
	const int ay = (int)std::lround(anchorY);
	const int gap = (int)std::lround(2.f * DpiUtil::dpiForPoint(anchorPt));

	// 默认：底边中点贴锚点（气泡在锚点上方）
	int x = ax - w / 2;
	int y = ay - h;
	// 上方越出工作区 → 翻到锚点所在控件下方
	if (y < wa.top) {
		y = (int)std::lround(anchorBottomY) + gap;
	}
	// 夹取到工作区：副屏负坐标同样适用，先夹右/下再夹左/上
	if (x + w > wa.right) x = wa.right - w;
	if (x < wa.left) x = wa.left;
	if (y + h > wa.bottom) y = wa.bottom - h;
	if (y < wa.top) y = wa.top;
	outX = x;
	outY = y;
}

void Tip::showNow()
{
	if (!tipHwnd || !IsWindow(tipHwnd) || !owner) return;

	const int ax = (int)std::lround(anchorX);
	const int ay = (int)std::lround(anchorY);

	// 1) 提示还不可见、或锚点已经跨到另一块屏时，先把窗口挪到锚点让 DPI 落定；
	//    可见且同屏的普通更新（滑块数值提示跟着鼠标走）不要再挪 —— 那会先跳到锚点、
	//    再跳到最终位置，屏上就是可见的一帧抖动。
	const POINT anchorPt{ ax, ay };
	if (!visible
		|| std::fabs(DpiUtil::dpiForWindow(tipHwnd) - DpiUtil::dpiForPoint(anchorPt)) > 0.01f) {
		SetWindowPos(tipHwnd, nullptr, ax, ay, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	// 2) 按锚点所在显示器应用字体
	applyDpiForPoint(anchorX, anchorY);

	// 3) 换文字
	TOOLINFOW ti{};
	ti.cbSize = TTTOOLINFOW_V2_SIZE;
	ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
	ti.hwnd = win->hwnd;
	ti.uId = 0;
	ti.lpszText = const_cast<LPWSTR>(text.c_str());
	SendMessage(tipHwnd, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);

	// 4) 量最终尺寸：让控件按最终字体/文字自己排一次版，只量这一次。
	SIZE size{ 0, 0 };
	if (visible) {
		// 已经在屏上：控件收到新文字后已按同一套规则重排，直接问尺寸
		size = bubbleSize();
	}
	else {
		// 让这次「不可见排版」真的不可见：系统 tooltip 显示时会走淡入动画
		// （SPI_GETTOOLTIPANIMATION 默认开启），动画会把分层窗的 alpha 从 0 逐步推到 255。
		// 只设 alpha 0 的话，量尺寸这一下会以 (ax, ay) 为左上角闪出来 —— 工具栏上的表现
		// 就是「气泡先在按钮中间出现、再跳到按钮上方」。空窗口区域让它一个像素都画不出来，
		// 但窗口仍然是「显示」状态，comctl32 会照常排版、TTM_GETBUBBLESIZE 照常可量。
		HRGN probeRgn = CreateRectRgn(0, 0, 0, 0);
		if (!probeRgn || !SetWindowRgn(tipHwnd, probeRgn, FALSE)) {
			if (probeRgn) DeleteObject(probeRgn);
		}
		SetLayeredWindowAttributes(tipHwnd, 0, 0, LWA_ALPHA);
		SendMessage(tipHwnd, TTM_TRACKPOSITION, 0, MAKELPARAM(ax, ay));
		SendMessage(tipHwnd, TTM_ACTIVATE, TRUE, 0);
		SendMessage(tipHwnd, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
		size = bubbleSize();
		SendMessage(tipHwnd, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
		SendMessage(tipHwnd, TTM_ACTIVATE, FALSE, 0);
		ShowWindow(tipHwnd, SW_HIDE);
		// 先恢复区域 / alpha 再走后面的尺寸兜底：量不到尺寸时窗口也不能停在“空区域”状态
		SetWindowRgn(tipHwnd, nullptr, TRUE);
		SetLayeredWindowAttributes(tipHwnd, 0, 255, LWA_ALPHA);
	}
	if (size.cx <= 0 || size.cy <= 0) return;

	// 5) 用最终尺寸算最终位置：贴锚点上方，放不下翻到下方，再夹取到所在显示器工作区
	int x = 0, y = 0;
	place(size.cx, size.cy, x, y);

	// 6) 一次性把尺寸和位置定死，然后显示。此后不再有任何“显示后校正”，
	//    新建、复用、跨屏都走这一条。
	SetWindowPos(tipHwnd, HWND_TOPMOST, x, y, size.cx, size.cy,
		SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	SendMessage(tipHwnd, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
	if (!visible) {
		SendMessage(tipHwnd, TTM_ACTIVATE, TRUE, 0);
		SendMessage(tipHwnd, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
		visible = true;
	}
	// 提上最顶层：建窗时的 WS_EX_TOPMOST 只保证它在置顶那一群里，
	// 而工具窗口个个都是置顶（ToolSub 的提示正好落在 ToolMain 上），
	// 不每次重新抢到同组最前面就会被它们盖住。SWP_NOACTIVATE：别抢焦点。
	SetWindowPos(tipHwnd, HWND_TOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
