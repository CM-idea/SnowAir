#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"

class ShapeText : public ShapeBase
{
public:
	ShapeText(AnnotHost* win);
	~ShapeText();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
	bool isValidWithoutDrag() override { return true; };
	bool hitErase(const float x, const float y) override;
	void finishEdit();
	void applyStyle();
	void startEdit();
	// 按当前 rect 把输入框摆好（附着文字时 rect 是以线段锚点为中心的）。
	// 编辑已经开着的时候调它，就能把插入符从"落笔的鼠标位置"挪回锚点
	void syncEditorGeometry();
	// 挂到箭头/线段上：把文字**中心**摆在线段中点（不跟线转 —— 线上文字一律横排，
	// 垂直的箭头要靠用户自己在编辑框里回车换行来排成竖列，见 .cpp 里的说明）
	void setAttachPose(float cx, float cy);
	// 命中这块文字（= 线上那个"洞"）：按文字自己的朝向把点转回未旋转的坐标系再判。
	// 线上文字角度恒为 0，这里是给用户手动转过角度的普通文字用的。
	// 坐标同 rect，都是标注坐标系
	bool hitLabelBox(float x, float y) const;
	float stylePrimarySize() const override { return fontSize; }
public:
	bool isEditing{ false };
	// 「线段中间打字」：画字之前先用底图把这块抠回来，线段/箭头从文字下穿过就会断成两截
	bool punchBg{ false };
	// 这条文字挂在哪条箭头/线段上（不持有所有权；每次绘制前会验证它还在列表里）
	class ShapeBase* attachTo{ nullptr };
	float attachCx{ 0.f }, attachCy{ 0.f };   // 挂点（线段中点），编辑中也要按它居中
private:
	void makeTextLayout();
	void paintPunch(ID2D1DeviceContext* ctx);
	void setAttr();
	void syncRectFromLayout();
	void updateDraggers();
private:
	std::wstring text;
	float fontSize{ 20.f };
	float angle{ 0.f }; // 弧度
	bool isBold{ false }, isItalic{ false };
	UINT32 colorValue{ 0 };
	D2D1_COLOR_F color{};
	D2D1_RECT_F rect{};
	std::vector<D2D1_RECT_F> draggers; // 0..3 角缩放；旋转改走「角外一圈」
	Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashedStrokeStyle;
	float borderPadding;
	// 线上文字用的行距（标注单位，"一个字"≈字体 baseline）。编辑框要设同一个值，
	// 打字时和落下后才是同一个盒
	float attachLineStep{ 0.f };
	// 线上文字行距压到"一个字"后，自然行高比行距多出来的那点（标注单位）。
	// 排版高度不含它，所以洞/编辑框的高度要补上，最后一行的下伸才不会被切掉。
	// 见 makeTextLayout / syncRectFromLayout / syncEditorGeometry
	float attachLineSlack{ 0.f };
	float pressX{ 0.f }, pressY{ 0.f };
	float resizeStartFont{ 20.f };
	D2D1_RECT_F resizeStartRect{};
	int rotateCorner{ 0 }; // 光标按角挑字形（0=左上 2=右上 4=右下 6=左下）
	float rotateStartAngle{ 0.f };
	float rotateStartAtan{ 0.f };
	winrt::event_token textChangedTok{}, focusTok{};
};
