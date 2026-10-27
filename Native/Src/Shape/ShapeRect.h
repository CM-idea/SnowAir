#pragma once
#include <include/Ling.h>
#include <functional>
#include "ShapeBase.h"
class ShapeRect : public ShapeBase
{
public:
	// 形态：矩形 / 多边形 / 星形（椭圆走 ShapeEllipse）。多边形/星形共用这套框选/旋转交互
	enum Kind { Rect = 0, Polygon = 1, Star = 2 };
	ShapeRect(AnnotHost* win, int kind = Rect, int sides = 0);
	~ShapeRect();
	int kind() const { return kind_; }
	int sides() const { return sides_; }
	// 多边形边数 / 星形角数（3–20）：属性栏滑条实时改选中的形状
	void setSides(int n);
	// 圆角（物理像素）：属性栏滑条实时改选中的形状（矩形四角统一 / 多边形·星形顶点倒圆）
	void setCornerRadius(float pct);
	void applyStyle() override;
	void syncToolFromShape() const override;
	UINT32 styleColorValue() const override;
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	bool hitErase(const float x, const float y) override;
	// —— 连线：矩形可以当连线目标（见 ShapeLink）——
	bool isLinkTarget() const override { return true; }
	bool hitLinkBody(const float x, const float y) const override;
	D2D1_POINT_2F linkCenter() const override;
	float linkRotation() const override { return angle; }
	bool linkEdgePoint(const float localAngle, D2D1_POINT_2F& out) const override;
	float stylePrimarySize() const override { return strokeWidth; }
	std::vector<float> editSnapshot() const override;
	void applyEditSnapshot(const std::vector<float>& state) override;
	bool translateBy(float dx, float dy) override;
private:
	void updateDraggers();
	void toLocal(float x, float y, float& lx, float& ly) const;
	D2D1_POINT_2F center() const;
	void withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw);
	// 圆角：四角独立（可拖动的圆角控点）
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildShape() const;
	// 多边形/星形：在内接椭圆上均匀取点（星形外顶点 + 内顶点交替）
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildPolyPath() const;
	// 命中"实际画出来的形状"（多边形/星形）：描边附近一定算，includeInterior 时内部也算
	bool hitShapeLocal(float lx, float ly, bool includeInterior) const;
	bool hasCornerDots() const { return kind_ == Rect; }
	// 圆角百分比（0–100，100 = 该形状的最大圆角）→ 矩形的像素半径
	float pctRadiusPx() const;
	// 当前生效的四角半径（统一百分比或四角独立），命中与绘制共用
	void effectiveRadii(float out[4]) const;
public:
	// 多边形/星形路径（属性栏预览也用它，保证预览和真正创建出来的形状一致）
	static Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildPolyGeometry(
		const D2D1_RECT_F& r, int kind, int sides, float radiusPct);
	// 虚线样式（预览/形状共用）
	static Microsoft::WRL::ComPtr<ID2D1StrokeStyle> makeDashStroke(float strokeWidth);
private:
	std::vector<D2D1_RECT_F> draggers;
	D2D1_RECT_F rect{ 0,0,0,0 };
	int kind_{ Rect };
	int sides_{ 3 };   // 多边形边数 / 星形角数（3–20）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	float angle{ 0.f }; // 弧度，顶部为 0
	// 角外一圈旋转：光标按角挑字形，拖动按增量转
	int rotateCorner{ 0 };
	float rotateStartAngle{ 0.f }, rotateStartAtan{ 0.f };
	// 四角圆角半径 TL/TR/BR/BL（物理像素）——圆角点拖过之后用这套；
	// 没拖过时按 radiusPct_（0–100）统一换算
	float radius[4]{ 0.f, 0.f, 0.f, 0.f };
	float radiusPct_{ 0.f };      // 0–100，100 = 最大圆角
	bool uniformPct_{ true };     // true：按百分比统一；false：用上面的像素值
	// 圆角点：dotCorner = 已被"单击选中"的角（0/2/4/6），-1 = 无
	int dotCorner{ -1 };
	int dotPressIndex{ -1 }; // 本次按下的圆角点（10..13），mouseUp 判定"单击 or 拖动"
	bool dotDragAll{ true };
	float dotStart[4]{ 0.f, 0.f, 0.f, 0.f };
	float dotPressX{ 0.f }, dotPressY{ 0.f };
	bool isFill{ false };
	bool isDash{ false };
};
