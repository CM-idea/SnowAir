#pragma once
#include <include/Ling.h>
#include <vector>
#include <functional>
#include "../Tool/ToolbarTheme.h"

// 光标落在哪一块。边/角用环带命中，远离选区为 None
enum class MaskHit { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomRight, BottomLeft };

// 信息栏控件命中：锁 / 选区圆角 / 滑条 / 选区阴影 / 阴影色块
enum class InfoHit { None, Lock, Radius, RadiusSlider, Shadow, ShadowSwatch };

// 框选遮罩。四块半透明遮罩 + 蓝色选区边框 + 左上角尺寸标签。
// 自己不持有绘制目标，paint 时由宿主窗口把当前的 context 传进来。
// 所有坐标都是宿主窗口的客户区坐标。
class CutMask
{
public:
	CutMask(Ling::WinBase* win);
	~CutMask();
	// 选区被挪动（拖拽 / 鼠标跟随 / 方向键 / 滚轮）之后的回调，由宿主在创建后设置。
	// 未设置时什么都不做 —— 录制侧靠它把采集裁剪框跟着平移。
	std::function<void()> onRectMoved;
	// 鼠标悬停时吸附到光标下的窗口矩形，选区真的变了才返回 true。
	// force=true 跳过投递节流（确认点击落空后按当前位置立即重查一次用）
	bool highlight(POINT pos, bool force = false);
	// 悬停是否停在有效目标上（区别于"自由拖框"：点一下即选中的窗口元素框）
	bool hoverPicked() const { return hoverTarget_.right > hoverTarget_.left && hoverTarget_.bottom > hoverTarget_.top; }
	void startMakeRect(POINT pos);
	void makeRect(POINT pos);
	// 选区边框附近环带命中；远离选区为 None（不再把整屏切成八块）
	MaskHit hitTest(POINT pos) const;
	// 调整中记下的命中（拖动全程保持，供光标使用）
	MaskHit adjustingHit() const { return adjustHit; }
	bool isResizingEdge() const {
		return adjustHit != MaskHit::None && adjustHit != MaskHit::Inside;
	}
	// 抬手后可强制重建尺寸标签
	void refreshLabel();
	// 开始调整：记下方向和起始矩形。按的是边或角时，这一下就把那条边吸到光标处
	void startAdjust(POINT pos);
	// 调整中：按 startAdjust 记下的方向改选区
	void adjust(POINT pos);
	// 抬手结束调整（清 adjusting 状态，阴影等可恢复）
	void endAdjust();
	// 宽高都大于 0 才算真的框出了东西
	bool hasRect() const;
	void clearRect();
	// 改选区中右键：恢复按下前的矩形
	void cancelAdjust();
	// 平移选区并夹到窗口内；真的移动了返回 true。
	// hostPoint 非空时，录制期的"单块屏"约束以该点定屏（鼠标跟随传光标位置：鼠标一进另一块屏，
	// 选区就整块贴过去）；为空则按选区中心定屏。
	bool translateBy(float dx, float dy, const POINT* hostPoint = nullptr);
	// —— 录制期的「单块屏」约束 ——
	// 屏幕录制只能针对单块显示器采集（跨屏录不全，见 CapVideo::startMp4），故录制阶段要求
	// 选区**完整落在某一块屏里**：框选、拖边、整体平移都会被夹进对应那块屏，不跨交界。
	void setSingleMonitorMode(bool on);
	// 把当前选区收进单块屏（进录屏阶段时调用：跨屏框好的框在这里被收小）；真的改了返回 true
	bool snapToSingleMonitor();
	// 录制期"这块屏能不能跨过去"（虚拟桌面坐标）：只有与当前采集目标挂在同一个显卡适配器上的屏
	// 才换得过去（见 CapVideo::reachableMonitors）。由宿主注入，且**每次现取**：采集侧一旦在某块屏上
	// 换失败（混合显卡常见），随后即可把该屏排除，避免选区先移过去再被回退。未注入时一律按可跨屏处理。
	std::function<bool(const RECT&)> canSwitchToMonitor;
	// withDim=false：只画描边/控点/标签（压暗已提前单独画过，避免被标注绘制状态影响）
	void paint(ID2D1DeviceContext* ctx, bool withDim = true);
	// 只画四周压暗，供"先压暗、再画标注"的顺序使用
	void paintDimLayer(ID2D1DeviceContext* ctx);
	// 录制期用：只画选框描边/尺寸标签（不画压暗），由"排除捕获"的选框层窗口调用。
	// borderOutside=true 时描边整条画在选区外侧：桌面复制不认 WDA_EXCLUDEFROMCAPTURE，
	// 画在里侧的描边会被录进视频。
	void paintChrome(ID2D1DeviceContext* ctx, bool borderOutside = false);
	// 长图阶段：预览图/长图在 cutMask 之后绘制，会把先画的描边盖住。
	// 内容层画完后再调它把绿框补画一遍，「滚动中 / 暂停 / 编辑」全程都能看到选区边界。
	void paintBorderOverlay(ID2D1DeviceContext* ctx);
	// 信息栏控件
	InfoHit hitInfoControl(POINT pos) const;
	bool onInfoDown(POINT pos);
	bool onInfoMove(POINT pos);
	void onInfoUp();
	bool infoInteracting() const { return infoDrag_ != InfoHit::None; }
	// 信息栏悬停 tip 锚点（屏幕物理像素）
	bool infoTipAnchor(InfoHit hit, float& screenX, float& screenY) const;
	float selRadius() const { return selRadius_; }
	int selShadowW() const { return selShadowW_; }
	// 导出时套圆角裁切 + 外阴影；无效果则原样返回 true
	bool applyExportEffects(std::vector<BYTE>& pixels, int& cw, int& ch) const;
public:
	D2D1_RECT_F maskRect{};
	float strokeWidth{ 2.f };
	// 选区定死之后（录屏 / 滚动截图）标签就没用了，而它可能压在选区内部被录进去
	bool hideLabel{ false };
	// 查找元素悬停：只画描边；拖框/已框选才压暗+控点
	bool showDim{ true };
	bool showHandles{ true };
	bool showBorder{ true }; // 长图编辑态可关，只留内容区
	// 识别结果白底：画在描边/控点/信息栏之下
	bool drawQrcodeFill{ false };
	// 查找元素高亮补间（指数追赶 + 查询节流，去掉重开顿挫）
	void onHoverAnimTick(UINT timerId);
	void snapHoverTarget();
	// 查找元素的辅助定时器：光标停稳后对同一点用完整预算再钻一次，直到细化或预算耗尽
	void onHoverDwellTimer(UINT timerId);
	// 按下/退出时立刻撤销查询状态（驻留重查、在途结果作废）；查询本身从不改动遮罩样式或鼠标捕获
	void cancelHoverQuery();
private:
	void makeLayout();
	// 把选区夹进"该归属的那块屏"（录制期用）：hostPoint 非空时以该点定屏（框选按起点、
	// 鼠标跟随传光标、整体拖动传光标），为空则以选区中心定屏。真的改了 r 返回 true。
	// keepIfNoFit 非空时：选区比目标屏还大就**原样退回**（不让它挪到装不下的屏上，录制侧同理）；
	// 为空则收小到屏的尺寸（只用在"跨屏框好再进录屏"和框选过程中）。
	bool clampToSingleMonitor(D2D1_RECT_F& r, const POINT* hostPoint = nullptr,
		const D2D1_RECT_F* keepIfNoFit = nullptr) const;
	bool monitorLocked_{ false };                 // 录制期的单块屏约束开关
	std::vector<D2D1_RECT_F> monitorsDesk_;       // 显示器矩形列表（虚拟桌面坐标）
	void applyAspectLock(D2D1_RECT_F& r) const;
	void toggleAspectLock();
	void toggleSelRadius();
	void toggleSelShadow();
	void setRadiusFromSliderX(float x);
	void pickShadowColor();
	void rebuildRadiusValueLayout();
	void paintSelShadow(ID2D1DeviceContext* ctx) const;
	// 描边 / 控点 / 尺寸标签：主窗口与录制期选框层共用
	void paintBorder(ID2D1DeviceContext* ctx, bool outside = false);
	void paintHandles(ID2D1DeviceContext* ctx);
	void paintLabel(ID2D1DeviceContext* ctx);
	void paintDim(ID2D1DeviceContext* ctx) const;
	void paintInfoIcons(ID2D1DeviceContext* ctx);
	Microsoft::WRL::ComPtr<IDWriteTextLayout> makeIconLayout(const wchar_t* code, float slot);
	void setHoverTarget(const D2D1_RECT_F& target, bool animate);
	void clearHoverAnim();
	void scheduleHoverRefine();
	void stopHoverRefine();
	static bool rectEq(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float eps = 0.5f);
	static bool rectContains(const D2D1_RECT_F& r, POINT pos);
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushInfoBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIcon;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSliderTrack;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSliderFill;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSliderThumb;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSelShadow;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutPos;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutSize;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> radiusValueLayout;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> lockLayout, unlockLayout;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> radiusLayout;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> shadowLayout;
	D2D1_RECT_F layoutRect{};
	D2D1_RECT_F lockBtnRect{};
	D2D1_RECT_F radiusBtnRect{};
	D2D1_RECT_F radiusSliderRect{};
	D2D1_RECT_F radiusValueRect{};
	D2D1_RECT_F shadowBtnRect{};
	D2D1_RECT_F shadowSwatchRect{};
	D2D1_RECT_F splitterRect{};
	float sizeTextX_{ 0.f };
	POINT pressPos{};
	Ling::WinBase* win{ nullptr };
	float paddingTop{ 2.f }, paddingMargin{3.f};
	MaskHit adjustHit{ MaskHit::None };
	D2D1_RECT_F adjustStartRect{};
	POINT adjustPressPos{};
	static constexpr float minSize{ 4.f };
	static constexpr float handleLogical{ 8.f };
	static constexpr float hitBandLogical{ 14.f };
	// 圆角滑条：轨 4 / 钮半径 7
	static constexpr float sliderTrackH{ ToolbarTheme::sliderTrackH };
	static constexpr float sliderThumbR{ ToolbarTheme::sliderThumbR };
	static constexpr float swatchSize{ ToolbarTheme::swatchSize };
	int labelIx_{ -1 }, labelIy_{ -1 }, labelIw_{ -1 }, labelIh_{ -1 };
	float lockRatio_{ 0.f }; // h/w；0 = 未锁定
	float selRadius_{ 0.f };
	float lastSelRadius_{ 20.f };
	int selShadowW_{ 0 };
	int lastSelShadowW_{ 10 };
	uint32_t selShadowColor_{ 0x00000040u }; // RRGGBBAA，默认黑 25%
	InfoHit infoDrag_{ InfoHit::None };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHandleFill;
	// 查找元素：逻辑目标 + 指数追赶（避免 Out 缓动反复重开导致发冲）
	D2D1_RECT_F hoverTarget_{};
	// 最近一次命中链（客户区坐标，[0]=最细节点，末尾=窗口）；高亮用 [0]，其余供后续层级细化
	std::vector<D2D1_RECT_F> hoverPath_;
	bool hoverAnimating_{ false };
	ULONGLONG lastHoverQueryMs_{ 0 };
	POINT lastHoverQueryPos_{};
	uint64_t hoverSeq_{ 0 };   // 最近一次投递到查询线程的序号（更旧的结果丢弃）
	// 查询结果是否仍是"整块级粗框"（无障碍树尚未建好 / 页面没加载完）→ 武装驻留细化重查
	bool hoverCoarse_{ false };
	// 是否处于"查找元素"悬停阶段（按下/进调整后就结束）。信息栏据此决定要不要让开高亮元素
	bool hoverPicking_{ false };
	int hoverRefineTries_{ 0 };      // 驻留细化预算：最多 5 次，鼠标移动重置
	bool hoverRefineTimerOn_{ false };
	static constexpr UINT hoverAnimTimerId{ 0x5201 };
	static constexpr UINT hoverRefineTimerId{ 0x5202 };
	static constexpr UINT kHoverRefineMs{ 80 };         // 驻留多久算"停稳了"
	static constexpr float hoverChaseRate{ 75.f }; // 约 40ms 贴合
	static constexpr UINT hoverTickMs{ 16 };
};
