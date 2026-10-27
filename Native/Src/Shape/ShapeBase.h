#pragma once
#include <include/Ling.h>
#include <algorithm>
#include <functional>
#include <vector>
#include <cmath>
class AnnotHost;
class ShapeBase
{
public:
	ShapeBase(AnnotHost* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	// 二次选中后工具条样式变化：把当前工具状态重新套到本形状（默认无动作）。
	// 所有标注共用这一条路径，不再按类型各写一套分支。
	virtual void applyStyle() {}
	// 反向同步：把本形状的属性写回工具属性栏，供重新打开该标注的工具时展示。
	virtual void syncToolFromShape() const {}
	// 当前颜色（0xRRGGBBAA），用于把色板指到该标注的颜色
	virtual UINT32 styleColorValue() const { return 0; }
	static UINT32 packColor(const D2D1_COLOR_F& c)
	{
		auto ch = [](float v) {
			return static_cast<UINT32>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
		};
		return (ch(c.r) << 24) | (ch(c.g) << 16) | (ch(c.b) << 8) | ch(c.a);
	}
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	// 临时范围提示（控件层）：画在标注之上、只出现在屏幕上，导出不带（withChrome=false）
	virtual void paintHint(ID2D1DeviceContext* ctx) {};
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
	// 撤销/重做切换后回调（默认无动作）。白板背景尺寸这类"非绘制操作"靠它回滚。
	virtual void onUndoChanged(bool undone) {}
	// 几何快照：拖动、缩放、旋转要能单独作为一步撤销。
	// 返回空表示该形状不支持（视口滤镜等）。
	virtual std::vector<float> editSnapshot() const { return {}; }
	virtual void applyEditSnapshot(const std::vector<float>& state) { (void)state; }
	// 整体平移（方向键微调）：返回 false 表示这个形状不支持单独移动
	virtual bool translateBy(float dx, float dy) { (void)dx; (void)dy; return false; }
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
	// 白板「编辑背景」时画的标注属于背景层：关掉编辑背景后不参与点选/擦除，
	// 「重置画布」也不会清掉它（编辑背景打开时才一起清）
	bool backgroundLayer{ false };
	int hoverDraggerIndex{ -1 };
protected:
	// 矩形整体平移（各形状的 translateBy 共用）
	static D2D1_RECT_F offsetRect(const D2D1_RECT_F& r, float dx, float dy)
	{
		return D2D1::RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy);
	}
	// —— 矩形容器几何：矩形/椭圆/多边形/星形/画笔/序号共用同一套 ——
	static D2D1_POINT_2F boxCenter(const D2D1_RECT_F& r);
	// 世界坐标 → 以 rect 中心为轴反旋转 angleRad 后的局部坐标
	static void boxToLocal(const D2D1_RECT_F& r, float angleRad, float x, float y,
		float& lx, float& ly);
	// 在 rect 中心按 angleRad 旋转坐标系后执行 draw，结束后恢复原变换
	static void withBoxRotation(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, float angleRad,
		const std::function<void()>& draw);
	// 8 个缩放柄（四角 + 四边中点）；selection 已按 dpi 与外扩量调整，half = draggerSize / 2
	static void layoutBoxDraggers(std::vector<D2D1_RECT_F>& out, const D2D1_RECT_F& selection, float half);
	// 四角优先、四边次之（不必精确点到控点）；返回 0–7，未命中 -1
	int hitBoxEdgeOrCorner(float x, float y, const D2D1_RECT_F& r, float band) const;
	// —— 选中框：所有控件统一虚线，并相对本体四周外扩 2px ——
	static ID2D1StrokeStyle* selectionStroke();
	D2D1_RECT_F selectionRect(const D2D1_RECT_F& r) const;
	// 本体描边有宽度时，选中框还要让开这半个描边，才能和"看得见的轮廓"留出 2px
	virtual float selectionExtra() const { return 0.f; }
	// —— Shift 旋转吸附：8 个朝向 = 45° 的整数倍 ——
	static float snapRotation8(float angleRad)
	{
		constexpr float kStep = 0.78539816339744830962f; // π/4
		return std::round(angleRad / kStep) * kStep;
	}
	// —— 缩放（矩形/椭圆/蒙版共用）——
	// 按下缩放柄时记录"固定对边/对角"的屏幕锚点与起始 rect；
	// 拖动时按锚点重建 rect —— 固定边在屏幕上不动，旋转过的形状也不会漂。
	// handle 用 hoverDraggerIndex 的 0..7；传 <0 表示新建（锚点 = 按下点）
	void beginScale(const D2D1_RECT_F& r, float angleRad, int handle, float pressX, float pressY);
	// keepAspect = Shift：等比缩放，固定对边（角=对角，边=对边中点）
	D2D1_RECT_F dragScale(float x, float y, bool keepAspect) const;
	D2D1_RECT_F scaleStartRect{};
	D2D1_POINT_2F scaleAnchor{ 0.f, 0.f };
	float scaleStartAngle{ 0.f };
	int scaleHandle{ -1 };
	bool scaleCreating{ false };
	// —— 四角外侧旋转 ——
	// 角上（≤6px）仍是缩放柄；角外 6–26px 那一圈才是旋转区，
	// 于是不再需要单独画一个旋转柄。传入局部坐标（已按 angle 反旋转）与轴对齐 rect。
	// 返回角序号 0=左上 2=右上 4=右下 6=左下，未命中 -1
	int hitRotateRing(float lx, float ly, const D2D1_RECT_F& r) const;
	// 设置旋转光标（图标字体四角旋转字形，随标注角度一起转）
	void setRotateCursor(int corner, float angleRad);
	void setIconCursor(wchar_t code, float angleRad = 0.f);
	// —— 圆角点 ——
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
