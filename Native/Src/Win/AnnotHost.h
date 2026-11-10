#pragma once
#include <include/Ling.h>

class ToolCap;
class ToolSub;
class ShapeBase;
class ShapeText;
class ShapeLine;
class History;

// 截图窗 / 桌面钉图共用的标注宿主（全屏纸或钉图纸）。Shape/History 只认这个，不再分两套。
class AnnotHost : public Ling::WinBase
{
public:
	~AnnotHost();
	std::wstring getCurToolId() const;
	// 属性栏/选中样式使用的工具 id：尺寸是独立工具 id，但界面与属性仍归在 arrow 家族
	std::wstring styleToolId() const;
	virtual void forwardKey(UINT key);
	// 嵌入到别的窗口时：鼠标捕获 / IME 用哪个 HWND（默认就是自己的窗口）。
	// 图片编辑器的标注宿主是一个隐藏窗口，需要把捕获与输入法都交给宿主窗口。
	virtual HWND annotInputHwnd() const { return hwnd; }
	// 供嵌入画布把标注画到自己的 D2D 上下文里（坐标系由调用方先设好）
	void paintAnnotPublic(ID2D1DeviceContext* ctx) { paintShapes(ctx); }
	// 线上文字「开洞」时，用宿主自己的背景填这块区域（截图=捕获图；图片编辑=画板/画布底）
	virtual void paintPunchBackground(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r);
	Ling::TextBox* getTextBox();
	void setEditingText(ShapeText* shape);
	// 属性滑条实时作用的形状：优先二次选中的；否则是刚画完、且仍属于当前工具的标注
	ShapeBase* liveStyleShape();
	// 新建文字气泡：等这次 mouseUp/annotUp 整条事件走完，再进文本编辑（避免输入框抢在
	// 形状这一帧提交前被 focus/合成，盖住标注和四周遮罩）
	void requestEditText(ShapeText* shape);
	// 自带文字编辑器：插入符定时刷新 + 字符/按键输入（不经过 TextBox 画布）
	void beginTextEdit();
	void endTextEdit();
	void annotChar(UINT ch);
	// 把输入法组字窗贴到插入符、候选框摆到插入符下方（见 .cpp 注释）
	void positionImeWindow();
	void onToolStyleChanged();
	virtual void onToolChanged() {}
	// 清理已退出编辑状态的 editingText 残留（见 .cpp 注释）
	void dropStaleEditingText();
	// 贴图「画面不透明度」（百分比）。仅 WinPin 有意义；ToolSub 的「透明度」属性栏通过它读写。
	// 基类默认无操作，非贴图宿主（截图等）调用不会有效果。
	virtual int  pinOpacityPercent() const { return 100; }
	virtual void setPinOpacityPercent(int /*percent*/) {}
	// 下层标注变了（拖动/改属性/撤销删除）→ 重新采样它上面盖着的模糊/马赛克。
	// changed 传 nullptr 表示"全部滤镜都重算"。z 序后面的才是盖在上面的。
	void refreshFiltersAbove(ShapeBase* changed);
	void syncViewportFilters();
	// shapePreview：预览形状本身（多边形边数、星形角数、圆角），否则预览笔迹粗细
	void setSizePreviewHold(bool hold, bool shapePreview = false);
	// 二次选中了"当前工具"的标注：改滑条会直接体现在本体上，不再画额外的画布预览
	bool sizePreviewSuppressed() const;
	// shapePreview：预览形状本身（多边形边数、星形角数、圆角），否则预览笔迹粗细
	void pulseSizePreview(bool shapePreview = false);
	// 丢弃未抬手的草稿形状及其交互状态（右键分层取消的公共路径）
	void discardDraft();
	bool hasSelectedAnnot() const;
	static std::wstring toolIdOfShape(ShapeBase* s);
	// 属性栏展示当前选中标注的主尺寸（线宽/字号等）
	void presentSelectedAnnotStyle();
	// 二次选中后将标注属性载入工具与属性栏（延后一拍调用，避免在鼠标按下中重建属性栏）
	void applySelectedAnnotStyleToTool();
	// —— 多选（shapeHover 是主选中，extraSelection_ 是其余选中）——
	bool hasMultiSelection() const { return !extraSelection_.empty(); }
	// 当前选中的全部标注，主选中在前；已按历史表校验存活
	std::vector<ShapeBase*> selectedShapes() const;
	bool isShapeSelected(ShapeBase* s) const;
	// 用画板像素矩形做框选（图片编辑器的橡皮筋同时选容器与标注）
	void selectShapesInRect(const D2D1_RECT_F& rect, bool additive);
	bool hasAnyAnnotSelection() const;
	void clearAnnotSelectionPublic() { clearAnnotSelection(true); }
	// 当前工具对应的目标集合：按工具 id + 形态（大箭头/普通箭头/线段/气泡…）过滤
	std::vector<ShapeBase*> selectedShapesForTool(const std::wstring& toolId) const;
	virtual void layoutTools() = 0;
	virtual void onHistoryChanged();
	// 撤销/重做后：多选组框要按恢复后的几何重新包一遍（否则框停在旧位置）
	void onHistoryUndoRedo();
	// 属性栏锚定主工具条（截图 ToolCap / 录屏 ToolVideo）
	struct MainBarAnchor {
		float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
		float btnCenterX{ 0 };
		float dpi{ 1.f };
	};
	virtual bool queryMainBarAnchor(MainBarAnchor& out) const;
	// 指定属性栏跟随的按钮（白板等没有"当前工具"的栏）；空串表示跟随当前工具
	void setMainBarAnchorId(const std::wstring& id);
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
	// 标注坐标下的底图范围（视口滤镜铺满用）。空矩形表示由 History 按窗口/底图尺寸推断。
	virtual D2D1_RECT_F annotContentRect() const { return D2D1::RectF(0.f, 0.f, -1.f, -1.f); }
	// 按当前工具/选中态设置光标（WinCap/WinPin::setCursor 调用）
	void applyAnnotCursor();
	void applyToolDrawCursor();
	// 白板「编辑背景」是否开启：决定新标注是否归入背景层、背景层能否被点选
	virtual bool boardBackgroundEditing() const { return false; }
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
	// 刚画完的标注：滑条（角数/边数/圆角）仍可继续作用在它上面
	ShapeBase* lastCreatedShape_{ nullptr };
	// 角度三点式：基线画完后等第三点的形状（下一次点击/拖拽继续给它）
	ShapeLine* pendingAngle{ nullptr };
	// 钢笔式点按：箭头/线段已经落下锚点、等下一次左键或右键收笔
	ShapeBase* pendingPen{ nullptr };
	// 按下后锁定的控点索引；拖动全程强制使用，避免重探命中把旋转/缩放打断成移动
	int annotDragIndex{ -1 };
	// 结束文字编辑后短暂吞掉下一次点选/落笔（失焦可能先于 annotDown）
	ULONGLONG annotSuppressUntil{ 0 };
	bool hasDragged{ false };
	// 仅纯点选（无拖动 / 缩放 / 旋转）后载入该标注的样式；拖动几何时不影响工具默认值
	ShapeBase* styleLoadCandidate{ nullptr };
	std::unique_ptr<History> history;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> screenImg;
	// 离屏合成时的「开洞底图」：保存/导出时当前目标就是这张底图，不能再用画布兜底色
	ID2D1Bitmap1* punchSource_{ nullptr };
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
	// —— 钢笔式点按（箭头/线段）——
	bool penPendingAlive() const;
	void finalizePendingPen();
	// 右键收笔：不追加锚点，收在最后一次单击的落点；返回 true 表示这次右键已被收笔消费
	bool finishPenStroke(const POINT& pos);
	void annotKey(UINT key);
	void annotTimer(UINT id);
	virtual POINT toImgPos(const POINT& pos) const;
	// 按下时命中（优先当前选中的控点）
	ShapeBase* hitShapeAt(float x, float y);
	// 方向键微调当前选中的标注（dx/dy 为标注坐标增量）：没有可移动的选中标注时返回 false
	bool nudgeSelectedAnnot(float dx, float dy);
	// 尺寸/角度：在线上挂一句自动文字（已存在则直接返回）；拖拽过程中就调用 → 文字实时出现
	ShapeText* ensureShaftText(ShapeBase* host, const std::wstring& text, float px);
	// 尺寸自带文字：按屏幕 DPI 把线长换算成物理尺寸（cm）；用户双击改过之后不再自动改写
	std::wstring dimensionTextFor(ShapeBase* host) const;
	void clearAnnotSelection(bool keepStylePreview = false);
	// —— 多选：框选 / 组变换 ——
	bool hasSelectableShapes() const;          // 有没有可被框选的标注（决定要不要起框）
	void beginMarquee(const POINT& imgPos);
	void updateMarquee(const POINT& imgPos);
	void finishMarquee();
	void toggleShapeInSelection(ShapeBase* s);
	void pruneAnnotSelection();                // 撤销/删除后丢掉悬空指针
	D2D1_RECT_F selectionBounds() const;
	// 组控件：和普通矩形控件同一套（8 个缩放柄 + 角外旋转圈），整体一个角度
	void syncGroupBoxFromSelection();
	bool groupBoxContains(float x, float y) const;
	int  hitGroupHandle(float x, float y) const;        // 0..7，未命中 -1
	int  hitGroupRotateCorner(float x, float y) const;  // 0/2/4/6，未命中 -1
	void beginGroupTransform(bool rotate, const POINT& imgPos);
	void updateGroupTransform(const POINT& imgPos, bool rotate);
	void finishGroupTransform();
	void beginGroupScale(int handle, const POINT& imgPos);
	void updateGroupScale(const POINT& imgPos);
	void finishGroupScale();
	void paintSelectionChrome(ID2D1DeviceContext* ctx);
	void paintMarquee(ID2D1DeviceContext* ctx);
	// 清空全部标注：**必须先断开 shapeHover / newShape / editingText 这些裸指针再释放**
	// （否则它们悬垂，下一次 hasSelectedAnnot()/paint 就会在已释放对象上做虚调用 → 闪退）
	// keepBackground：保留「背景层」标注（白板未开启编辑背景时的重置画布）
	void clearAllShapes(bool keepBackground = false);
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
	// 按住 Alt = 强制落笔：有工具时跳过"按下先命中已有标注"和悬停手型，让笔直接压在旧标注上。
	// 没有选中工具时不生效（此时仍按"点谁选谁"处理）。
	bool altForceDraw() const;
	// 尺寸工具（标注栏的「尺寸」形态）：只绘制尺寸线，不点选 / 拖动 / 连线已有标注
	bool dimensionToolActive() const;
protected:
	Ling::TextBox* textBox{ nullptr };
	ShapeText* editingText{ nullptr };
	ShapeText* pendingEditText{ nullptr };   // 等 213 定时器到点再进编辑
	bool annotMouseDown{ false };
	bool prevPressCreatedShape{ false };
	bool sizePreviewHold{ false };
	ULONGLONG sizePreviewPulseUntil{ 0 };
	// 最近一次调整的是形状参数（边数/角数/圆角）还是笔迹粗细
	bool sizePreviewShape{ false };
	// 拖动已有标注：记录编辑前的几何，抬手时补一条撤销步骤
	ShapeBase* editStepTarget{ nullptr };
	std::vector<float> editStepBefore;
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
	// 连线提示（起点"＋"只属于箭头工具；叉是"箭头工具 + 空工具"）：
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
	// —— 多选状态 ——
	std::vector<ShapeBase*> extraSelection_;   // 不含 shapeHover
	bool marqueeActive_{ false };
	D2D1_POINT_2F marqueeStart_{ 0.f, 0.f };
	D2D1_RECT_F marqueeRect_{};
	bool groupMoveActive_{ false };
	bool groupRotateActive_{ false };
	bool groupScaleActive_{ false };
	int groupScaleHandle_{ -1 };
	D2D1_RECT_F groupBox_{};
	float groupAngle_{ 0.f };
	D2D1_RECT_F groupScaleStartRect_{};
	D2D1_POINT_2F groupScaleStartCenter_{ 0.f, 0.f };
	D2D1_POINT_2F groupScaleLastCenter_{ 0.f, 0.f };
	float groupScaleLastSx_{ 1.f }, groupScaleLastSy_{ 1.f };
	D2D1_POINT_2F groupTransformStart_{ 0.f, 0.f };
	D2D1_POINT_2F groupLastDelta_{ 0.f, 0.f };
	D2D1_POINT_2F groupPivot_{ 0.f, 0.f };
	float groupRotateStartAngle_{ 0.f };
	float groupRotateLastDelta_{ 0.f };
	std::vector<ShapeBase*> groupTargets_;
	std::vector<std::vector<float>> groupBefore_;
	// 组选框/框选的画刷（懒建）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushGroupStroke;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushGroupHandleFill;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushGroupHandleShadow;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> groupDashStyle;
	std::wstring forcedToolId;
	bool mainToolUserPlaced{ false };
};
