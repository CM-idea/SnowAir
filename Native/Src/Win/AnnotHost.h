#pragma once
#include <include/Ling.h>

class ToolCap;
class ToolSub;
class ShapeBase;
class ShapeText;
class History;

// 截图窗 / 桌面钉图共用的标注宿主（全屏纸或钉图纸）。Shape/History 只认这个，不再分两套。
class AnnotHost : public Ling::WinBase
{
public:
	~AnnotHost();
	std::wstring getCurToolId() const;
	virtual void forwardKey(UINT key);
	Ling::TextBox* getTextBox();
	void setEditingText(ShapeText* shape);
	void onToolStyleChanged();
	// 下层标注变了（拖动/改属性/撤销删除）→ 重新采样它上面盖着的模糊/马赛克。
	// changed 传 nullptr 表示"全部滤镜都重算"。z 序后面的才是盖在上面的。
	void refreshFiltersAbove(ShapeBase* changed);
	// 拖动过程中用的限流版：重采样含 CPU 模糊（O(w*h*r)），全屏滤镜每帧重算会卡；
	// 抬手时再调一次不限流的做收尾，保证最终一定准确。
	void refreshFiltersAboveThrottled(ShapeBase* changed);
	void syncViewportFilters();
	void setSizePreviewHold(bool hold);
	void pulseSizePreview();
	bool hasSelectedAnnot() const;
	// 打开标注工具且不取消当前选中（截图 / 钉图 / 长图 / 录屏）
	virtual void activateAnnotTool(const std::wstring& toolId) {}
	static std::wstring toolIdOfShape(ShapeBase* s);
	// 属性栏展示当前选中标注的主尺寸（线宽/字号等）
	void presentSelectedAnnotStyle();
	virtual void layoutTools() = 0;
	virtual void onHistoryChanged();
	// 属性栏锚定主工具条（截图 ToolCap / 录屏 ToolVideo）
	struct MainBarAnchor {
		float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
		float btnCenterX{ 0 };
		float dpi{ 1.f };
	};
	virtual bool queryMainBarAnchor(MainBarAnchor& out) const;
	// 录屏等场景：不走 ToolCap 时强制当前工具 id
	void setForcedToolId(const std::wstring& id) { forcedToolId = id; }
	void clearForcedToolId() { forcedToolId.clear(); }
	void requestFadeTick();
	// 用户拖过主工具条后，layout 不再把栏弹回默认位（属性栏仍跟主栏）
	void markMainToolUserPlaced() { mainToolUserPlaced = true; }
	void clearMainToolUserPlaced() { mainToolUserPlaced = false; }
	bool isMainToolUserPlaced() const { return mainToolUserPlaced; }
	// 标注坐标 → 窗口客户区（钉图乘 scale；长图按选区视口映射）
	virtual D2D1_POINT_2F annotToClientPt(float ax, float ay) const;
	virtual D2D1_POINT_2F clientToAnnotPt(float cx, float cy) const;
	// 按当前工具/选中态设置光标（WinCap/WinPin::setCursor 调用）
	void applyAnnotCursor();
	void applyToolDrawCursor();
	// 离屏合成底图+标注；crop 非空则只输出该矩形（全屏坐标）
	bool getAnnotPixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size, const D2D1_RECT_F* crop = nullptr);
	// 长图：在已有 BGRA 底图上叠标注（形状已是长图像素坐标）
	bool composeAnnotOnImage(const BYTE* bgra, int imgW, int imgH, std::vector<BYTE>& out);
protected:
	virtual 	void applyEmptyToolCursor();
	void paintSerialCursor(ID2D1DeviceContext* ctx);
	// 「线段中间打字」：文本工具悬停在线段/箭头中间时，显示一块空白（底图抠回来）示意可以打字
	void updateTextMidHint(const POINT& imgPos, const std::wstring& tool);
	void paintTextMidHint(ID2D1DeviceContext* ctx);
	// 连线：线中点上的叉（悬停出现，点它**删掉这条箭头/线段**）+ 悬停形状时的落点提示点
	void updateLinkHints(const POINT& imgPos, const std::wstring& tool);
	void paintLinkHints(ID2D1DeviceContext* ctx);
	// 起点旁边那句"拖动可连线"
	void paintLinkHintText(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& anchor);
	void paintSizePreview(ID2D1DeviceContext* ctx);
	// 尺寸预览中心：截图选区中心 / 钉图画面中心
	virtual bool sizePreviewOrigin(float& cx, float& cy, float& halfSpan) const { return false; }
	ShapeBase* probeShapeUnder(float x, float y, ShapeBase* preferFirst);
public:
	float scale{ 1.f };
	std::unique_ptr<ToolCap> toolCap;
	std::unique_ptr<ToolSub> toolSub;
	ShapeBase* shapeHover{ nullptr };
	ShapeBase* newShape{ nullptr };
	// 按下后锁定的控点索引；拖动全程强制使用，避免重探命中把旋转/缩放打断成移动
	int annotDragIndex{ -1 };
	// 结束文字编辑后短暂吞掉下一次点选/落笔（失焦可能先于 annotDown）
	ULONGLONG annotSuppressUntil{ 0 };
	bool hasDragged{ false };
	std::unique_ptr<History> history;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> screenImg;
protected:
	AnnotHost();
	void ensureHistory();
	void paintShapes(ID2D1DeviceContext* ctx);
	void paintShapes(ID2D1DeviceContext* ctx, bool withChrome); // 预览条可关控点/序号跟手
	// skipEphemeral：离屏导出时跳过渐隐画笔等临时笔迹
	void paintShapes(ID2D1DeviceContext* ctx, bool withChrome, bool skipEphemeral);
	// 画笔交互（坐标 = 窗口客户区 / 底图像素，scale 由 toImgPos 处理）
	bool annotDown(POINT pos, BOOL isRight);
	void annotMove(POINT pos);
	void annotUp(POINT pos, BOOL isRight);
	void annotKey(UINT key);
	void annotTimer(UINT id);
	virtual POINT toImgPos(const POINT& pos) const;
	// 按下时命中（优先当前选中的控点）；对齐 QT hitTestOp
	ShapeBase* hitShapeAt(float x, float y);
	void clearAnnotSelection();
	// 清空全部标注：**必须先断开 shapeHover / newShape / editingText 这些裸指针再释放**
	// （否则它们悬垂，下一次 hasSelectedAnnot()/paint 就会在已释放对象上做虚调用 → 闪退）
	void clearAllShapes();
	bool shouldPaintAnnotHandles() const;
	void lockAnnotDragFromHover();
	void applyLockedAnnotDrag();
	// 右键切工具条、双击复制等钉图专属手势：返回 true 表示已处理
	virtual bool annotRightClick() { return false; }
	virtual bool annotDoubleClick() { return false; }
	virtual bool annotEmptyToolDrag(POINT /*pos*/) { return false; } // 无工具时拖窗等
	virtual void annotAfterEmptyToolUp() {}
	// 录屏等：工具启用时只落笔、不点选已有标注（避免画着被拖走）
	virtual bool annotSkipHitSelect() const { return false; }
protected:
	Ling::TextBox* textBox{ nullptr };
	ShapeText* editingText{ nullptr };
	bool annotMouseDown{ false };
	bool prevPressCreatedShape{ false };
	bool sizePreviewHold{ false };
	ULONGLONG sizePreviewPulseUntil{ 0 };
	bool numberCursorFollow{ false }; // 序号工具跟手预览（系统光标隐藏时）
	// 文本工具落在线段中间：显示的空白位（图像坐标）
	bool textMidHintActive{ false };
	// 这条线上已经有文字了：不再挖空（悬停只需给工字型 + 点击进编辑）
	bool textMidHasLabel{ false };
	D2D1_RECT_F textMidHint{};	POINT annotPressPos{ 0, 0 };
	// 连线：箭头工具按进"没选中的矩形/椭圆"起笔时记下它 —— 若这一次只是单击（没拖），
	// annotUp 会把空线删掉并改成选中这个形状
	ShapeBase* linkPressTarget{ nullptr };
	// 连线：当前悬停着"叉"的那条线，以及叉的位置（图像坐标）
	ShapeBase* linkBtn{ nullptr };
	D2D1_POINT_2F linkBtnPos{ 0.f, 0.f };
	// 连线提示：
	//   linkHoverTarget —— 起点所在的形状（悬停时的可连形状 / 拖动中=起点所属的形状）
	//   linkAnchorDot / linkShowDot —— 起点那个点（和控点同款）：悬停时画在形状边缘的落点上，
	//                                 拖动中一直跟着线起点。**目标形状上不画**
	ShapeBase* linkHoverTarget{ nullptr };
	D2D1_POINT_2F linkAnchorDot{ 0.f, 0.f };
	bool linkShowDot{ false };
	// 叉与提示点的画刷（懒建，和控点同一套配色）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLinkBtnFill;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLinkBtnBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLinkBtnShadow;
	// "拖动可连线"那句话的胶囊底 + 字色
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLinkHintBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLinkHintFg;
	ULONGLONG annotLastDownTime{ 0 };
	POINT annotLastDownPos{ 0, 0 };
	std::wstring forcedToolId;
	bool mainToolUserPlaced{ false };
	ULONGLONG lastFilterRefreshMs{ 0 };
};
