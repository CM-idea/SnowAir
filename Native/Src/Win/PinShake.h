#pragma once
#include <windows.h>

// 贴图 / 贴屏「内容重复」提示：把窗口抬到最前，再左右抖几下。
// 抖动走窗口定时器分步移动（不 Sleep 阻塞 UI 线程），调用方只需在 onTimer 里转发 tick：
//   if (id == PinShake::kTimerId) PinShake::tick(shakeState, hwnd);
// 鼠标按下时调用 cancel()，避免用户已经开始拖动、抖动收尾又把窗口弹回原位。
namespace PinShake {
	// 贴在别的窗口里，窗口内定时器 id 与 WinPin（101/102、210~214）/ ClipPin（0x71）都不冲突
	constexpr UINT kTimerId = 103;
	constexpr UINT kIntervalMs = 26;
	constexpr int kAmplitude = 8;   // 起始左右偏移（物理像素）
	constexpr int kSteps = 8;       // 左右各 4 次（幅度递减），随后归位

	struct State {
		int baseX{ 0 };
		int baseY{ 0 };
		int step{ 0 };
		bool active{ false };
		bool restoreTopMost{ true };   // 抖完 / 取消后要不要恢复成置顶（原来不是置顶就放回去）
	};

	// 把窗口抬到最前（临时置顶，抖完按原状态恢复）：
	// 非置顶的贴图若只在非置顶组里抬，用户可能根本看不见它被抖。
	inline void raiseToFront(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) return;
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}

	// 开始抖动：先抬到最前，再从窗口当前位置左右晃。连续重复贴图时沿用上次的基准位置，
	// 不会因为上一轮还没抖完就把窗口越抖越偏。
	inline void start(State& s, HWND hwnd, bool topMost)
	{
		if (!hwnd || !IsWindow(hwnd)) return;
		raiseToFront(hwnd);
		if (s.active) {
			KillTimer(hwnd, WM_APP + kTimerId);
		}
		else {
			RECT rc{};
			if (!GetWindowRect(hwnd, &rc)) return;
			s.baseX = rc.left;
			s.baseY = rc.top;
		}
		s.restoreTopMost = topMost;
		s.step = 0;
		s.active = true;
		SetTimer(hwnd, WM_APP + kTimerId, kIntervalMs, nullptr);
	}

	// 抖完 / 中途取消：归位并按原状态放回置顶组。
	// 原本就置顶的窗口不再动 z 序 —— 抖之前刚把识别浮层抬到它上面，这里再抬一次会把浮层压下去。
	inline void settle(State& s, HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) return;
		if (s.restoreTopMost) {
			SetWindowPos(hwnd, nullptr, s.baseX, s.baseY, 0, 0,
				SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		else {
			SetWindowPos(hwnd, HWND_NOTOPMOST, s.baseX, s.baseY, 0, 0,
				SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}

	// 定时器每拍调用一次：返回 true 表示抖动还没结束。
	inline bool tick(State& s, HWND hwnd)
	{
		if (!s.active) return false;
		if (!hwnd || !IsWindow(hwnd)) {
			s.active = false;
			return false;
		}
		++s.step;
		if (s.step > kSteps) {
			s.active = false;
			KillTimer(hwnd, WM_APP + kTimerId);
			settle(s, hwnd);
			return false;
		}
		const int dir = (s.step & 1) ? 1 : -1;
		const int mag = kAmplitude - ((s.step - 1) / 2) * 2;
		SetWindowPos(hwnd, nullptr, s.baseX + dir * mag, s.baseY, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		return true;
	}

	// 中途取消（用户按下鼠标 / 开始拖动）：立刻回到底座位置并停表。
	inline void cancel(State& s, HWND hwnd)
	{
		if (!s.active) return;
		s.active = false;
		settle(s, hwnd);
		KillTimer(hwnd, WM_APP + kTimerId);
	}
}
