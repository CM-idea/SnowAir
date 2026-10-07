#pragma once
#include <include/Ling.h>
#include <wrl/client.h>
#include <vector>

// 线/箭头共用折线几何与命中
struct ArrowHitInfo {
	enum Kind { None, Start, End, Move, Break } kind{ None };
	int pointIndex{ -1 };
	int insertAfter{ -1 };
};

class ShapeBase;

// 端点绑到某个形状上（矩形/椭圆）当"连线"。
// 不持有所有权：每次绘制/命中前会和 history 里的列表核对，形状被删/撤销就自动脱钩。
// localAngle 是"形状局部坐标系里的方向角"——端点因此永远落在形状边缘、朝这个方向。
// 默认（pinned=false）这个角**跟着另一端走**：连线永远朝对面贴边，形状挪到哪边都不会
// 挂在过期的那一侧；用户手动拖过端点才 pinned=true（保持他挑的方向）。
// 拖端点不会脱钩（用户要的"拖不断"），断开得点线中点上的叉
struct ArrowLink {
	ShapeBase* shape{ nullptr };
	float localAngle{ 0.f };
	bool pinned{ false };
};

struct ArrowPolyState {
	std::vector<D2D1_POINT_2F> pts;
	bool freeStart{ false };
	bool freeEnd{ false };
	int breakHoverKey{ -1 };
	// 连线：两端各自可以绑一个形状（见 ShapeLink）
	ArrowLink linkStart, linkEnd;

	enum class DragKind { None, Start, End, Break, Move };
	DragKind drag{ DragKind::None };
	int dragPointIndex{ -1 };
	bool dragFree{ false };
	D2D1_POINT_2F press{};
	std::vector<D2D1_POINT_2F> dragOrigin;

	void setTwo(float x0, float y0, float x1, float y1);
	void syncFromEnds(float sx, float sy, float ex, float ey);
	D2D1_POINT_2F front() const;
	D2D1_POINT_2F back() const;
	void translate(float dx, float dy);

	ArrowHitInfo hit(float x, float y, float dpi, bool dragging, bool allowVirtualBreak = true) const;
	void beginDrag(const ArrowHitInfo& h, float x, float y);
	void dragTo(float x, float y);
	void endDrag();
	bool toggleFreeAt(float x, float y, float dpi);

	static float pointToSegDist(float px, float py, D2D1_POINT_2F a, D2D1_POINT_2F b);
	static std::vector<D2D1_POINT_2F> transformAroundPivot(
		const std::vector<D2D1_POINT_2F>& src, int pivotIdx,
		D2D1_POINT_2F from, D2D1_POINT_2F to);
	// 大箭头实心多边形（支持折线）
	static std::vector<D2D1_POINT_2F> buildBigArrowPolygon(
		const std::vector<D2D1_POINT_2F>& pts, float strokeWidth);
};

void paintArrowPolyHandles(
	ID2D1DeviceContext* ctx,
	const ArrowPolyState& poly,
	float dpi,
	ID2D1SolidColorBrush* fill,
	ID2D1SolidColorBrush* border,
	ID2D1SolidColorBrush* shadow,
	bool dragging,
	bool showVirtualBreaks = true);

// 箭头/线段的描边部件：轮廓生成时把「轴线」与「箭头头部」分开，
// 于是两条规则可以各自落地：
//   · 圆头作用于整体 —— 轴线与头部共用同一套 cap/join；
//   · 虚线只作用于轴线 —— 头部永远实线。
namespace ArrowParts
{
	// 轴线（杆身）折线路径：虚线只挂这条
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> shaftPath(
		ID2D1Factory* factory, const std::vector<D2D1_POINT_2F>& pts);
	// 开口箭头 V（左翼 → 尖端 → 右翼）：圆头时尖端走圆角，平头时是尖角
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> openHeadPath(
		ID2D1Factory* factory, D2D1_POINT_2F from, D2D1_POINT_2F to, float strokeWidth);
	// 标注箭头：两端各一条垂直于端段的短竖线
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> annotBarsPath(
		ID2D1Factory* factory, const std::vector<D2D1_POINT_2F>& pts, float strokeWidth);
	float annotBarHalf(float strokeWidth);
	// 轴线样式：cap/join 跟「圆头」，dashed 只在这里生效
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> makeShaftStyle(
		ID2D1Factory* factory, float strokeWidth, bool roundCap, bool dashed);
	// 头部样式：cap/join 同样跟「圆头」，永远实线
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> makeHeadStyle(
		ID2D1Factory* factory, bool roundCap);
}
