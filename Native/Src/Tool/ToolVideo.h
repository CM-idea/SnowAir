#pragma once
#include <include/Ling.h>
#include "ToolbarTheme.h"

class WinCap;
class Tip;
// 录屏工具条（单一布局）：
// 拖拽 | 穿透 | 播放/暂停 | 时间 | 音频 | 麦 | 矩形…橡皮 | 撤销 | GIF | 保存 | 取消 | 完成
class ToolVideo : public Ling::WinBase
{
public:
	ToolVideo(WinCap* win);
	~ToolVideo();
	bool onSaveKey(bool toClipboard);
	// —— 「系统按键」用：与点这颗按钮**同一段实现**（按钮的 onClick 也调这里，别写第二份）——
	void cmdTogglePierce() { togglePierce(); }
	void cmdToggleRecord() { togglePlayPause(); }
	void cmdToggleGif() { onGifClick(); }
	void cmdToggleSystemAudio();
	void cmdToggleMic();
	// —— 「系统按键」用：绘图工具命令键。录屏阶段的绘图工具在本条工具条上（非截图条），
	//    故工具键与工具循环必须走这里。——
	void cmdAnnotTool(const std::wstring& id);   // 选中某件绘图工具（等同点击该按钮）
	void cycleAnnotTool();                       // 工具循环（Tab）
	void updateUndoEnabled();
	void cancelAnnotSelect();
	void syncPierce(bool on);
	float getBtnCenterX() const;
	const std::wstring& curAnnotId() const { return curAnnotId_; }
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void buildBar();
	void makeSlot(const std::wstring& id);          // 按槽位 id 建一个控件（分隔符走 makeSpliter）
	std::vector<std::wstring> resolveSlots() const; // 运行时槽位顺序（自定义布局优先）
	void onTimerCB(UINT id);
	void togglePlayPause();
	void togglePierce();
	void startRecord();
	void setPaused(bool on);
	void saveFile();
	void finishRecord(bool toClipboard);
	void updateTimerText();
	void applyToggleStyle(Ling::Button* btn, bool selected);
	void applyAnnotSelect(const std::wstring& id);
	void onAnnotClick(const std::wstring& id);
	void onGifClick();
	void updateAudioButtons();
	void paintChrome();
	Ling::Button* makeIconBtn(const std::wstring& code, const std::wstring& id = L"");
	Ling::Node* makeSpliter();
	float iconSideInset() const;
	float dragMarginRight() const;
	float contentPadRight() const;
	float contentWidth() const;
	void refreshSize();
	void onDragDown(POINT pos, bool isRight);
	void onDragMove(POINT pos);
	void onDragUp(POINT pos, bool isRight);
	// —— 贴边隐藏（早期 QQ 那种）——
	// 只有"手动拖动后松手贴在屏幕上/下边缘"才触发；自动布局本来就在边缘不算。
	// 隐藏后只留一条边在屏幕上，鼠标移到边上滑出，移开后自动缩回。
	void peekCollapse();
	void peekReveal();
	void schedulePeekCollapse();
	enum class PeekEdge { None, Top, Bottom };
	PeekEdge peekEdge_{ PeekEdge::None };
	bool peekHidden_{ false };
	bool dragMoved_{ false };
	static constexpr UINT peekTimerId{ 101 };
	static constexpr int peekStripPx{ 4 };          // 缩进后露出的条高（逻辑像素）
	static constexpr UINT peekHideDelayMs{ 150 };   // 鼠标移出后多久缩回
private:
	WinCap* win;
	bool dpiChanged{ false };
	bool draggingBar{ false };
	POINT dragMouseScreen{};
	int dragWinX{ 0 }, dragWinY{ 0 };

	std::unique_ptr<Tip> tip;
	Ling::Node* contentNode{ nullptr };
	Ling::Canvas* chromeCanvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Ling::Button* dragHandle{ nullptr };
	Ling::Button* btnPierce{ nullptr };
	Ling::Button* btnPlay{ nullptr };
	Ling::Button* btnAudio{ nullptr };
	Ling::Button* btnMic{ nullptr };
	Ling::Button* btnGif{ nullptr };
	Ling::Button* btnUndo{ nullptr };
	Ling::Label* timerLabel{ nullptr };
	std::vector<Ling::Button*> annotBtns;
	std::wstring curAnnotId_;
	int selectIndex{ 0 }; // 0=mp4 1=gif
	int totalSeconds{ 0 };
	// 开录 1 秒后把工作集里的空闲页换出去一次（采集/编码缓冲已分配、开场截图已释放）
	bool trimmedAfterStart_{ false };
	bool selectSpeaker{ true }, selectMic{ false };
	bool isRecording{ false };
	bool isPaused{ false };
	bool pierceOn{ false };
	static constexpr float btnSize{ 42.f };
	static constexpr float toolbarRadius{ ToolbarTheme::borderRadius };
	static constexpr float hoverRadius{ ToolbarTheme::hoverRadius };
	static constexpr float hoverInset{ 4.f };
	static constexpr float spliterH{ 24.f };
	static constexpr float timerW{ 56.f };
	static constexpr float spliterW{ 1.f };
	static constexpr UINT tickTimerId{ 100 };
};
