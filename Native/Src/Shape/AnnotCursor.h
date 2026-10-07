#pragma once
#include <Windows.h>

// 四角旋转光标：
// 字形取自图标字体 font_5228143 的「左上/右上/左下/右下旋转」，渲染成 32×32
// 位图后转 HICON，按「角 + 标注角度」缓存。鼠标移到标注四角外侧一圈时用它提示可旋转。
namespace AnnotCursor
{
	// corner：与 ShapeBase::hitRotateRing 的角序号一致 —— 0=左上 2=右上 4=右下 6=左下
	// angleRad：标注当前旋转角（光标跟着标注一起转）
	// 失败返回 nullptr（调用方自行退回箭头）
	HCURSOR rotate(int corner, float angleRad);
	// 任意图标字形 → 光标（圆角点用 Icon::RoundCorner）
	HCURSOR glyph(wchar_t code, float angleRad = 0.f);
}
