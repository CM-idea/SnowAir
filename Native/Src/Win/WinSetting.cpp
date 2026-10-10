#include "pch.h"
#include <dwmapi.h>
#include "../App.h"
#include "../Lang.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"

// 子页面头文件（集中 include，避免 WinSetting.h 膨胀）
#include "WinSettingHotkeys.h"
#include "WinSettingQuickTranslate.h"
#include "WinSettingHistory.h"
#include "WinSettingPlugins.h"
#include "WinSettingAppearance.h"
#include "WinSettingFeatures.h"
#include "WinSettingAbout.h"
#include "../Tool/IconCodes.h"

// 说明：
//   shadcn/ui 设计 Token 已统一迁移到 SettingTheme.h（namespace SettingTheme）。
//   构件库统一在 SettingWidgets.h/cpp（namespace SettingUi）。
//   本文件不再保留任何本地 Shadcn namespace，单一事实源 = SettingTheme。

std::unique_ptr<WinSetting> winSetting;

WinSetting::WinSetting() : Ling::WinBase()
{
	// ★ 主题必须先套用：SettingTheme 的语义色是运行时变量，各子页在构建时就把它读进控件了，
	//   所以要在建窗（onCreated → loadPage）之前按设置重写一遍调色板。
	SettingTheme::apply(Setting::get()->getAppTheme());
	onDestroy.add([]() {
		Ling::App::get()->dq.TryEnqueue([]() { winSetting.reset(); });
		});
	setTitle(Lang::get(L"setting.title"));
	setSize(880, 600);
	setCenter();
	// ★ 使用 WS_OVERLAPPEDWINDOW：启用系统原生标题栏
	//   - 自带可拖动标题栏 + 最小/最大化/关闭 3 个系统按钮
	//   - 关闭按钮悬停自动使用 Windows 默认红色背景，无需自绘
	createNativeWindow(
		NULL,
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN
	);
}

WinSetting::~WinSetting()
{
	// 注销「?」气泡：SettingUi 里存的是裸指针，窗口销毁后必须清掉，避免悬垂引用
	SettingUi::attachHelpTip(nullptr);
}

void WinSetting::init()
{
	init(0);
}

void WinSetting::init(int pageIndex)
{
	if (!winSetting) winSetting.reset(new WinSetting());
	else SetForegroundWindow(winSetting->hwnd);
	winSetting->showPage(pageIndex);
}

void WinSetting::dispose()
{
	// 先真正 DestroyWindow 再释放对象：~WinBase 不会销毁 HWND，而退出时消息循环已经结束，
	// 若只 reset()，窗口会留下一块"内容已拆、窗口还在"的白板，一直挂到进程退出（期间显示未响应）。
	// 这里先关窗，白板就根本不会出现，后面的子页析构都在屏幕外完成。
	if (winSetting && winSetting->hwnd) winSetting->close();
	winSetting.reset();
}

// 菜单项附属数据：图标 + 文字标签，用于在 hover/选中时同步更新颜色
struct MenuItemData {
	Ling::Label* icon{ nullptr };
	Ling::Label* lab{ nullptr };
	winrt::event_token enterTok{};
	winrt::event_token leaveTok{};
};
static std::vector<MenuItemData> gMenuData;

// 菜单视觉：根据选中/未选中应用风格
//   选中 = sidebarSelected 底 + sidebarSelectedFg 字（浅色=浅灰底深字；深色=深灰底浅字）
//   未选中 = 透明底 + mutedForeground 字，悬停→sidebarAccent + foreground
static void ApplyMenuItemStyle(Ling::Button* item, bool selected, MenuItemData& data)
{
	using namespace SettingTheme;
	item->setBorderRadius(radiusMd);
	item->setHeight(menuItemH);
	item->setFlexDirection(Ling::FlexDirection::Row);
	item->setAlignItems(Ling::Align::Center);
	item->setJustifyContent(Ling::Justify::Start);
	item->setMarginBottom(menuGap);
	item->setPadding(sp2, sp3, sp2, sp3);

	// 移除旧的 hover 订阅，避免重复累积
	item->onEnter.remove(data.enterTok);
	item->onLeave.remove(data.leaveTok);

	auto setFg = [item, &data](uint32_t c) {
		item->setColor(c);
		if (data.lab)  data.lab->setColor(c);
		if (data.icon) data.icon->setColor(c);
	};

	if (selected) {
		item->setBg(sideSelected);
		item->setHoverBg(sideSelected);
		setFg(sidebarSelectedFg);
	}
	else {
		item->setBg(0);                           // 透明（露出窗口最外层的底）
		item->setHoverBg(sideHover);
		setFg(textSecondary);
		data.enterTok = item->onEnter.add([&data, item](Ling::Button*) {
			item->setColor(SettingTheme::textPrimary);
			if (data.lab)  data.lab->setColor(SettingTheme::textPrimary);
			if (data.icon) data.icon->setColor(SettingTheme::textPrimary);
		});
		data.leaveTok = item->onLeave.add([&data, item](Ling::Button*) {
			item->setColor(SettingTheme::textSecondary);
			if (data.lab)  data.lab->setColor(SettingTheme::textSecondary);
			if (data.icon) data.icon->setColor(SettingTheme::textSecondary);
		});
	}
}

void WinSetting::onCreated()
{
	enableShadow();

	// 深色主题：连系统标题栏一起切成深色（Win10 1809+，属性 20 = DWMWA_USE_IMMERSIVE_DARK_MODE）
	if (SettingTheme::isDark() && hwnd) {
		BOOL dark = TRUE;
		DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
	}

	// 「?」说明气泡（工具栏停悬气泡那套外壳）：需要宿主 hwnd，只能在窗口创建后构造。
	// 必须在 loadPage 之前注册 —— 子页构建时 optionRow/groupLabel 会据此生成问号。
	helpTip_ = std::make_unique<SettingUi::HoverTip>(this);
	SettingUi::attachHelpTip(helpTip_.get());

	// 整体背景：画布浅灰（右侧白色内容卡片悬浮其上，参考 TRAE Design）
	body->setBg(SettingTheme::canvas);
	body->setFlexDirection(Ling::FlexDirection::Row);

	// ===================== 左侧导航栏（透明悬浮于画布上，菜单项直接落在浅灰底） =====================
	auto sideNav = body->makeChild<Ling::Node>();
	sideNav->setWidth(SettingTheme::sideWidth);
	sideNav->setHeightPercent(100.f);
	sideNav->setFlexDirection(Ling::FlexDirection::Column);
	sideNav->setJustifyContent(Ling::Justify::SpaceBetween);
	sideNav->setBg(0);   // 透明 → 露出画布浅灰底
	// (left, top, right, bottom)：左右用 sidePadX，上下用 sidePadY
	// 菜单项右边缘 = sideWidth - sidePadX，其到白色卡片左边缘的间隙 = sidePadX，
	// 正好等于菜单项左边缘到窗口的间距（sidePadX），实现左右对称留白。
	sideNav->setPadding(SettingTheme::sidePadX, SettingTheme::sidePadY,
		SettingTheme::sidePadX, SettingTheme::sidePadY);

	// ① 顶部：LOGO（深色方块 + 雪花标） + SnowAir + 设置中心
	{
		auto brandTop = sideNav->makeChild<Ling::Node>();
		brandTop->setFlexDirection(Ling::FlexDirection::Row);
		brandTop->setAlignItems(Ling::Align::Center);
		brandTop->setMarginBottom(SettingTheme::sp6);

		// LOGO：直接用图标字体里的 LOGO 字形（放大 + 换成应用图标那支绿），不套深色方块
		auto logoBox = brandTop->makeChild<Ling::Node>();
		logoBox->setSize(44.f, 44.f);
		logoBox->setMarginRight(SettingTheme::sp2);   // LOGO 与右侧文字之间的间距（收窄）

		auto logoLabel = logoBox->makeChild<Ling::Label>();
		logoLabel->setText(Icon::Logo);
		logoLabel->setFontFamily(Icon::Family);
		logoLabel->setFlexGrow(1.0);
		logoLabel->setHeightPercent(100.f);
		logoLabel->setAlignItems(Ling::Align::Center);
		logoLabel->setJustifyContent(Ling::Justify::Center);
		logoLabel->setColor(0x34C759FF);             // #34C759
		logoLabel->setFontSize(40.f);

		// 软件名两行
		auto brandText = brandTop->makeChild<Ling::Node>();
		brandText->setFlexDirection(Ling::FlexDirection::Column);
		brandText->setFlexGrow(1.0);

		auto appLabel = brandText->makeChild<Ling::Label>();
		appLabel->setText(L"SnowAir");
		appLabel->setFontSize(SettingTheme::fontXl);
		appLabel->setColor(SettingTheme::textPrimary);

		auto subLabel = brandText->makeChild<Ling::Label>();
		subLabel->setText(L"设置中心");
		subLabel->setFontSize(SettingTheme::fontSm);
		subLabel->setColor(SettingTheme::textSecondary);
	}

	// ② 中部：7 菜单
	initMenuItems(sideNav);

	// ③ 底部：版本号（zinc-400 弱文字）
	auto versionLabel = sideNav->makeChild<Ling::Label>();
	versionLabel->setText(L"v0.1.0 beta");
	versionLabel->setFontSize(SettingTheme::fontXs);
	versionLabel->setColor(SettingTheme::textTertiary);

	// ===================== 右侧内容区：浅灰画布 + 白色固定尺寸圆角卡片 =====================
	// 白色卡片固定铺满右侧可用高度（不随内容多少伸缩 —— 解决"内容少时卡片拉短/内容多时
	// 卡片拉长"的问题）；滚动发生在卡片内部，滚动条绘制在卡片右缘内部，不再落到窗口最右
	// 边缘的浅灰画布上。
	auto rightArea = body->makeChild<Ling::Node>();
	rightArea->setFlexGrow(1.0);
	// 右内容区是 body(Row) 的 flex 项：Yoga 默认 flexShrink=0 且 min-width:auto，
	// 会被内容中最宽单行文字的 min-content 撑开，导致卡片/内容区整体溢出窗口右缘。
	// 显式允许水平收缩 + min-width:0，让它收缩到窗口可用宽度（配合下方层层 min-width:0 与 Text 折行）。
	rightArea->setFlexShrink(1.f);
	YGNodeStyleSetMinWidth(rightArea->node, 0.f);
	rightArea->setHeightPercent(100.f);
	rightArea->setPadding(0.f, SettingTheme::cardOutset,
		SettingTheme::cardOutset, SettingTheme::cardOutset);
	rightArea->setBg(0);   // 透出画布浅灰

	// 白色圆角卡片本体：固定铺满 rightArea（flexGrow 撑满高度），卡片高度恒定。
	auto card = rightArea->makeChild<Ling::Node>();
	card->setWidthPercent(100.f);
	card->setHeightPercent(100.f);
	card->setFlexGrow(1.0);
	card->setFlexShrink(1.0);
	// 关键：解除 flex 项目默认的 min-width:auto。
	//   Yoga 计算 min-content 时用"无约束"测量（单行整宽），若不禁用，长文本会把
	//   卡片下限锁在"单行整宽"，即使底层 Text 已支持折行也无法把卡片收缩回可用宽度。
	//   min-width:0 允许卡片水平收缩到可用宽度，配合 Text 折行才能真正让内容不被撑宽。
	YGNodeStyleSetMinWidth(card->node, 0.f);
	card->setFlexDirection(Ling::FlexDirection::Column);
	card->setBg(SettingTheme::background);
	card->setBorder(1.f, SettingTheme::contentBorder);   // 内容区描边 EEEEEE
	card->setBorderRadius(SettingTheme::contentCardRadius);
	cardNode = card;   // 视口容器：QuickFeatures 页直接挂这（不包外层滚动）

	// 卡片内部滚动容器：滚动条绘制在卡片右缘内部
	contentScroll = card->makeChild<Ling::ScrollerBox>();
	contentScroll->setFlexGrow(1.0);
	contentScroll->setHeightPercent(100.f);
	contentScroll->setScrollBarVisible(false);   // 设置页不显示滚动条（滚轮仍可滚）
	YGNodeStyleSetMinWidth(contentScroll->node, 0.f);
	contentScroll->content->setFlexDirection(Ling::FlexDirection::Column);
	contentScroll->content->setPadding(SettingTheme::contentPadX, SettingTheme::contentPadY,
		SettingTheme::contentPadX, SettingTheme::contentPadY);
	// 注意：不允许把 content 高度撑满视口（不要 setMinHeightPercent(100%)）。
	// 普通内容页高度必须纯由内容驱动 —— 视口相关高度在「拉高→缩回」后容易残留：
	// content->h 卡在拉高时的峰值，导致底部多余空白 + 假滚动条。
	// 本容器（contentScroll）只服务普通内容页；Quick 页已直接挂 cardNode，不信赖此容器。

	// 子页挂接到此（内容 padding 由 contentScroll->content 提供）
	content = contentScroll->makeChild<Ling::Node>();
	content->setWidthPercent(100.f);
	content->setFlexGrow(1.f);   // 撑满 contentScroll->content（扣除 padding 后的内容区），子页才能贴底
	// 关键：允许 content 被父容器（contentScroll->content）压缩。
	//   Yoga 默认 flexShrink=0，且 min-height:auto / min-content 会把 content 的下限锁在
	//   "子页内容高度"。这只在父容器为 definite height 时才体现（Quick 页 case 1 把
	//   contentScroll->content 锁 100% 视口高）：若不解除，消息越高 content 越高，整个页面
	//   被内容撑出视口、外层 contentScroll 出现滚动条、把固定高度的输入区顶到折叠区之下
	//   —— 正是"拉高再缩矮被裁剪"的根因（composer 固定高度 + 消息流 stretch=1）。
	//   对普通内容页无副作用：那种场景 contentScroll->content 是 auto 高度，容器高度本由
	//   content 驱动，flexShrink=1 在 auto 容器里不会触发收缩，整页滚动照常。
	content->setFlexShrink(1.f);
	YGNodeStyleSetMinHeight(content->node, 0.f);
	YGNodeSetMinContentHeight(content->node, 0.f);
	YGNodeStyleSetMinWidth(content->node, 0.f);
	content->setFlexDirection(Ling::FlexDirection::Column);

	// 加载默认页（热键页 index=0）
	loadPage(menuIndex);

	show();
}

// 7 菜单：按键顺序 Hotkeys → Quick → History → Plugins → Appearance → Features → About
void WinSetting::initMenuItems(Ling::Node* sideNav)
{
	const std::wstring keys[7] = {
		L"setting.hotkeys",
		L"setting.quick",
		L"setting.history",
		L"setting.plugins",
		L"setting.appearance",
		L"setting.function",
		L"setting.about",
	};
	const wchar_t* icons[7] = {
		Icon::Hotkey,      // 热键设置
		Icon::Quick,       // 快捷翻译
		Icon::Folder,      // 历史
		Icon::Plugin,      // 插件集成
		Icon::Appearance,  // 外观设置
		Icon::Function,    // 功能设置
		Icon::Logo,        // 关于
	};
	menus.clear();
	gMenuData.assign(7, MenuItemData{});

	auto menuBox = sideNav->makeChild<Ling::Node>();
	menuBox->setFlexGrow(1.0);
	menuBox->setFlexDirection(Ling::FlexDirection::Column);
	menuBox->setJustifyContent(Ling::Justify::Start);

	for (int i = 0; i < 7; ++i) {
		auto menuItem = menuBox->makeChild<Ling::Button>();
		menuItem->setWidthPercent(100.f);
		menuItem->setText(L""); // 内部 Text 置空，由手动 Label 控制
		menuItem->setFlexDirection(Ling::FlexDirection::Row);
		menuItem->setAlignItems(Ling::Align::Center);
		menuItem->setJustifyContent(Ling::Justify::Start);

		auto& d = gMenuData[i];
		d.icon = menuItem->makeChild<Ling::Label>();
		d.icon->setText(icons[i]);
		d.icon->setFontFamily(Icon::Family);
		d.icon->setFontSize(18.f);
		d.icon->setMarginRight(SettingTheme::sp3);
		d.icon->setFlexShrink(0.f);

		d.lab = menuItem->makeChild<Ling::Label>();
		d.lab->setText(Lang::get(keys[i]));
		d.lab->setFontSize(SettingTheme::fontLg);
		d.lab->setFlexShrink(1.f);

		ApplyMenuItemStyle(menuItem, i == menuIndex, d);
		menuItem->onClick.add([this](auto mi) { onMenuItemClick(mi); });
		menus.push_back(menuItem);
	}
}

void WinSetting::onMenuItemClick(Ling::Button* menuItem)
{
	auto index = Ling::Util::getIndex(menus, menuItem);
	if (index < 0 || index == menuIndex) return;

	// 切换选中样式
	ApplyMenuItemStyle(menus[menuIndex], false, gMenuData[menuIndex]);
	menuIndex = index;
	ApplyMenuItemStyle(menuItem, true, gMenuData[menuIndex]);

	// 加载对应子页面
	loadPage(menuIndex);
}

// 切到指定菜单页（托盘菜单「快捷翻译 / 截图历史 / 设置」用）。已打开时也生效；
// 与 onMenuItemClick 的区别是"目标页就是当前页"时直接返回，不重建页面。
void WinSetting::showPage(int index)
{
	if (menus.empty() || gMenuData.empty()) return;
	if (index < 0) index = 0;
	if (index > (int)menus.size() - 1) index = (int)menus.size() - 1;
	if (index == menuIndex) return;
	ApplyMenuItemStyle(menus[menuIndex], false, gMenuData[menuIndex]);
	menuIndex = index;
	ApplyMenuItemStyle(menus[menuIndex], true, gMenuData[menuIndex]);
	loadPage(menuIndex);
}

// 按 index 构建对应子页面。
// 约定：子页继承 Ling::Node。此处采用与 Ling::Node::makeChild 等价的手动挂接
// （个别 VS 版本在复杂派生链路中对 `std::derived_from<T, Node>` requires 子句
// 的判定偶有失败，因此直接 new 并 setChild，避免模板约束的编译器误差）。
void WinSetting::loadPage(int index)
{
	if (!content) return;

	// 若上一页是「直挂视口容器」的页（QuickTranslate / History）：它们不经 contentScroll，
	// 切走前必须先把 contentScroll 恢复显示、并把页面从 cardNode 摘除，
	// 否则页面残留为 cardNode 子节点、contentScroll 被 display:none 钉死。
	contentScroll->show();
	if (pageQuick) {
		cardNode->removeChild(pageQuick);   // 立即销毁 pageQuick（返回值丢弃 == 解构）
		pageQuick = nullptr;
	}
	if (pageHistory) {
		cardNode->removeChild(pageHistory);
		pageHistory = nullptr;
	}

	// 清理旧子页：先手动置空指针（避免访问悬空），再清空 content 的所有子节点
	pageHotkeys    = nullptr;
	pageQuick      = nullptr;
	pageHistory    = nullptr;
	pagePlugins    = nullptr;
	pageAppearance = nullptr;
	pageFeatures   = nullptr;
	pageAbout      = nullptr;
	content->removeAllChildren();

	// 恢复外层 content 为「内容驱动 + 允许整页滚动」的默认态，供普通内容页使用。
	// 高度保持 Auto：完全由子页内容决定。绝不 setMinHeightPercent(100%) 撑满视口 ——
	// 视口相关高度在「拉高→缩回」后可能残留，导致底部多余空白 + 假滚动条。
	YGNodeStyleSetHeightAuto(contentScroll->content->node);

	// 让子页占满 content
	auto makeFullFill = [](Ling::Node* n) {
		n->setWidthPercent(100.f);
		n->setFlexGrow(1.0);
		n->setFlexShrink(1.0);
		// 关键：解除 flex 项目默认的 min-content 下限（min-height:auto / min-width:auto），
		// 并允许被 content 压缩。否则子页会因内容高度撑满而拒绝被压缩，
		// 内部 ScrollerBox 拿不到受限高度、不出现溢出、无法滚动。
		// min-width:0 则让长文本在水平方向也能收缩（配合 Text 折行），避免把卡片撑宽。
		// 与 ScrollerBox 构造函数同策略（绝对 min-*:0 更可靠）。
		YGNodeStyleSetMinHeight(n->node, 0.f);
		YGNodeStyleSetMinWidth(n->node, 0.f);
		n->setFlexDirection(Ling::FlexDirection::Column);
	};

	// 等价于 makeChild<T>(this)：new → adoptChild 挂接 + 所有权转移（SettingUi 友元模板访问 protected setChild）
	auto adopt = [this](Ling::Node* child) {
		SettingUi::adoptChild(content, child);
	};

	// 菜单顺序：0=Hotkeys 1=Quick 2=History 3=Plugins 4=Appearance 5=Features 6=About
	switch (index) {
	case 0:
		pageHotkeys = new WinSettingHotkeys(this);
		adopt(pageHotkeys); makeFullFill(pageHotkeys);
		break;
	case 1:
		// 快捷翻译页自管理布局（仅页内 msgScroll/histScroll 滚动），
		// 直接把该页塞进视口容器、不包外层整页滚动面板。
		// 做法：隐藏整页滚动的 contentScroll，把 pageQuick 直接挂到 cardNode（=视口容器）。
		// 于是页面高度恒等于视口高度（definite height），Yoga 以受限高度测量子页；
		// 子页内 topBar(固定32) + msgWrap(flexGrow) + inputArea(底部输入区) 在受限高度内排布，
		// 记录流占满剩余空间并内部滚动、固定高度的输入区始终贴底完整可见。
		// 从结构上彻底移除外层 ScrollBox —— 杜绝 "内容把外层撑出视口 → 整页滚动条 → 输入区被
		// 折叠在滚动区之下" 这一"拉高再缩矮被裁剪"的根因。
		contentScroll->hide();
		pageQuick = new WinSettingQuickTranslate(this);
		SettingUi::adoptChild(cardNode, pageQuick);
		makeFullFill(pageQuick);
		// 还原卡片内边距：旧方案里这块 padding 由 contentScroll->content->setPadding(...) 提供，
		// 让内容缩进卡片边框之内，露出 1px 描边 + 16px 圆角 + 四周画布留白。本页改为直接挂
		// cardNode 后若不补回等价 padding，页面内容会顶满到内容盒边缘、把白卡描边/圆角盖掉
		// —— 即"四周边框都没了"。此处给 pageQuick 自身加同样的 contentPadX/Y，内容内缩后
		// 边框/圆角恢复可见，同时仍保持 definite height（无外层滚动、输入区不被裁剪）。
		pageQuick->setPadding(SettingTheme::contentPadX, SettingTheme::contentPadY,
			SettingTheme::contentPadX, SettingTheme::contentPadY);
		break;
	case 2:
		// 截图历史页：底部一行（打开目录 / 清空历史 / 当前条目）需始终贴住视口底部 →
		// 与快捷翻译页同策略，直接把该页塞进视口容器 cardNode（页面高度恒等于视口高度），
		// 页内只让中部缩略图网格自建滚动，底行因此恒贴底、不随网格滚走。
		contentScroll->hide();
		pageHistory = new WinSettingHistory(this);
		SettingUi::adoptChild(cardNode, pageHistory);
		makeFullFill(pageHistory);
		// 页面自带内容内边距（原先由 contentScroll->content 提供）
		pageHistory->setPadding(SettingTheme::contentPadX, SettingTheme::contentPadY,
			SettingTheme::contentPadX, SettingTheme::contentPadY);
		break;
	case 3:
		pagePlugins = new WinSettingPlugins(this);
		adopt(pagePlugins); makeFullFill(pagePlugins);
		break;
	case 4:
		pageAppearance = new WinSettingAppearance(this);
		adopt(pageAppearance); makeFullFill(pageAppearance);
		break;
	case 5:
		pageFeatures = new WinSettingFeatures(this);
		adopt(pageFeatures); makeFullFill(pageFeatures);
		break;
	case 6:
		pageAbout = new WinSettingAbout(this);
		adopt(pageAbout); makeFullFill(pageAbout);
		break;
	}
}

LRESULT WinSetting::onHitTest(const POINT pos)
{
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	// ★ 使用 WS_OVERLAPPEDWINDOW 后，系统原生标题栏自带：
	//    拖动、双击最大化、最小/最大化/关闭按钮（含关闭按钮悬停红底）
	// 这里仅保留 client 区的边缘 resize hit-test（非最大化时）
	if (!isMaximized) {
		auto result = borderHitTest(pt);
		if (result != HTCLIENT) return result;
	}
	return HTCLIENT;
}
