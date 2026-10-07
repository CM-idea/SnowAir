#pragma once
#include <include/Ling.h>
#include <functional>
#include "SettingWidgets.h"

struct DictEntry;

/**
 * 快捷翻译 (Section 1)：
 *   输入/粘贴文本 → 选源语言与目标语言 → 立即翻译；带翻译历史侧栏（本地持久化）。
 *   翻译引擎只使用非 AI 的在线免费接口（微软/谷歌/有道/百度）与离线 Bergamot。
 */
class WinSettingQuickTranslate : public Ling::Node
{
public:
	WinSettingQuickTranslate(Ling::WinBase* parent);
	~WinSettingQuickTranslate();
private:
	// —— 语言选项弹层 ——
	void showOptionsBox(Ling::Button* anchor, const std::vector<std::wstring>& options,
		int current, float boxW, std::function<void(int)> onPick);
	void hideOptionsBox();
	// —— 翻译主面板 ——
	void buildTranslateTab(Ling::Node* p);
	void refreshTopBar();                 // 同步源/目标语言按钮文本
	void ensureTargetWhenSrcZh();         // 源为中文/自动识别时把目标纠到英语
	void refreshProviderBtn();            // 同步翻译服务下拉按钮文本
	void showLangBox(Ling::Button* btn, bool target);
	void showProviderBox(Ling::Button* btn);
	void doTranslate(const std::wstring& text);
	void appendBubble(const std::wstring& role, const std::wstring& text);
	void updatePendingBubble(const std::wstring& text);
	// —— 字典模式（单击译文英文单词，于该条下方展开释义）——
	bool dictModeOn() const;
	void buildResultContent(Ling::Node* bubble, Ling::Node* block, const std::wstring& text);   // 词元流 + 隐藏释义卡
	void onDictWordClicked(Ling::Node* block, Ling::Button* btn, const std::wstring& word, int index);
	Ling::Node* makeDictCard(Ling::Node* block);   // 在该条消息下方新建释义卡
	void setWordActive(Ling::Button* btn, bool active);   // 选中的词：加粗 + 绿色
	void fillDictCard(Ling::Node* card, const DictEntry& e);
	void resetDictState();                // 消息列表重建时清掉选中态
	void scrollToBottom();
	void updateWelcome();                 // 无记录时显示居中提示语
	void updateSendBtn();                 // 发送按钮：按有无输入切换配色（灰底黑图标 / 绿底白图标）
	// —— 历史侧栏 ——
	void toggleHistory();
	void animateHistory();                // onTimer 单帧步进侧栏宽度
	void refreshHistoryList();
	void loadSessions();
	void loadSession(const std::wstring& id);
	void saveSessions();
	void clearSessions();
	void startNewSession();
	// —— 工具 ——
	static std::wstring trim(std::wstring s);
	std::wstring wrapText(const std::wstring& text, float maxW);
	struct Session;
	Session* currentSession();
	static std::wstring newId();
private:
	// 用独立 WS_POPUP 弹层窗口承载：不挂宿主 body，故不会被窗口客户区裁剪。
	std::unique_ptr<SettingUi::Popup> optionsPopup;
	// —— 翻译面板 ——
	Ling::ScrollerBox* msgScroll{ nullptr };
	Ling::Node* msgList{ nullptr };
	Ling::TextBox* input{ nullptr };
	Ling::Button* sendBtn{ nullptr };
	Ling::Node* welcomeWrap{ nullptr };    // 空态居中提示语
	Ling::Button* srcLangBtn{ nullptr };
	Ling::Label* srcLangVal{ nullptr };    // 源语言下拉的选中值
	Ling::Button* tgtLangBtn{ nullptr };
	Ling::Label* tgtLangVal{ nullptr };    // 目标语言下拉的选中值
	Ling::Button* swapBtn{ nullptr };
	bool tgtPinnedZh{ false };   // 用户在「源为中文/自动」时手动把目标改回中文 → 不再自动纠偏
	Ling::Button* providerBtn{ nullptr };  // 翻译服务（微软/谷歌/有道/百度/离线）
	Ling::Label* providerVal{ nullptr };   // 翻译服务下拉的选中值
	Ling::Label* counterLab{ nullptr };    // 输入字数 "n / 5000"
	// —— 历史侧栏 ——
	Ling::Button* histToggleBtn{ nullptr };  // 侧栏切换按钮（展开/折叠图标随状态切换）
	Ling::Node* historyShell{ nullptr };
	Ling::Node* histInner{ nullptr };
	Ling::ScrollerBox* histScroll{ nullptr };
	Ling::Node* histList{ nullptr };
	winrt::event_token timerTok;          // onTimer 订阅（仅注册一次）
	bool histAnimRunning{ false };
	float histAnimW{ 0.f };
	bool histOpen{ false };
	// —— 会话与记录 ——
	struct Msg {
		std::wstring role;      // "user" 原文 / "assistant" 译文
		std::wstring content;
	};
	struct Session {
		std::wstring id;
		std::wstring title;
		long long ts{ 0 };
		std::vector<Msg> msgs;
	};
	std::vector<Session> sessions;
	std::wstring curId;
	bool busy{ false };
	Ling::Node* pendingBubble{ nullptr };   // 正在翻译的译文气泡
	Ling::Label* pendingText{ nullptr };
	// —— 字典模式 ——
	Ling::Node* dictCard{ nullptr };        // 当前显示的释义卡（同一时刻只显示一张）
	Ling::Button* dictActiveBtn{ nullptr }; // 当前选中的词按钮
	std::wstring dictActiveWord;            // 当前展开的词
	int dictActiveIndex{ -1 };              // 词在译文里的序号（再点同词 = 收起）
};
