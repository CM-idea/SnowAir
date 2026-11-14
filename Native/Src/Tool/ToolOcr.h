#pragma once
#include <include/Ling.h>
#include <functional>
#include "../Ocr/OcrTypes.h"

class Tip;
class ToolOcrCompare;
// 选区 OCR/翻译结果浮层：底色取自选区原图（采样）+ 多块可编辑文本
// 宿主从 WinCap 泛化为 WinBase —— 截图窗（WinCap）与桌面贴图（WinPin）共用同一套面板。
class ToolOcr : public Ling::WinBase
{
public:
	ToolOcr(Ling::WinBase* host, OcrResult result, bool translateMode,
		std::vector<BYTE> image, int imageW, int imageH);
	~ToolOcr();
	void syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY);
	// 宿主是贴图时置 true（须在建窗之前设）：不显示右上角复制按钮；
	// 且只有识别文本框吃鼠标（可选中/复制文本），其余区域穿透 —— 贴图仍能右键弹菜单 / 滚轮缩放。
	void setPinOverlay(bool on) { pinOverlay_ = on; }
	// 贴图宿主用：复制当前选中（无选区则整段）文字，不关闭任何窗口
	void copyText() { copyAll(); }
	// 贴图宿主用：客户端坐标是否落在识别文字（含背景块）上 —— 落上则鼠标归文字，不拖窗口
	bool hitText(POINT client);
	// 贴图宿主注入：宿主的悬停控件（锁定/编辑/关闭）画在本浮层最上层。
	// 浮层在贴图之上，两个独立窗口没法互相穿插，只能把控件镜像过来画。
	void setChromePainter(std::function<void(ID2D1DeviceContext*)> fn) { chromePainter_ = std::move(fn); }
	// 悬停轮询（浮层客户区坐标；不在贴图上时由宿主自己隐藏控件）
	void setChromeHover(std::function<void(POINT)> fn) { chromeHover_ = std::move(fn); }
	// 控件命中 / 按下：返回 true 表示这次按下归控件，由宿主执行动作
	void setChromeHitTest(std::function<bool(POINT)> hit, std::function<bool(POINT)> down)
	{
		chromeHit_ = std::move(hit);
		chromeDown_ = std::move(down);
	}
	void repaintChrome() { if (hwnd) refresh(); }
	void applyTranslate(const std::vector<std::wstring>& lines);
	// 翻译模式专用：把"识别出的区域 + 译文"一次性装上并重绘一次 —— 中间不显示识别出来的原文，
	// 避免"先闪一下原文、再被译文替换"。
	void setTranslatedResult(OcrResult result, const std::vector<std::wstring>& lines);
	void setResult(OcrResult result);
	// 结果来自缓存：直接显示，不播扫框/淡入动效
	void suppressReveal() { revealPending_ = false; suppressReveal_ = true; }
	// 翻译等待期：识别一完成就按识别到的行循环扫框（不显示文字/译文）。
	// 译文到了之后由 setTranslatedResult 的扫框接内容淡入（复用同一套框）。
	void startScanWait(OcrResult scan);
	// 识别还没回来（没有行信息）时先给一条横贯选区的扫描线，点下去立刻有反馈；
	// 识别一完成就换成逐行扫框（startScanWait / startReveal）。
	void startScanPending();
	// 收掉等待期的扫描线与循环扫框（识别失败、不再等待时用）
	void stopScanWait();
	void showBanner(const std::wstring& msg, bool error);
	void clearBanner();
	bool translateMode() const { return translateMode_; }
	const OcrResult& result() const { return result_; }
	// 选区原图（BGRA）。翻译模式先建面板、后识别，识别直接复用这份像素，省掉再留一份。
	const std::vector<BYTE>& srcImage() const { return srcImage_; }
	// 截图宿主注入：双击文字 = 「完成」。text 为空表示"复制整段"（host 按原逻辑处理）；
	// 非空表示拖选后按 Ctrl+C 的那一段 —— host 只复制这段并退出。
	// 浮层是独立置顶窗口，双击/快捷键本来只会落到文字上，截图窗收不到。
	void setOnConfirm(std::function<void(const std::wstring&)> fn) { onConfirm_ = std::move(fn); }
	// 右键：收起结果面板（由宿主接着处理后续的右键层级）
	void setOnDismiss(std::function<void()> fn) { onDismiss_ = std::move(fn); }
	// 选区是"单击元素"得到的：此时选区就是元素外框，排版可以按这个宽度满排
	void setElementBlock(bool v) { elementBlock_ = v; }
	// 宿主注入：把（截图/贴图的）标注画到识别文字**之上**。识别结果浮层是独立置顶窗口，
	// 而标注画在宿主窗口里，两个窗口没法互相穿插 —— 于是把标注"镜像"到本窗口顶层，
	// 视觉上就变成"标注盖在识别结果上面"。
	void setAnnotPainter(std::function<void(ID2D1DeviceContext*)> fn) { annotPainter_ = std::move(fn); }
	// 标注变化时由宿主调用，重画镜像层。
	void repaintOverlay() { if (hwnd) refresh(); }
	// 主动重绘本面板（提示条/文字改动后调用，别指望宿主那次 refresh 会带上本窗口）
	void repaint() { if (hwnd) refresh(); }
protected:
	// 覆盖矩形就是选区的精确物理尺寸：DPI 变了由本类按选区重贴，不套系统的建议矩形
	bool keepGeometryOnDpiChange() const override { return true; }
private:
	void onCreated() override;
	void layout() override;
	void rebuildEditors();
	// 结果落地动效：逐行画描边框，框淡出后该行内容淡入（见 startReveal）
	void startReveal();
	// 动效第二步：真正下发透明度关键帧（下一拍执行，代次对不上就作废）。
	// loop=true 用于翻译等待期：只循环扫框、不碰内容（这时还没有编辑框）。
	void animateReveal(bool loop = false);
	// 按识别框逐行建扫描框（描边 + 极淡填充；斜排跟随四点角度）
	void buildScopes(const std::vector<OcrRegion>& regions);
	// 等待识别的扫描线：亮线 + 拖尾都用普通节点画（识别框/文字也是节点，这条路确定能出画面）
	void buildScanLine();
	void layoutScanLine();
	void stopScanLine();
	void applyMaskGeometry();
	// 把译文本行填进各区域的 displayText 并重算整段文字（不重绘）
	void applyTranslateLines(const std::vector<std::wstring>& lines);
	void copyAll();
	// 文本重排的编辑回退：Ctrl+Z / Ctrl+Y 走本类自己的栈（TextBox 本身没有 undo）
	void undoRedoReflow(bool redo);
	// 右上角「对比」：弹出/收起选区原图浮窗
	void toggleCompare();
	// 对比浮窗打开时按钮显示选中态（主题强调色）
	void refreshCompareBtnState();
	// 悬停轮询：鼠标进入选区时显示右上角按钮，离开则隐藏
	void updateButtonsHover();
	// —— 跨行选区（每行是独立控件，选中范围由本类统一算）——
	struct SelPoint { int line{ -1 }; int index{ 0 }; };
	SelPoint hitSelPoint(POINT client) const;
	void applySelection();
	void clearSelection();
	std::wstring selectedText() const;
	LRESULT onHitTest(const POINT pos) override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	BOOL setCursor() override;
	// 命中"文字带"：文本框外扩一圈
	bool nearText(POINT client) const;
private:
	Ling::WinBase* host;
	// 覆盖矩形（客户区坐标）：onHitTest 用它把选区外的部分设为穿透
	D2D1_RECT_F maskRect_{};
	bool hasMask_{ false };
	bool pinOverlay_{ false };
	bool elementBlock_{ false };   // 选区来自元素查找（单击元素）：宽度按元素外框满排
	// 上次建编辑框时的面板物理尺寸：仅尺寸变化才重建（拖动只挪窗口，不必重建）。
	int panelW_{ 0 };
	int panelH_{ 0 };
	// 每个识别框的"背景块"（严格等于识别框尺寸，负责盖住原文字）与文本框分开存。
	std::vector<Ling::Node*> bgs_;
	// 逐行动效的扫描框：动效结束后 Opacity=0 留在树上，下次重建统一删掉
	std::vector<Ling::Node*> scopes_;
	// 结果落地才播动效；选区/DPI 变化引起的重建不重播
	bool revealPending_{ true };
	// 缓存命中：下一次结果落地也不播动效（suppressReveal 设置，落地后自动复位）
	bool suppressReveal_{ false };
	// 翻译等待期的循环扫框：只画框不建文字，结果落地后作废
	bool scanWait_{ false };
	std::vector<OcrRegion> scanRegions_;
	// 识别前的扫描线：节点从下往上扫，相位 0→1
	bool scanLineOn_{ false };
	float scanPhase_{ 0.f };
	ULONGLONG scanStartTick_{ 0 };
	Ling::Node* scanTrail_{ nullptr };   // 扫描线只有它：竖向线性渐变（0% → 100%，整体 20% 透明）
	// 结果比扫描线先到：先压住，等扫描线走够最短时长再落地，动画才看得见
	std::function<void()> pendingApply_;
	static constexpr ULONGLONG kScanMinMs{ 550 };
	// 重建代次：排队中的动效只在"最后一次重建"上生效
	unsigned buildGen_{ 0 };
	OcrResult result_;
	bool translateMode_{ false };
	// 文本重排：整段结果放进一个普通可编辑文本框（Enter 换行），不再逐框覆盖原位置
	bool reflow_{ false };
	// 文本重排的编辑历史（每次文字变化记一步，Ctrl+Z/Ctrl+Y 在这里走）
	std::vector<std::wstring> reflowHistory_;
	int reflowHistPos_{ -1 };
	bool reflowHistGuard_{ false };
	std::wstring banner_;
	bool bannerError_{ false };
	Ling::Node* bannerNode{ nullptr };
	Ling::Label* bannerLabel{ nullptr };
	std::vector<Ling::TextBox*> editors;
	// 翻译：译文按原行宽折行后纵向顺延，超出选区时用滚轮滚动（不画滚动条）
	std::vector<Ling::TextBox*> trBoxes_;
	std::vector<float> trTops_;    // 各框未滚动时的顶端（物理像素）
	float trScrollY_{ 0.f };
	float trMaxScroll_{ 0.f };
	Ling::Button* copyBtn{ nullptr };
	Ling::Button* compareBtn{ nullptr };
	bool buttonsShown_{ false };
	std::unique_ptr<Tip> tip;
	// 与其它窗口的定时器 id 错开（属性栏 0x5000 / 系统按键 0x5100）
	static constexpr UINT kHoverTimerId{ 0x5200 };
	static constexpr UINT kHoverPollMs{ 100 };
	// 扫描线：每帧推进相位，一轮扫完的时长
	static constexpr UINT kScanTimerId{ 0x5300 };
	static constexpr UINT kScanTickMs{ 8 };
	static constexpr float kScanSweepMs{ 300.f };
	int hostX_{ 0 }, hostY_{ 0 };   // 宿主窗口位置（覆盖矩形是宿主客户坐标）
	// 上次建编辑框时的 dpi：换屏 / 改缩放后 DPI 变了，字号和坐标都得按新 dpi 重排。
	float builtDpi_{ 0.f };
	int imgW{ 1 }, imgH{ 1 };
	// —— 跨行选区状态（阅读顺序：行优先；index = 行内字符偏移）——
	// selDragging_ 只表示"鼠标正按着拖选"；Ctrl+A 全选只设端点、不进入拖选，
	// 否则鼠标只是移动（没有按下）也会被 onMouseMove 当成扩选，选区跟着光标跑。
	bool selDragging_{ false };
	int selALine_{ -1 }, selAIdx_{ 0 }, selBLine_{ -1 }, selBIdx_{ 0 };
	POINT selAnchor_{}, selCurrent_{};
	bool selSpatial_{ false };
	// —— 标注镜像层：盖在识别文字之上的画布 ——
	Ling::Canvas* annotCanvas_{ nullptr };
	std::function<void(ID2D1DeviceContext*)> annotPainter_;
	// —— 贴图控件镜像：压在文字与标注之上（复用标注镜像层，保证新建文字块后仍在最上层）——
	std::function<void(ID2D1DeviceContext*)> chromePainter_;
	std::function<void(POINT)> chromeHover_;
	std::function<bool(POINT)> chromeHit_;
	std::function<bool(POINT)> chromeDown_;
	std::function<void(const std::wstring&)> onConfirm_;
	std::function<void()> onDismiss_;
	// 双击判定自己做：Ling 的窗口类没带 CS_DBLCLKS，WM_LBUTTONDBLCLK 不会来
	ULONGLONG lastDownTick_{ 0 };
	POINT lastDownPt_{};
	// 选区原图（BGRA，逐行自上而下）——用于采样叠层底色；底面不透明，文字色按底色亮度反相
	std::vector<BYTE> srcImage_;
	uint32_t palBg_{ 0xFFFFFFF2 };
	uint32_t palFg_{ 0x14181CFF };
	// 对比原图浮窗（后声明：先于 srcImage_ 析构，浮窗不会读到已释放的像素）
	std::unique_ptr<ToolOcrCompare> compare_;
};
