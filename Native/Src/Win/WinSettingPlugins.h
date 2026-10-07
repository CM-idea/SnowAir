#pragma once
#include <include/Ling.h>
#include "SettingWidgets.h"
#include <memory>
#include <string>
#include <vector>

/**
 * 插件集成 —— 内容布局：
 *   - 页面标题 + 短描述（sectionHeader）
 *   - 「已安装组件：N」统计行
 *   - 每个插件一张卡片：标题行、说明、右上角方形图标下载按钮、下方绿色「已安装」徽章行
 *   - 右上角下载按钮非常驻：鼠标停在卡片上才显示
 *   - 底部「打开组件目录」按钮
 */
class WinSettingPlugins : public Ling::Node
{
public:
	WinSettingPlugins(Ling::WinBase* parent);
	~WinSettingPlugins();
private:
	// 插件卡片各部件
	struct PackUi {
		Ling::Node* card{ nullptr };
		Ling::Node* titleRow{ nullptr };   // 标题 + 内嵌控件
		Ling::Label* desc{ nullptr };
		Ling::Label* progress{ nullptr };  // 下载进度（默认隐藏）
		Ling::Node* badgeRow{ nullptr };   // 徽章行（随徽章显隐整体收起）
		Ling::Button* dlBtn{ nullptr };    // 右上角方形图标按钮（悬停显示 / 下载中显示进度）
		Ling::Button* delBtn{ nullptr };   // 已安装时悬停在卡片上显示（删除）
	};

	// 卡片悬停 → 显示右上角下载或删除按钮
	struct CardSlot {
		Ling::Node* card{ nullptr };
		Ling::Button* dlBtn{ nullptr };
		Ling::Button* delBtn{ nullptr };
		bool busy{ false };      // 正在下载
		bool offer{ false };     // 是否提供下载（未安装且不在下载中）
		bool installed{ false }; // 已安装（悬停显示删除）
	};
	enum CardIndex { CardOcr = 0, CardTr = 1, CardGh = 2 };

	void build();
	void refreshStatus();
	// 用一组标签重填徽章行（先清空children）；空列表 = 隐藏整行
	void setBadges(Ling::Node* row, const std::vector<std::wstring>& labels);
	PackUi makePack(Ling::Node* parent, const std::wstring& title, const std::wstring& desc);
	void updateHover(POINT pos);
	void startOcrDownload();
	void startTrDownload();
	void deleteCard(int idx);
	// 下载中：按钮显示「NN%」数字进度（图标字体切回默认 UI 字体）
	void setCardProgress(CardSlot& c, float p);
	// 下载结束：复位按钮图标 / 停掉省略号动画 / 清进度行 / 刷新状态
	void finishDownload(int idx, const std::wstring& err);
	// 「正在下载」省略号逐字跳动（. / .. / ... / ...），避免长时间不动像卡死
	void startDots(Ling::Label* lab, const std::wstring& prefix, const std::wstring& suffix = L"");
	void stopDots();
	void applyDots();

private:
	Ling::Label* summary{ nullptr };
	Ling::Node* ocrBadgeRow{ nullptr };
	Ling::Label* ocrProgress{ nullptr };
	Ling::Node* trBadgeRow{ nullptr };
	Ling::Label* trProgress{ nullptr };
	int trPackIdx{ 0 };            // 离线翻译当前选中的语言包下标
	std::vector<CardSlot> cards;   // [0]=OCR [1]=翻译 [2]=果核看图
	winrt::event_token hoverTok{};
	winrt::event_token timerTok{};
	// 省略号动画状态（同一时刻只服务一个进度行）
	Ling::Label* dotsLabel{ nullptr };
	std::wstring dotsPrefix, dotsSuffix;
	int dotsPhase{ 0 };
	bool dotsRunning{ false };
};
