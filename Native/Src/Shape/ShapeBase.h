#pragma once
#include <include/Ling.h>
class AnnotHost;
class ShapeBase
{
public:
	ShapeBase(AnnotHost* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	virtual void mouseMove(const float x, const float y) { };
	virtual void mouseDrag(const float x, const float y) {};
	virtual void mouseDown(const float x, const float y) { };
	virtual void mouseUp(const float x, const float y) { };
	virtual void mouseWheel(const float x, const float y, const short delta) {};
	virtual void setCursor() {};
	virtual bool isValidWithoutDrag() { return false; };
	// 水印/包浆：视口滤镜，不可点选，选工具即生效
	virtual bool isViewportFilter() { return false; };
	// 渐隐画笔等：不可点选，淡出后自动移除
	virtual bool isEphemeral() { return false; };
	virtual bool hitErase(const float x, const float y) { return false; };
	// —— 连线：能被吸附的一侧（矩形/椭圆） ——
	// 能不能当连线目标
	virtual bool isLinkTarget() const { return false; }
	// 点是否落在形状里（画线时端点落进来就吸附）
	virtual bool hitLinkBody(const float x, const float y) const { return false; }
	// 形状中心（连线端点朝哪个方向）
	virtual D2D1_POINT_2F linkCenter() const { return D2D1_POINT_2F{ 0.f, 0.f }; }
	// 形状自身旋转（世界方向角 → 局部方向角要减掉它）
	virtual float linkRotation() const { return 0.f; }
	// 按局部方向角求边缘交点：连线端点就落在这上面
	virtual bool linkEdgePoint(const float localAngle, D2D1_POINT_2F& out) const { return false; }
	// —— 连线：线的一侧（箭头/线段） ——
	// 有没有连线端（决定线中点那个"删掉这条线"的叉显不显示）
	virtual bool hasLink() const { return false; }
	// 连线端的**起点**（起点这一端吸附着形状时给出）：拖动连线时提示点一直跟着它 ——
	// 拖进目标形状时点不能跟着终点跑过去（用户要的：目标上不需要点提示）
	virtual bool linkStartPoint(D2D1_POINT_2F& out) const { return false; }
	// 叉（线中点）的位置 / 命中
	virtual bool linkButtonPos(D2D1_POINT_2F& out) const { return false; }
	virtual bool hitLinkButton(const float x, const float y, const float dpi) const { return false; }
	// 把绑定的端点重新贴回形状边缘（形状移动/缩放/旋转、或形状没了）
	virtual void syncLinks() {}
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y);
	void paintHandle(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect);
	bool isSelected() const;
	// 未选中时悬停：点选手型；已选中时由子类设缩放/移动光标
	bool applyUnselectedHoverCursor();
	// 属性栏主滑条对应尺寸（线宽 / 字号 / 序号半径）；无则 -1
	virtual float stylePrimarySize() const { return -1.f; }
public:
	AnnotHost* win;
	bool isUndo;
	int hoverDraggerIndex{ -1 };
protected:
	// 四角优先、四边次之（不必精确点到控点）；返回 0–7，未命中 -1
	int hitBoxEdgeOrCorner(float x, float y, const D2D1_RECT_F& r, float band) const;
	// —— 四角外侧旋转 ——
	// 角上（≤6px）仍是缩放柄；角外 6–26px 那一圈才是旋转区，
	// 于是不再需要单独画一个旋转柄。传入局部坐标（已按 angle 反旋转）与轴对齐 rect。
	// 返回角序号 0=左上 2=右上 4=右下 6=左下，未命中 -1
	int hitRotateRing(float lx, float ly, const D2D1_RECT_F& r) const;
	// 设置旋转光标（图标字体四角旋转字形，随标注角度一起转）
	void setRotateCursor(int corner, float angleRad);
	void setIconCursor(wchar_t code, float angleRad = 0.f);
	// —— 圆角点（对齐 Adobe AI 的 live corner widget）——
	// 四角内侧各一个圆点：未选中该点时向内拖 = 四角一起调；单击选中后再拖 = 只调这一个角。
	// 返回 10..13（左上/右上/右下/左下），未命中 -1
	int hitCornerDot(float lx, float ly, const D2D1_RECT_F& r) const;
	static D2D1_POINT_2F cornerDotPos(int dotIndex, const D2D1_RECT_F& r, float inset);
	void paintCornerDot(ID2D1DeviceContext* ctx, D2D1_POINT_2F p, bool active);
	// dotIndex(10..13) → 半径数组下标 0..3（TL/TR/BR/BL）
	static int dotSlot(int dotIndex) { return dotIndex - 10; }
	static int dotIndexFromCorner(int corner);
	// 半径下标 0..3 → 角序号 0/2/4/6
	static int cornerFromDotSlot(int slot) { return slot == 0 ? 0 : slot == 1 ? 2 : slot == 2 ? 4 : 6; }
	float draggerSize;
	float hitPad;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHandleFill;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHandleBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHandleShadow;
};
