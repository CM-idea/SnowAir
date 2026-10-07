#pragma once
#include <include/Ling.h>
#include <functional>
#include "ShapeBase.h"

// 矩形系控点：选框 + 四角（马赛克/高亮/水印/包浆等共用；边中点不画，
// 也不再画单独旋转柄 —— 鼠标移到四角外侧一圈即提示旋转）
class ShapeArea : public ShapeBase
{
public:
	ShapeArea(AnnotHost* win);
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
	bool hitErase(const float x, const float y) override;
	float rotation() const { return angle; }
	// 带旋转的区几何（高亮并集挖洞/描边）
	Microsoft::WRL::ComPtr<ID2D1Geometry> makeAreaGeometry(bool asEllipse) const;
protected:
	virtual void onGeometrySettled() {}
	virtual void onGeometryChanged() {}
	// 是否支持"四角独立圆角点"（对齐 AI 的 live corner widget）：
	// 只有矩形系（高亮矩形）开，马赛克/包浆/水印不开
	virtual bool cornerDotsEnabled() const { return false; }
	void updateDraggers();
	int hitResizeZone(float lx, float ly) const;
	virtual float strokeHitPad() const;
	D2D1_POINT_2F center() const;
	void toLocal(float x, float y, float& lx, float& ly) const;
	void withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw);
	// 圆角：四角独立（TL/TR/BR/BL，物理像素）
	float radius[4]{ 0.f, 0.f, 0.f, 0.f };
	// 圆角点：dotCorner = 已被单击选中的角（0/2/4/6），-1 = 无
	int dotCorner{ -1 };
	int dotPressIndex{ -1 }; // 本次按下的圆角点（10..13），mouseUp 判定"单击 or 拖动"
	bool dotDragAll{ true };
	float dotStart[4]{ 0.f, 0.f, 0.f, 0.f };
	float dotPressX{ 0.f }, dotPressY{ 0.f };
	D2D1_RECT_F rect{ 0,0,0,0 };
	std::vector<D2D1_RECT_F> draggers;
	float pressX{ 0 }, pressY{ 0 };
	float angle{ 0.f }; // 弧度，顶部为 0
	// 旋转（角外一圈）：按下点方位 + 按下时的角度，拖动按增量算
	int rotateCorner{ 0 }; // 0=左上 2=右上 4=右下 6=左下（给光标挑字形）
	float rotateStartAngle{ 0.f };
	float rotateStartAtan{ 0.f };
};
