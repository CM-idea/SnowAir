#pragma once
#include <include/Ling.h>
#include <functional>
#include <memory>
#include <string>

namespace SettingUi { class HoverTip; }
class WinSettingAbout : public Ling::Node
{
public:
	WinSettingAbout(Ling::WinBase* parent);
	~WinSettingAbout();
	// 检查更新结果（网络请求完成后由 UI 线程回调进来）；seq 用于丢弃过期请求
	void onCheckFinished(int seq, bool ok, const std::wstring& remoteTag);
	// 晾衣架检查在 8 秒超时/失败时不会发事件：主程序结果回来后兜底收尾
	void onRackCheckFallback(int seq);
	// 从菜单重新点「关于」时把绿色「立即更新」按钮恢复成「检查更新」
	void resetCheckButton();
protected:
	// 配图是绝对定位的：每次排版后按页面宽度重新居中
	void layout() override;
private:
	void startCheck();
	// 按钮分两种状态：无更新=刷新图标 +「检查更新」；有更新=下载图标 + 绿色「立即更新」
	void onCheckButton();
	void applyCheckButton();
	void updateCheckTextColor();
	// 底部状态行：文字 + 「· 刚刚检查」+ 圆点颜色（检查中=灰 / 成功=绿 / 失败=红）
	void setStatus(const std::wstring& text, const std::wstring& time, uint32_t dotColor);
	// 按「主程序 / 晾衣架」两组结果合成状态行与按钮状态
	void renderCheckResult();
	// 两组检查都结束时收尾（只执行一次）
	void finishCheck();
	// 彩蛋：悬停「Liuyiran」时逐个字符点亮（进度条），点满后才浮出二维码气泡
	void startEggProgress();
	void stopEggProgress();
	void resetEggChars();
	void showEggTipNow();
	// 光标是否还停在作者行上（进度点满时复检，防止鼠标已经离开还弹）
	bool cursorOverAuthor() const;
	// 每个字符点亮一步，8 个字符 ≈ 0.8 秒
	static constexpr UINT kEggProgressTimerId{ 0x5110 };
	static constexpr UINT kEggLitStepMs{ 100 };
private:
	Ling::Button* checkBtn_{ nullptr };
	Ling::Label*  checkLabel_{ nullptr };   // 按钮文字（检查中 / 立即更新）
	Ling::Label*  checkIconLabel_{ nullptr }; // 按钮图标（刷新 / 下载）
	// 按钮图标的自转开关（true=开始转，false=停）；图标控件类型只在本 .cpp 里可见
	std::function<void(bool)> spinIcon_;
	Ling::Node*   statusDot_{ nullptr };
	Ling::Label*  statusText_{ nullptr };
	Ling::Label*  statusTime_{ nullptr };
	Ling::Node*   artHost_{ nullptr };   // 顶部配图（绝对定位，最后创建 → 压在最上层）
	std::unique_ptr<SettingUi::HoverTip> eggTip_;   // 彩蛋气泡：复用设置页的功能提示气泡外壳
	Ling::Button* authorBtn_{ nullptr }; // 作者行（悬停它才出气泡）
	std::vector<Ling::Label*> authorChars_;   // Liuyiran 的每个字符（悬停时逐个点亮）
	int eggLit_{ 0 };                         // 已点亮的字符数
	int checkSeq_{ 0 };                 // 每次点「检查更新」+1；过期请求的结果直接丢弃
	bool checking_{ false };
	bool appChecked_{ false };
	bool appOk_{ false };
	bool appUpdate_{ false };
	bool rackChecked_{ false };
	bool rackUpdate_{ false };
	bool ocrUpdate_{ false };   // 已装 OCR 组件与当前内置发布号不一致
	// 按钮当前是不是「立即更新」；actionIsDownload=true 打开下载页，false 跳插件集成
	bool updateAction_{ false };
	bool actionIsDownload_{ false };
	bool checkHovered_{ false };
	winrt::event_token rackChangedTok_{};
	winrt::event_token eggTimerTok_{};   // 宿主窗口 onTimer 的订阅（析构时必须撤）
};
