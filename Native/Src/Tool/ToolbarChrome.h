#pragma once
#include <d2d1_1.h>
#include <cmath>
#include "ToolbarTheme.h"

namespace Ling { class Node; }

namespace ToolbarChrome
{
	// 圆角栏：抗锯齿填充 + 内收描边（2× 超采样柔化，无投影、不改间距）
	void paintRoundBar(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, float radiusPx,
		ID2D1Brush* bg, float borderWidthPx);

	// 选区信息条：圆角底，无投影
	void paintInfoBar(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, float radiusPx,
		ID2D1Brush* bg);

	// 圆角栏 + 箭头，同一套柔化
	void paintBarWithCaret(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, float radiusPx,
		ID2D1Brush* bg, float borderWidthPx, float caretX, float tipY, float baseY);

	// 属性栏 / 色板 / 悬停气泡共用外壳：tipDown=false 尖朝上，true 尖朝下
	// borderWidthPx 传 0 即无描边（悬停气泡不要边框）。
	void paintPropBubble(ID2D1DeviceContext* ctx, float winW, float winH, float dpi,
		ID2D1Brush* bg, float caretX, bool tipDown,
		float borderWidthPx = ToolbarTheme::borderWidth);

	// 内容区相对窗口的逻辑边距（与 paintPropBubble 配套）
	struct ContentPad { float top, bottom; };
	inline ContentPad propBubbleContentPad(bool tipDown)
	{
		const float pad = ToolbarTheme::shadowPad;
		const float caret = ToolbarTheme::caretSize;
		return tipDown ? ContentPad{ pad, pad + caret } : ContentPad{ pad + caret, pad };
	}

	// 相对参考条上下两侧，选空间够的一侧；preferBelow=true 时优先下弹，false 优先上弹
	// tipDown=true 表示窗口在参考条上方、箭头朝下
	inline bool preferTipDown(float refTop, float refBottom, float popupH, float gap,
		float workTop, float workBottom, bool preferBelow = true)
	{
		const float need = popupH + gap;
		const float spaceAbove = refTop - workTop;
		const float spaceBelow = workBottom - refBottom;
		if (preferBelow) {
			if (spaceBelow >= need) return false;
			if (spaceAbove >= need || spaceAbove > spaceBelow) return true;
			return false;
		}
		if (spaceAbove >= need) return true;
		if (spaceBelow >= need || spaceBelow > spaceAbove) return false;
		return true;
	}

	// 参考矩形附近的显示器工作区（失败则退回 ref）
	inline RECT workAreaNear(RECT ref)
	{
		MONITORINFO mi{ sizeof(MONITORINFO) };
		auto mon = MonitorFromRect(&ref, MONITOR_DEFAULTTONEAREST);
		if (!mon || !GetMonitorInfo(mon, &mi)) return ref;
		return mi.rcWork;
	}

	// 属性气泡统一落点：物理像素；desiredX / arrowAnchorX 为屏坐标
	struct BubblePlacement {
		int x{ 0 };
		int y{ 0 };
		bool tipDown{ false };
		float arrowX{ 0.f };
		bool tipFlipped{ false };
		bool arrowChanged{ false };
		bool moved{ false };
		bool needPaint{ false };
	};

	inline BubblePlacement placeBubble(
		const RECT& workArea,
		float refTop, float refBottom,
		float popupW, float popupH, float gap,
		float desiredX, float arrowAnchorX,
		bool preferBelow,
		bool tipDownPrev, float arrowXPrev,
		int curX, int curY)
	{
		BubblePlacement out{};
		out.tipDown = preferTipDown(refTop, refBottom, popupH, gap,
			(float)workArea.top, (float)workArea.bottom, preferBelow);
		out.tipFlipped = (out.tipDown != tipDownPrev);
		const float py = out.tipDown ? (refTop - gap - popupH) : (refBottom + gap);
		int upperX = workArea.right - (int)popupW;
		if (upperX < workArea.left) upperX = workArea.left;
		int finalX = (int)desiredX;
		if (finalX < workArea.left) finalX = workArea.left;
		if (finalX > upperX) finalX = upperX;
		out.x = finalX;
		out.y = (int)py;
		out.arrowX = arrowAnchorX - (float)finalX;
		out.arrowChanged = std::fabs(out.arrowX - arrowXPrev) > 0.5f;
		out.moved = (out.x != curX || out.y != curY);
		out.needPaint = out.tipFlipped || out.arrowChanged;
		return out;
	}

	// 内容区 Top/Bottom 边距随箭头方向切换
	void applyPropBubbleContentPad(Ling::Node* contentNode, bool tipDown);
}
