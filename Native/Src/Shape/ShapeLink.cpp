#include "pch.h"
#include "Win/AnnotHost.h"
#include "History.h"
#include "ArrowPoly.h"
#include "ShapeBase.h"
#include "ShapeLink.h"
#include <cmath>

namespace ShapeLink
{
	namespace
	{
		// 目标形状还在列表里吗。注意 **撤销不算死**：形状对象还在，重新做（redo）就该再把线连回去，
		// 所以这里只看"对象还在不在"，isUndo 由 place 那边决定要不要贴（撤销中不贴，端点原地不动）
		ShapeBase* findAlive(AnnotHost* win, ShapeBase* s)
		{
			if (!s || !win || !win->history) return nullptr;
			for (auto& o : win->history->shapes) {
				if (o.get() == s) return s;
			}
			return nullptr;
		}

		// 绑定到 (x,y) 这个落点：锚点 = 落点相对形状中心的方向，换算到形状局部坐标系。
		// 默认不 pinned —— sync 里会让它跟着另一端走（连线朝对面贴边）
		void bindTo(ArrowLink& link, ShapeBase* target, float x, float y)
		{
			link.shape = target;
			link.pinned = false;
			if (!target) return;
			const auto c = target->linkCenter();
			link.localAngle = std::atan2(y - c.y, x - c.x) - target->linkRotation();
		}

		// 把这一端的点摆到形状边缘的锚点方向上（没绑定就什么都不做）
		void place(const ArrowLink& link, bool start, ArrowPolyState& poly)
		{
			if (!link.shape || poly.pts.size() < 2) return;
			D2D1_POINT_2F p{};
			if (!link.shape->linkEdgePoint(link.localAngle, p)) return;
			(start ? poly.pts.front() : poly.pts.back()) = p;
		}

		float polyLengthMid(const ArrowPolyState& poly, D2D1_POINT_2F& out)
		{
			if (poly.pts.size() < 2) return 0.f;
			float total = 0.f;
			for (size_t i = 1; i < poly.pts.size(); i++)
				total += std::hypot(poly.pts[i].x - poly.pts[i - 1].x, poly.pts[i].y - poly.pts[i - 1].y);
			if (total < 1.f) {
				out = poly.pts.front();
				return 0.f;
			}
			const float half = total * 0.5f;
			float walk = 0.f;
			for (size_t i = 1; i < poly.pts.size(); i++) {
				const float seg = std::hypot(poly.pts[i].x - poly.pts[i - 1].x, poly.pts[i].y - poly.pts[i - 1].y);
				if (walk + seg >= half) {
					const float t = seg > 0.f ? (half - walk) / seg : 0.f;
					out = D2D1::Point2F(
						poly.pts[i - 1].x + (poly.pts[i].x - poly.pts[i - 1].x) * t,
						poly.pts[i - 1].y + (poly.pts[i].y - poly.pts[i - 1].y) * t);
					return total;
				}
				walk += seg;
			}
			out = poly.pts.back();
			return total;
		}
	}

	ShapeBase* pickTarget(AnnotHost* win, float x, float y)
	{
		if (!win || !win->history) return nullptr;
		auto& shapes = win->history->shapes;
		for (int i = (int)shapes.size() - 1; i >= 0; i--) {
			auto* s = shapes[i].get();
			if (!s || s->isUndo || s->isEphemeral() || !s->isLinkTarget()) continue;
			if (s->hitLinkBody(x, y)) return s;
		}
		return nullptr;
	}

	void bindAtEndpoint(AnnotHost* win, ArrowPolyState& poly, bool start, float x, float y)
	{
		ArrowLink& link = start ? poly.linkStart : poly.linkEnd;
		bindTo(link, pickTarget(win, x, y), x, y);
		place(link, start, poly);
	}

	void dragEndpoint(AnnotHost* win, ArrowPolyState& poly, bool creating, float x, float y)
	{
		const bool draggingStart = poly.drag == ArrowPolyState::DragKind::Start;
		const bool draggingEnd = poly.drag == ArrowPolyState::DragKind::End;
		if (!draggingStart && !draggingEnd) return;
		ArrowLink& link = draggingStart ? poly.linkStart : poly.linkEnd;

		ShapeBase* under = pickTarget(win, x, y);
		if (under && under != link.shape) {
			// 落进（另一个）形状：吸附过去
			bindTo(link, under, x, y);
		}
		else if (!under && creating) {
			// 正在画新线、且离开了形状：脱开，端点跟手（所见即所得）
			link.shape = nullptr;
		}
		else if (link.shape && !creating) {
			// 改已有线：拖端点 = 沿边缘滑到鼠标方向并钉住（拖不断）。
			// 只挪了几像素不算"用户想挑位置" —— 不钉住，继续自动朝对面贴边
			const float moved = std::hypot(x - poly.press.x, y - poly.press.y);
			if (moved > 4.f * win->dpi) {
				const auto c = link.shape->linkCenter();
				link.localAngle = std::atan2(y - c.y, x - c.x) - link.shape->linkRotation();
				link.pinned = true;
			}
		}
		// 画的过程中**不能**钉锚点：否则端点会停在鼠标最后停留的那一侧
		// （用户截图里"箭头穿进矩形、头贴在远的那条边"就是这么来的）
		sync(win, poly);
	}

	void sync(AnnotHost* win, ArrowPolyState& poly)
	{
		for (int k = 0; k < 2; k++) {
			const bool start = (k == 0);
			ArrowLink& link = start ? poly.linkStart : poly.linkEnd;
			if (!link.shape) continue;
			if (!findAlive(win, link.shape)) {
				link.shape = nullptr;   // 形状真的没了（删掉/被清空）：脱钩，端点停在原地
				continue;
			}
			// 撤销中的形状不参与摆放：端点停在原地，redo 回来再贴上去
			if (link.shape->isUndo) continue;
			// 没被用户钉住时，锚点跟着**另一端**走：连线永远朝对面贴边。
			// 这样把矩形拖到另一边时，线不会还挂在过期的那一侧
			if (!link.pinned) {
				const D2D1_POINT_2F other = start ? poly.pts.back() : poly.pts.front();
				const auto c = link.shape->linkCenter();
				if (std::hypot(other.x - c.x, other.y - c.y) > 1.f) {
					link.localAngle = std::atan2(other.y - c.y, other.x - c.x) - link.shape->linkRotation();
				}
			}
			place(link, start, poly);
		}
	}

	bool hasLink(const ArrowPolyState& poly)
	{
		return poly.linkStart.shape != nullptr || poly.linkEnd.shape != nullptr;
	}

	bool linkButtonPos(const ArrowPolyState& poly, D2D1_POINT_2F& out)
	{
		if (!hasLink(poly)) return false;
		D2D1_POINT_2F mid{};
		if (polyLengthMid(poly, mid) <= 0.f) return false;
		out = mid;
		return true;
	}

	bool hitLinkButton(const ArrowPolyState& poly, float x, float y, float dpi)
	{
		D2D1_POINT_2F mid{};
		if (!linkButtonPos(poly, mid)) return false;
		return std::hypot(x - mid.x, y - mid.y) <= 11.f * dpi;
	}

	// 悬停提示：形状边缘"朝 (x,y) 方向"的点（和落笔时 bindTo 用的方向一致）
	bool edgePointToward(ShapeBase* target, float x, float y, D2D1_POINT_2F& out)
	{
		if (!target) return false;
		const auto c = target->linkCenter();
		const float local = std::atan2(y - c.y, x - c.x) - target->linkRotation();
		return target->linkEdgePoint(local, out);
	}
}
