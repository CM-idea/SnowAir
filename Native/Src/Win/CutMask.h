#pragma once
#include <include/Ling.h>
#include <vector>
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
	// 鼠标悬停时吸附到光标下的窗口矩形，选区真的变了才返回 true
	bool highlight(POINT pos);
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
	// 平移选区并夹到窗口内；真的移动了返回 true
	bool translateBy(float dx, float dy);
	void paint(ID2D1DeviceContext* ctx);
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
	// 导出时套圆角裁切 + 外阴影（对齐 QT exportCrop）；无效果则原样返回 true
	bool applyExportEffects(std::vector<BYTE>& pixels, int& cw, int& ch) const;
public:
	D2D1_RECT_F maskRect{};
	float strokeWidth{ 2.f };
	// 选区定死之后（录屏 / 滚动截图）标签就没用了，而它可能压在选区内部被录进去
	bool hideLabel{ false };
	// 查找元素悬停：只画描边；拖框/已框选才压暗+控点（对齐 Tauri2）
	bool showDim{ true };
	bool showHandles{ true };
	bool showBorder{ true }; // 长图编辑态可关，只留内容区
	// 识别结果白底：画在描边/控点/信息栏之下
	bool drawQrcodeFill{ false };
	// 查找元素高亮补间（指数追赶 + 查询节流，去掉重开顿挫）
	void onHoverAnimTick(UINT timerId);
	void snapHoverTarget();
	// 查找元素的两个辅助定时器（同一个 onTimer 通道，按 id 分流）：
	//  · 驻留细化重查：光标停稳后对同一点用完整预算再钻一次，直到细化或预算耗尽
	//  · 查询捕获看门狗：请求被"只做最新一次"合并丢弃时没有结果回来，兜底放开临时捕获
	void onHoverDwellTimer(UINT timerId);
	// 按下/退出时立刻撤销查询状态（临时点穿、临时捕获、驻留重查、在途结果作废）
	void cancelHoverQuery();
private:
	void makeLayout();
	void applyAspectLock(D2D1_RECT_F& r) const;
	void toggleAspectLock();
	void toggleSelRadius();
	void toggleSelShadow();
	void setRadiusFromSliderX(float x);
	void pickShadowColor();
	void rebuildRadiusValueLayout();
	void paintSelShadow(ID2D1DeviceContext* ctx) const;
	void paintDim(ID2D1DeviceContext* ctx) const;
	void paintInfoIcons(ID2D1DeviceContext* ctx);
	Microsoft::WRL::ComPtr<IDWriteTextLayout> makeIconLayout(const wchar_t* code, float slot);
	void setHoverTarget(const D2D1_RECT_F& target, bool animate);
	void clearHoverAnim();
	// 查询期间给遮罩上/撤"临时鼠标捕获"：命中测试会给遮罩临时加 WS_EX_TRANSPARENT，
	// 那一瞬间鼠标按下会漏给下层窗口。捕获是线程态，只能在 UI 线程做。
	void beginQueryGuard();
	void endQueryGuard();
	void scheduleHoverRefine();
	void stopHoverRefine();
	static bool rectEq(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float eps = 0.5f);
	static bool rectAlmost(const D2D1_RECT_F& a, const D2D1_RECT_F& b);
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
	// 统一：QT CaptureInfoBar 圆角滑条（轨 4 / 钮半径 7）
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
	bool hoverAnimating_{ false };
	ULONGLONG lastHoverQueryMs_{ 0 };
	POINT lastHoverQueryPos_{};
	uint64_t hoverSeq_{ 0 };   // 最近一次投递到查询线程的序号（更旧的结果丢弃）
	// 查询结果是否仍是"整块级粗框"（无障碍树尚未建好 / 页面没加载完）→ 武装驻留细化重查
	bool hoverCoarse_{ false };
	int hoverRefineTries_{ 0 };      // 驻留细化预算：最多 5 次，鼠标移动重置
	bool hoverRefineTimerOn_{ false };
	bool queryGuard_{ false };       // 查询期间临时拿到的鼠标捕获（按下/结果回来/看门狗到期撤）
	static constexpr UINT hoverAnimTimerId{ 0x5201 };
	static constexpr UINT hoverRefineTimerId{ 0x5202 };
	static constexpr UINT hoverGuardTimerId{ 0x5203 };
	static constexpr UINT kHoverRefineMs{ 120 };        // 驻留多久算"停稳了"
	static constexpr UINT kQueryGuardWatchdogMs{ 150 }; // 捕获兜底放手
	static constexpr float hoverChaseRate{ 75.f }; // 约 40ms 贴合
	static constexpr UINT hoverTickMs{ 16 };
};
