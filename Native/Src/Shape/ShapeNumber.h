#pragma once
#include <include/Ling.h>
#include <functional>
#include "ShapeBase.h"
class ShapeNumber : public ShapeBase
{
public:
	ShapeNumber(AnnotHost* win);
	~ShapeNumber();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 单击就是它的正常用法：落一个序号徽章，不需要拖动
	bool isValidWithoutDrag() override { return true; };
	bool hitErase(const float x, const float y) override;
	int style() const { return styleKind; }
	// 当前显示文字（手动标签优先，否则按数字/字母/Emoji 格式化）
	std::wstring displayLabel() const { return formatLabel(); }
	void setDisplayLabel(const std::wstring& text);
	void applyStyle() override;
	void syncToolFromShape() const override;
	// 尾巴被拖到控制圈外：Delete/Backspace 先收回圈上
	bool retractTail() override;
	// 序号值 / 序号样式（属性栏「当前序号」推算下一个值时要看已有形状）
	int numberVal() const { return val; }
	float stylePrimarySize() const override { return r; }
	std::vector<float> editSnapshot() const override;
	void applyEditSnapshot(const std::vector<float>& state) override;
	bool translateBy(float dx, float dy) override;
	D2D1_RECT_F bounds() const override;
	bool rotateBy(float pivotX, float pivotY, float deltaRad) override;
	bool scaleBy(float pivotX, float pivotY, float angleRad, float sx, float sy) override;
public:
private:
	static int getNextVal(AnnotHost* win, int style);
	std::wstring formatLabel() const;
	bool isEmoji() const;
	D2D1_POINT_2F localPoint(const float degrees);
	D2D1_POINT_2F transformPoint(const D2D1_POINT_2F& point);
	void makePath();
	void makeTextLayout();
	void updateDraggers();
	// 尾巴拖出后：移动序号时保持尾巴尖的世界坐标不变，靠改角度/长度重新接回中心
	void updateTailAnchor();
	bool syncTailFromAnchor();
	void syncRectFromCenter();
	void syncCenterFromRect();
	D2D1_POINT_2F boxCenter() const;
	void toLocal(float x, float y, float& lx, float& ly) const;
	void withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw);
	// 序号选中态的大同心圆半径：尾巴尖永远在圈内，整圈都可拖来缩放
	float controlRadius() const { return r * 1.7f; }
private:
	std::vector<D2D1_RECT_F> draggers;
	D2D1_RECT_F rotateHandle{};
	D2D1_RECT_F rect{ 0,0,0,0 }; // Emoji：与矩形相同的包围盒
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutText;
	float pressX{ 0.f }, pressY{ 0.f }, cx{ 0.f }, cy{ 0.f }, r{ 0.f }, angle{ 270.f };
	// 尾巴长度（相对半径的倍率）：默认 4/3；拖出控制圈后可大于 1.7，尾巴尖跟着控制点走
	float tailFactor_{ 4.f / 3.f };
	// 尾巴被拖出圈后，尾巴尖在这里"钉住"；移动序号时角度/长度跟着重算，尖不动
	bool tailAnchored_{ false };
	D2D1_POINT_2F tailAnchor_{ 0.f, 0.f };
	bool tailExtended() const { return tailFactor_ > 1.7f + 1e-3f; }
	float tailDotRadius() const { return r * (tailExtended() ? tailFactor_ : 1.7f); }
	float boxAngle{ 0.f }; // Emoji 旋转（弧度，顶部为 0，对齐 ShapeArea）
	D2D1_POINT_2F tip{ 0,0 }, mid{ 0,0 };
	bool isFill{ false }, isWheel{ false };
	int val{ 1 };
	int styleKind{ 0 }; // 0 数字 / 1 字母 / 2 Emoji
	std::wstring emoji; // Emoji 模式落笔时固定的表情
	std::wstring labelText; // 属性栏手动输入的序号文字（优先于 val / emoji，可含中文）
};
