#pragma once
#include <d2d1.h>
#include <wrl/client.h>

// 四角可独立圆角的矩形。
// D2D 的 DrawRoundedRectangle / FillRoundedRectangle 只吃统一半径，
// 要做 Adobe AI 那种"单角圆角"就得自己描路径。
// 注意别叫 RoundRect：wingdi.h 里有个同名的 GDI 函数 ::RoundRect。
namespace RoundBox
{
	// radii 顺序：TL, TR, BR, BL（物理像素）；内部按 min(w,h)/2 夹紧
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> build(
		ID2D1Factory* factory, const D2D1_RECT_F& r, const float radii[4]);

	// 夹紧：相邻两角在同一条边上的半径之和不能超过边长
	//（rTL+rTR ≤ 宽、rTL+rBL ≤ 高 …）。四角都相等时上限就是 min(w,h)/2，
	// 但只圆一个角时上限可以到整条短边 —— 也就是"整个角最大化"。
	void clamp(const D2D1_RECT_F& r, const float in[4], float out[4]);
	// 四角统一的半径上限（整体拖动 / 属性栏滑条用）
	float maxRadius(const D2D1_RECT_F& r);
	// 固定其余三角时，第 slot 个角（0=TL 1=TR 2=BR 3=BL）能取到的最大半径
	float maxRadiusFor(const D2D1_RECT_F& r, const float radii[4], int slot);
	// 四角都≈0 → 就是直角矩形（调用方可走快路径）
	bool isSquare(const float radii[4]);
}
