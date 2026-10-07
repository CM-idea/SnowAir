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
	float stylePrimarySize() const override { return r; }
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
	void syncRectFromCenter();
	void syncCenterFromRect();
	D2D1_POINT_2F boxCenter() const;
	void toLocal(float x, float y, float& lx, float& ly) const;
	void withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw);
private:
	std::vector<D2D1_RECT_F> draggers;
	D2D1_RECT_F rotateHandle{};
	D2D1_RECT_F rect{ 0,0,0,0 }; // Emoji：与矩形相同的包围盒
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutText;
	float pressX{ 0.f }, pressY{ 0.f }, cx{ 0.f }, cy{ 0.f }, r{ 0.f }, angle{ 270.f };
	float boxAngle{ 0.f }; // Emoji 旋转（弧度，顶部为 0，对齐 ShapeArea）
	D2D1_POINT_2F tip{ 0,0 }, mid{ 0,0 };
	bool isFill{ false }, isWheel{ false };
	int val{ 1 };
	int styleKind{ 0 }; // 0 数字 / 1 字母 / 2 Emoji
	std::wstring emoji; // Emoji 模式落笔时固定的表情
};
