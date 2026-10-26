#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>

class WinSettingAbout : public Ling::Node
{
public:
	WinSettingAbout(Ling::WinBase* parent);
	~WinSettingAbout();
	// 检查更新结果（网络请求完成后由 UI 线程回调进来）；seq 用于丢弃过期请求
	void onCheckFinished(int seq, bool ok, const std::wstring& remoteTag);
protected:
	// 配图是绝对定位的：每次排版后按页面宽度重新居中
	void layout() override;
private:
	void startCheck();
	// 底部状态行：文字 + 「· 刚刚检查」+ 圆点颜色（检查中=灰 / 成功=绿 / 失败=红）
	void setStatus(const std::wstring& text, const std::wstring& time, uint32_t dotColor);
private:
	Ling::Button* checkBtn_{ nullptr };
	Ling::Label*  checkLabel_{ nullptr };   // 按钮文字（检查中要换成「检查中…」）
	// 按钮图标的自转开关（true=开始转，false=停）；图标控件类型只在本 .cpp 里可见
	std::function<void(bool)> spinIcon_;
	Ling::Node*   statusDot_{ nullptr };
	Ling::Label*  statusText_{ nullptr };
	Ling::Label*  statusTime_{ nullptr };
	Ling::Node*   artHost_{ nullptr };   // 顶部配图（绝对定位，最后创建 → 压在最上层）
	int checkSeq_{ 0 };                 // 每次点「检查更新」+1；过期请求的结果直接丢弃
};
