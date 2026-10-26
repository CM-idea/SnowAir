#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
#include "ArrowPoly.h"

// 线段：与箭头共用折线控点。highlighter=true 时作为荧光笔：
// 同样的两点交互，但更宽、半透明、平头（透明度跟画笔的荧光笔设置）
class ShapeLine : public ShapeBase
{
public:
	// 形态：直线 / 波浪线 / 角度（角度 = 三条点折线：腿-顶点-腿）
	// 尺寸不在这里：它就是「标记箭头 + 中间自带文字」，外观与标记箭头完全一致
	enum Kind { Straight = 0, Wave = 1, Angle = 2 };
	explicit ShapeLine(AnnotHost* win, bool highlighter = false, int kind = Straight);
	bool isAngle() const { return kind_ == Angle; }
	bool angleComplete() const { return kind_ == Angle && poly.pts.size() >= 3; }
	int anglePointCount() const { return (int)poly.pts.size(); }
	// 角度是三点式：第一下点/拖出基线，第二下点/拖出第三点后完成
	bool isValidWithoutDrag() override { return kind_ == Angle; }
	// 自动测量的角度文字（"45°"，取两条边的**较小夹角** 0~180°），供挂在线上的文字同步
	float measuredAngleDeg() const;
	std::wstring angleLabelText() const;
	void updateAngleLabel();
	// 当前测量区间：起始边角度 + 扫角（视觉逆时针口径，0~2π）
	float angleSweep(float& start) const;
	// 角度数字的字号（逻辑像素）：比普通线上文字大一点，读数更清楚
	static constexpr float AngleLabelFontSize{ 18.f };
	float angleLabelFontPx() const { return AngleLabelFontSize * win->dpi; }
	// 荧光笔宽度 = 画笔滑条值 × 该倍数（滑条显示 4 时实际 20）
	static constexpr float HighlighterWidthScale{ 5.f };
	bool isHighlighter() const { return highlighter; }
	~ShapeLine();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	bool hitErase(const float x, const float y) override;
	bool toggleFreeEndpoint(float x, float y);
	void resetFree();
	// —— 连线（线段这一侧，逻辑都在 ShapeLink）——
	bool hasLink() const override;
	bool linkStartPoint(D2D1_POINT_2F& out) const override;
	bool linkButtonPos(D2D1_POINT_2F& out) const override;
	bool hitLinkButton(const float x, const float y, const float dpi) const override;
	void syncLinks() override;
	// 「线段中间打字」：线段没有大箭头形态；折线长度中点（图像坐标）
	bool bigHead() const { return false; }
	bool shaftMidPoint(float& mx, float& my) const;
	float shaftAngle() const;   // 起点→终点方向（弧度）。线上文字一律横排，现在没人用它
	float stylePrimarySize() const override { return strokeWidth; }
	std::vector<float> editSnapshot() const override;
	void applyEditSnapshot(const std::vector<float>& state) override;
	bool translateBy(float dx, float dy) override;
private:
	void ensureStrokeStyle();
	void ensureMinLength();
	void constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY);
	bool hitBody(float x, float y);
	int hoverFromHit(const ArrowHitInfo& h) const;
	// 波浪线：沿首尾两点生成一条两端收幅的正弦波
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildWavePath();
	// 角度：顶点处画一小段弧提示张角
	void paintAngleArc(ID2D1DeviceContext* ctx);
private:
	ArrowPolyState poly;
	ArrowHitInfo lastHit{};
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f };
	bool isDash{ false };
	bool isRound{ false };
	bool highlighter{ false };   // 荧光笔：更宽半透明平头，最小长度按笔宽算
	int kind_{ Straight };       // 直线 / 波浪线 / 角度
	bool angleFirstSet_{ false };      // 角度：第一点已落
	bool angleBaselineDone_{ false };  // 角度：基线（两点）已成形，等第三点
	// 测量方向：第三点落下时定下"从较小夹角那一侧开始"，之后沿这个方向连续累计，
	// 允许 0~360°，不会在 180° 处翻到另一侧重来
	bool angleCcw_{ true };
	bool angleDirSet_{ false };
	bool creating{ false };   // 本次按下是不是"新画一条"（决定连线端点离开形状时脱不脱开）
};
