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
