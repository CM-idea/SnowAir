#pragma once
#include <include/Ling.h>

class AnnotHost;
class ShapeBase;
struct ArrowPolyState;

// 连线（箭头/线段端点 ↔ 矩形/椭圆）。规则是用户定的：
//   · 画的时候端点落进矩形/椭圆里就自动吸附：端点在形状**边缘**上、朝另一端的方向
//     （永远贴"最近的那条边"，不会穿进形状里挂到远的那侧）；
//   · 已经连上的端点**拖不断**：拖它只是让吸附点沿边缘滑动（拖进别的形状则改连那个）；
//   · 形状移动/缩放/旋转、甚至被删掉：每次绘制/命中前重新核对（形状没了就自动脱钩，
//     端点停在最后的位置）。所以不需要在 History 里做反向登记；
//   · 线中点悬停出现的那个叉 = **直接删掉这条箭头/线段**（不是只断开）。
namespace ShapeLink
{
	// 落点下面能连的形状：只在矩形/椭圆里找，取最上面那个（后画的盖在上面）
	ShapeBase* pickTarget(AnnotHost* win, float x, float y);

	// 落笔画线时：落点落在形状里就绑上（并把端点先摆到边缘）
	void bindAtEndpoint(AnnotHost* win, ArrowPolyState& poly, bool start, float x, float y);

	// 拖端点时调用。creating = 正在画新线：落进形状就吸附、离开就脱开（所见即所得）；
	// creating = false（改已有线）：按"拖不断"处理，只在边缘上滑动
	void dragEndpoint(AnnotHost* win, ArrowPolyState& poly, bool creating, float x, float y);

	// 把绑定的端点重新贴回形状边缘；形状没了就脱钩
	void sync(AnnotHost* win, ArrowPolyState& poly);

	bool hasLink(const ArrowPolyState& poly);
	// 线中点（折线按长度取中点）：那个"删掉这条线"的叉就画在这儿
	bool linkButtonPos(const ArrowPolyState& poly, D2D1_POINT_2F& out);
	bool hitLinkButton(const ArrowPolyState& poly, float x, float y, float dpi);
	// 悬停提示用：形状边缘"朝 (x,y) 方向"的那个点（和落笔时绑定的方向一致）
	bool edgePointToward(ShapeBase* target, float x, float y, D2D1_POINT_2F& out);
}
