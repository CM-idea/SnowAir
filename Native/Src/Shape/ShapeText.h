#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"

class ShapeText : public ShapeBase
{
public:
	// bubble=true：标注里的「对话气泡」——圆角矩形+尾巴；burst=true：爆裂（同一套文字交互，换外框）
	explicit ShapeText(AnnotHost* win, bool bubble = false, bool burst = false);
	bool isBubble() const { return bubble_; }
	bool isBurst() const { return burst_; }
	bool isFrame() const { return bubble_ || burst_; }
	~ShapeText();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	bool isValidWithoutDrag() override { return !isFrame(); };   // 气泡/爆裂和矩形一样：必须拖出框
	bool hitErase(const float x, const float y) override;
	void finishEdit(bool suppressNextClick = true);
	void applyStyle();
	void startEdit();
	// 撤销/重做恢复文字内容，并同步重排和气泡/文字框尺寸
	void applyTextFromUndo(const std::wstring& t);
	// 标注文字编辑：直接维护 text / caretIndex
	void insertText(const std::wstring& s);
	void backspace();
	void deleteForward();
	void moveCaret(int delta);
	void moveCaretTo(bool end);
	// 「尺寸」：挂到线段上时初始化一段默认文字（固定字号，随后 setAttachPose 摆到中点）
	void initAttachedText(const std::wstring& t, float px);
	// 自动文字（角度测量）：用户手动编辑过之后就不再自动改写
	void setAutoText(const std::wstring& t);
	// 挂到箭头/线段上：把文字**中心**摆在线段中点（不跟线转 —— 线上文字一律横排，
	// 垂直的箭头要靠用户自己在编辑框里回车换行来排成竖列，见 .cpp 里的说明）
	void setAttachPose(float cx, float cy);
	// 命中这块文字（= 线上那个"洞"）：按文字自己的朝向把点转回未旋转的坐标系再判。
	// 线上文字角度恒为 0，这里是给用户手动转过角度的普通文字用的。
	// 坐标同 rect，都是标注坐标系
	bool hitLabelBox(float x, float y) const;
	// 编辑时唯一"不退出"的点击区域：插入符那一小条（点其它任何地方都提交退出）
	bool hitCaretArea(float x, float y) const;
	float stylePrimarySize() const override { return fontSize; }
	std::vector<float> editSnapshot() const override;
	void applyEditSnapshot(const std::vector<float>& state) override;
	bool translateBy(float dx, float dy) override;
public:
	bool isEditing{ false };
	int caretIndex_{ 0 };
	// 一次文字编辑会话的起点：结束时作为一步撤销
	std::wstring editStartText_;
	std::vector<float> editStartGeo_;
	// 「线段中间打字」：画字之前先用底图把这块抠回来，线段/箭头从文字下穿过就会断成两截
	bool punchBg{ false };
	// 这条文字挂在哪条箭头/线段上（不持有所有权；每次绘制前会验证它还在列表里）
	class ShapeBase* attachTo{ nullptr };
	float attachCx{ 0.f }, attachCy{ 0.f };   // 挂点（线段中点），编辑中也要按它居中
private:
	void makeTextLayout();
	// 编辑中插入符在标注坐标系里的位置 / 行高（文字矩形 + 排版内 hit-test）
	D2D1_POINT_2F editCaretPos(float& lineH) const;
	void paintPunch(ID2D1DeviceContext* ctx);
	void setAttr();
	void syncRectFromLayout();
	void updateDraggers();
	// 气泡外框：圆角矩形 + 尾巴（一条闭合路径：走到尾巴根部跳过一小段再连到尾巴尖）
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildBubbleGeometry() const;
	// 爆裂外框：7 个尖角 + 内凹曲线（顶部是凹谷）
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildBurstGeometry() const;
	// 每个爆裂一份固定随机抖动（控制点/顶点），保证同一形状多次重绘不变、不同形状各不一样
	void initBurstJitter();
	// 气泡/爆裂二选一
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildFrameGeometry() const;
	// 气泡/爆裂描边：跟随标注的虚线/圆头/线宽
	void updateFrameStyle();
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> frameStyle_;
	bool frameDash_{ false }, frameRound_{ false };
	// 气泡命中：只有描边附近 / 文字上算命中（中间空白不抢点击）
	bool hitBubbleStroke(float x, float y, float band) const;
	// 爆裂命中：轮廓或内部都算（空框也要能点选）
	bool hitBurstShape(float x, float y, float band) const;
	bool hitBubbleText(float x, float y) const;    // 按文字实际排版尺寸判，不是整个框
	// 选中框与本体可见描边之间留 2px（描边中心在 rect 上，外面还有半个线宽）
	float selectionExtra() const override { return isFrame() ? bubbleStroke_ * 0.5f : 0.f; }
	// 气泡尾巴尖：默认跟着矩形走；用户拖过之后保留手动位置
	void updateBubbleTail();
private:
	std::wstring text;
	// 最近一次自动生成的文字（尺寸/角度）。用户把内容改成别的值之后就停止自动覆盖；
	// 只是双击进编辑、内容没改，仍然继续跟着测量刷新
	std::wstring autoText_{};
	float fontSize{ 20.f };
	float angle{ 0.f }; // 弧度
	bool isBold{ false }, isItalic{ false };
	bool bubble_{ false };
	bool burst_{ false };
	// 爆裂随机抖动：每段 6 个 [-1,1] 系数（c1x,c1y,c2x,c2y,ex,ey），创建时生成一次
	std::vector<float> burstJitter_;
	float bubbleStroke_{ 2.f };              // 气泡描边粗细（取标注滑条值）
	D2D1_POINT_2F bubbleTail_{ 0.f, 0.f };   // 尾巴尖（标注坐标）
	bool tailCustom_{ false };               // 用户拖过尾巴尖后不再跟随默认位置
	// 气泡圆角（四角统一）：选中后拖四角内侧的圆角点调整
	float bubbleRadius_{ 14.f };
	float dotStartRadius_{ 0.f };
	float dotPressX_{ 0.f }, dotPressY_{ 0.f };
	int dotPressIndex_{ -1 };
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
	// 见 makeTextLayout / syncRectFromLayout
	float attachLineSlack{ 0.f };
	float pressX{ 0.f }, pressY{ 0.f };
	float resizeStartFont{ 20.f };
	D2D1_RECT_F resizeStartRect{};
	// 气泡/爆裂旋转后的角缩放：沿用 ShapeBase 的世界锚点缩放，避免直接改 rect 造成漂移
	bool scaling_{ false };
	int scaleHandle_{ 0 };
	int rotateCorner{ 0 }; // 光标按角挑字形（0=左上 2=右上 4=右下 6=左下）
	float rotateStartAngle{ 0.f };
	float rotateStartAtan{ 0.f };
};
