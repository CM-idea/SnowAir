#include "pch.h"
#include "../Lang.h"
#include "WinSetting.h"
#include "WinSettingAbout.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"

WinSettingAbout::WinSettingAbout(Ling::WinBase* parent) : Ling::Node(parent)
{
	SettingUi::sectionHeader(this, Lang::get(L"setting.about"),
		Lang::get(L"setting.aboutDesc"));

	// 应用信息卡片（名称 + 版本）
	auto info = SettingUi::card(this);
	auto name = info->makeChild<Ling::Label>();
	name->setText(Lang::get(L"about.appName"));
	name->setFontSize(14.f);
	name->setColor(SettingTheme::textPrimary);
	auto ver = Ling::Util::getVerNum();
	auto verStr = std::format(L"{}.{}.{}", ver[0], ver[1], ver[2]);
	auto fmt = Lang::get(L"about.versionFmt");
	auto pos = fmt.find(L"%1");
	if (pos != std::wstring::npos) fmt.replace(pos, 2, verStr);
	auto vLabel = info->makeChild<Ling::Label>();
	vLabel->setText(fmt);
	vLabel->setFontSize(13.f);
	vLabel->setColor(SettingTheme::textSecondary);
	vLabel->setMarginTop(2.f);

	// 按钮行（检查更新 / 反馈问题）
	auto row = makeChild<Ling::Node>();
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setAlignItems(Ling::Align::Center);
	row->setMarginTop(SettingTheme::blockGap);

	auto* checkBtn = SettingUi::btnOutline(row, Lang::get(L"about.checkUpdate"), []() {
		ShellExecute(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/releases",
			nullptr, nullptr, SW_SHOWNORMAL);
	});
	checkBtn->setMarginRight(SettingTheme::sp2);

	SettingUi::btnOutline(row, Lang::get(L"about.feedback"), []() {
		ShellExecute(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/issues",
			nullptr, nullptr, SW_SHOWNORMAL);
	});

	// 内容区只有外层 content 提供底部留白：清掉末尾元素的 marginBottom，使底部与左右一致。
	SettingUi::trimTrailingGap(this);
}

WinSettingAbout::~WinSettingAbout()
{
}
