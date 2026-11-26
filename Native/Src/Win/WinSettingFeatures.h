#pragma once
#include <include/Ling.h>
#include <array>
#include <vector>
#include "../Shape/LaserStroke.h"
#include "SettingWidgets.h"

class WinSettingFeatures : public Ling::Node
{
public:
	WinSettingFeatures(Ling::WinBase* parent);
	~WinSettingFeatures();
private:
	void build();
	void showTab(int index);
	void buildBasic(Ling::Node* p);
	void buildRecord(Ling::Node* p);
	void buildOutput(Ling::Node* p);
	void buildOcr(Ling::Node* p);
	void buildTranslate(Ling::Node* p);
	void buildRecognize(Ling::Node* p);   // 识别模型：文字识别 + 翻译模型两组
	void buildOther(Ling::Node* p);       // 其他功能：功能提示 / 快捷键提示 / 悬停预览 / 记忆工具样式
	void rebuildTab(int index);
	// 延迟到调度队列再重建：调用点若位于卡片内按钮/下拉自身的回调中，同步 removeAllChildren
	// 会在事件派发中途销毁正在执行回调的控件（同 WinSettingHistory 筛选下拉的处理）。
	void deferRebuildTab(int index);
	// 「恢复默认」的置灰：当前子页各项都还在出厂值 → 灰字且点击不做事（与外观页同款口径）
	void syncRestoreState();

	// —— 鼠标轨迹 / 单击效果预览 ——
	// 预览层是一块盖在设置页上的透明 Canvas：高亮圆、跟随线条、单击波纹/光标放大
	// 都在这里按录制时的同一套几何绘制。悬停轨迹行看轨迹，单击单击效果行看点击反馈。
	void initPreviewOverlay();
	void updatePreviewOnMouseMove(const POINT& pos);
	void updatePreviewOnMouseDown(const POINT& pos);
	// 轨迹/波纹的取色：只有一个共享色号，两行各放一个色块，改一处两处同步
	void pickMouseFxColor();
	void syncColorSwatches();
	void updateSwatchVisibility();
	// 「光标放大」预览：动画期间临时藏掉系统光标，由预览层画放大的真实光标，
	// 否则会变成"真实光标上面/下面又叠了一个光标"。
	void restorePreviewCursor();
	void updatePreviewFrame();
	void paintPreview();
	void startPreviewTimer();
	void stopPreviewTimerIfIdle();
	bool ensurePreviewCursorBitmap();
	ID2D1SolidColorBrush* ensurePreviewBrush();
private:
	std::array<Ling::Node*, 5> tabs{};
	SettingUi::SegmentedPillsRef segPills;   // 分段胶囊 Tab（滑动指示）
	Ling::Button* restoreBtn{ nullptr };
	int tabIndex{ 0 };
	// 预览触发区：整张卡片都生效（选了效果后不必再对准某块空白）
	Ling::Node* trailPreviewArea_{ nullptr };
	Ling::Node* clickPreviewArea_{ nullptr };
	Ling::Button* trailSwatch_{ nullptr };   // 高亮/线条用色：选中时显示
	Ling::Button* clickSwatch_{ nullptr };   // 单色波纹用色：选中时显示；光标放大不显示
	Ling::Canvas* previewCanvas_{ nullptr };   // 挂在 win->body 上，窗口坐标
	struct PreviewClickFx { POINT pt; ULONGLONG t; };
	LaserStroke::Trail previewTrail_;              // 物理像素（窗口客户区）
	std::vector<PreviewClickFx> previewClicks_;
	POINT previewHoverPt_{ -1, -1 };
	bool previewHoverOn_{ false };
	bool previewTimerOn_{ false };
	bool previewCursorHidden_{ false };
	static constexpr UINT kPreviewTimerId{ 0x5A2A };
	static constexpr UINT kPreviewTickMs{ 16 };
	// 光标放大预览：按下时才抓一次当前系统光标（和录屏里用的是同一种位图）
	bool previewCursorTried_{ false };
	int previewCursorW_{ 0 }, previewCursorH_{ 0 };
	POINT previewCursorHotspot_{ 0, 0 };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> previewCursorBmp_;
	UINT32 previewBrushColor_{ 0 };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> previewBrush_;
	winrt::event_token mouseMoveTok{};
	winrt::event_token mouseDownTok_{};
	winrt::event_token timerTok_{};
};
