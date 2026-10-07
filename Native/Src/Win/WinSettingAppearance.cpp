#include "pch.h"
#include <algorithm>
#include <cmath>
#include "../Lang.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "WinSettingAppearance.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
#include "../Tool/IconCodes.h"
#include "../Tool/ToolbarTheme.h"   // 预览条镜像实际工具栏用的同一套常量
#include "../Tip.h"                  // chip 悬停文字（系统 tooltip）
#include "../TrayIcon.h"             // 托盘图标样式（预览上色 / 应用）
#include <commdlg.h>
#include <shobjidl.h>

namespace {
	// 延迟销毁弹层：popup 常在宿主 onMouseDown 事件订阅回调 / 全局鼠标钩子回调中被关闭
	// （点击浮层外部、或点击触发按钮切换当前下拉）。若在这里同步 reset()，会在事件/钩子回调
	// 中途触发 Popup 析构 → UnhookWindowsHookEx + DestroyWindow + host->onMouseDown.remove(当前 token)，
	// 使 winrt::event 订阅列表迭代器失效并造成 Win32 重入，表现为 UI 线程未响应、多次重复后崩溃。
	// 把所有权移入 pending 推迟到调度队列再真正析构，避开事件迭代期间的自我销毁。
	void deferPopupReset(std::unique_ptr<SettingUi::Popup>& popup)
	{
		if (!popup) return;
		if (auto* app = Ling::App::get()) {
			auto pending = std::make_shared<std::unique_ptr<SettingUi::Popup>>();
			*pending = std::move(popup);
			app->dq.TryEnqueue([pending]() { pending->reset(); });
		} else {
			popup.reset();   // 无调度队列时兜底：同步析构（正常路径不会走到）
		}
	}
	bool tbIsSplitId(const std::wstring& id) { return id.rfind(L"split", 0) == 0; }
	void tbVecErase(std::vector<std::wstring>& v, const std::wstring& id)
	{
		for (auto it = v.begin(); it != v.end();) {
			if (*it == id) it = v.erase(it); else ++it;
		}
	}
	bool tbVecHas(const std::vector<std::wstring>& v, const std::wstring& id)
	{
		for (auto& s : v) if (s == id) return true;
		return false;
	}
	// 隐藏区固定格式：split-extra 模板在首位 + 其余工具（不含分隔符）
	std::vector<std::wstring> tbNormalizeInactive(std::vector<std::wstring> list)
	{
		std::vector<std::wstring> tools;
		for (auto& id : list) if (!tbIsSplitId(id)) tools.push_back(id);
		std::vector<std::wstring> out{ L"split-extra" };
		for (auto& t : tools) out.push_back(t);
		return out;
	}
	uint32_t tbFade(uint32_t c, uint8_t a) { return (uint32_t(a) << 24) | (c & 0xFFFFFF); }
	uint32_t tbSlotColor(const ToolbarStore::Colors& c, int slot)
	{
		// 5 个色槽：背景 / 停悬 / 图标 / 选中 / 框选（原先的"停悬图标"已并入"图标"）
		switch (slot) {
		case 0: return c.primary;
		case 1: return c.hover;
		case 2: return c.icon;
		case 3: return c.accent;
		case 4: return c.selection;
		}
		return 0;
	}
}

WinSettingAppearance::WinSettingAppearance(Ling::WinBase* parent) :Ling::Node(parent)
{
    // 悬停文字（系统 tooltip，需要 hwnd）。必须在构建各 Tab 之前建好：
    // tbBuildTheme 在构建期就会给自定义色块 bind，若此刻 tbTip_ 还是空，bind 被跳过 → 色块永远没有提示。
    tbTip_ = std::make_unique<Tip>(win);
    // 3 个子 Tab：工具栏外观 / 软件外观 / 其他外观（分段胶囊滑动 + 右侧横排「恢复默认/预览效果」）
    auto tabRow = makeChild<Ling::Node>();
    tabRow->setFlexDirection(Ling::FlexDirection::Row);
    tabRow->setWidthPercent(100.f);
    tabRow->setAlignItems(Ling::Align::Center);
    tabRow->setMarginBottom(SettingTheme::blockGap);
    buildSegTabs(tabRow);
    // 胶囊自适应宽度 + 中间弹性空白，把右侧「恢复默认/预览效果」推到最右
    auto* spacer = tabRow->makeChild<Ling::Node>();
    spacer->setFlexGrow(1.f);
    spacer->setFlexShrink(1.f);
    buildTabActions(tabRow);
    tabHost = makeChild<Ling::Node>();
    tabHost->setFlexGrow(1.f);
    tabHost->setWidthPercent(100.f);
    tabHost->setPositionType(Ling::Position::Relative);

    tabs[0] = tabHost->makeChild<Ling::Node>();
    tabs[0]->setFlexGrow(1.f); tabs[0]->setWidthPercent(100.f);
    tabs[1] = tabHost->makeChild<Ling::Node>();
    tabs[1]->setFlexGrow(1.f); tabs[1]->setWidthPercent(100.f);
    tabs[2] = tabHost->makeChild<Ling::Node>();
    tabs[2]->setFlexGrow(1.f); tabs[2]->setWidthPercent(100.f);
    buildToolbarTab(tabs[0]);
    buildAppTab(tabs[1]);
    buildOtherTab(tabs[2]);
    showTab(0);
    // 内容区只有外层 content 提供底部留白：清掉各 tab 末尾元素的 marginBottom，使底部与左右一致。
    SettingUi::trimTrailingGap(tabs[0]);
    SettingUi::trimTrailingGap(tabs[1]);
    SettingUi::trimTrailingGap(tabs[2]);

    // 拖拽幽灵：绝对定位浮块（不占布局），跟手显示被拖的图标；随本页一起销毁
    tbGhost = makeChild<Ling::Node>();
    tbGhost->setPositionType(Ling::Position::Absolute);
    tbGhost->setSize(36.f, 36.f);
    tbGhost->setBg(SettingTheme::cardBg);
    tbGhost->setBorder(1.f, SettingTheme::border);
    tbGhost->setBorderRadius(SettingTheme::radiusSm);
    tbGhost->setFlexDirection(Ling::FlexDirection::Row);
    tbGhost->setAlignItems(Ling::Align::Center);
    tbGhost->setJustifyContent(Ling::Justify::Center);
    tbGhostIcon = tbGhost->makeChild<Ling::Label>();
    tbGhostIcon->setFontFamily(Icon::Family);
    tbGhostIcon->setFontSize(Icon::SizeSm);
    tbGhostIcon->setColor(SettingTheme::textPrimary);
    tbGhost->hide();
    // chip 右上角红叉（页级共享、绝对定位）：放在 tabs 之后创建 → 初始即在最上层
    tbChipClose = makeChild<Ling::Button>();
    tbChipClose->setPositionType(Ling::Position::Absolute);
    tbChipClose->setSize(14.f, 14.f);
    tbChipClose->setBorderRadius(SettingTheme::radiusFull);
    tbChipClose->setBg(0xFF383CFF);
    tbChipClose->setHoverBg(0xFF383CFF);
    // 用图标字库的"取消/叉"字形（图标字形在 em 框里居中，比 UI 字体的 × 更居中）
    tbChipClose->setText(Icon::Cancel);
    tbChipClose->setFontFamily(Icon::Family);
    tbChipClose->setFontSize(9.f);
    tbChipClose->setColor(0xFFFFFFFF);
    tbChipClose->setHoverColor(0xFFFFFFFF);
    tbChipClose->setFlexDirection(Ling::FlexDirection::Row);
    tbChipClose->setJustifyContent(Ling::Justify::Center);
    tbChipClose->setAlignItems(Ling::Align::Center);
    tbChipClose->onClick.add([this](Ling::Button*) {
        if (tbChipCloseId.empty() || tbLocked(tbChipCloseId)) return;
        // 复用拖拽的"移到隐藏区"逻辑：把该工具挪出去
        tbDragId = tbChipCloseId;
        tbDragFrom = 0;
        tbDragTo = 1;
        tbDropIndex = (int)tbActive.size();
        tbHideChipClose();
        tbDrop();
    });
    // 鼠标挪到叉自己身上时（它有一半在图标外，chip 会先报 onLeave）保持显示
    tbChipClose->onEnter.add([this](Ling::Button*) { if (tbChipClose) tbChipClose->show(); });
    tbChipClose->onLeave.add([this](Ling::Button*) { this->tbHideChipClose(); });
    tbChipClose->hide();

    auto weakThis = getWeakThis();
    win->onDestroy.add([this, weakThis]() {
        if (!weakThis.lock()) return;
        this->hideSelectBox();
        });
    // 工具栏编辑器 chip 命中检测（按下即检测，拖拽/点击在 move/up 阶段分流）
    auto weakTb = getWeakThis();
    tbDownTok = win->onMouseDown.add([this, weakTb](POINT pos, bool right) {
        if (!weakTb.lock()) return;
        this->tbOnDown(pos, right);
        });
}

WinSettingAppearance::~WinSettingAppearance()
{
    selectPopup.reset();
    tbKindPopup.reset();
    win->onMouseDown.remove(tbDownTok);
    win->onMouseMove.remove(tbMoveTok);
    win->onMouseUp.remove(tbUpTok);
}

void WinSettingAppearance::buildTabActions(Ling::Node* row)
{
    // 与子 Tab 同行的右侧操作：恢复默认 / 预览效果（参考图顶部行）
    auto mkOutline = [](Ling::Node* p, const std::wstring& text) {
        auto* b = p->makeChild<Ling::Button>();
        b->setText(text);
        b->setHeight(32.f);
        b->setPadding(12.f, 0, 12.f, 0);
        b->setBorderRadius(SettingTheme::radiusSm);
        b->setBorder(1.f, SettingTheme::input);
        b->setBg(SettingTheme::cardBg);
        b->setColor(SettingTheme::textPrimary);
        b->setHoverBg(SettingTheme::accent);
        b->setHoverColor(SettingTheme::textPrimary);
        b->setFlexDirection(Ling::FlexDirection::Row);
        b->setJustifyContent(Ling::Justify::Center);
        b->setAlignItems(Ling::Align::Center);
        b->setMarginLeft(8.f);
        return b;
        };
    tbRestoreBtn = mkOutline(row, Lang::get(L"setting.tbRestore"));
    tbRestoreBtn->onClick.add([this](Ling::Button*) { this->tbRestoreDefaults(); });
    tbPreviewBtn = mkOutline(row, Lang::get(L"setting.tbPreview"));
    tbPreviewBtn->onClick.add([this](Ling::Button*) { this->tbTogglePreview(); });
}


void WinSettingAppearance::showTab(int index)
{
    if (index < 0 || index > 2) return;
    tabIndex = index;
    // 同步胶囊选中态（不触发 onChange，避免点击后递归）
    segHighlight(index);
    // 「预览效果」只属于工具栏外观页：切到软件/其他外观时隐藏
    if (tbPreviewBtn) {
        if (index == 0) tbPreviewBtn->show(); else tbPreviewBtn->hide();
    }
    for (int i = 0; i < 3; ++i) {
        if (!tabs[i]) continue;
        if (i == index) tabs[i]->show(); else tabs[i]->hide();
    }
}

// 顶部 3 个分段胶囊：保留现有「选中 primary 黑底白字 / 未选中透明黑字 + hover 高亮」样式，
// 但 Tab0 改为「标题 + ▾下拉」组合以承接工具栏类型切换（点击标题切 Tab，点击 ▾ 弹类型菜单）。
void WinSettingAppearance::buildSegTabs(Ling::Node* row)
{
    auto* track = row->makeChild<Ling::Node>();
    track->setFlexDirection(Ling::FlexDirection::Row);
    track->setHeight(SettingTheme::ctrlH);
    track->setBorderRadius(SettingTheme::radiusLg);
    track->setBg(SettingTheme::secondary);

    // Tab0：胶囊容器（标题按钮 + ▾按钮），整体按胶囊样式绘制，选中态由 segHighlight 画 primary 底
    seg0Box = track->makeChild<Ling::Node>();
    seg0Box->setHeight(SettingTheme::ctrlH);
    seg0Box->setBorderRadius(SettingTheme::radiusLg);
    seg0Box->setFlexDirection(Ling::FlexDirection::Row);
    seg0Box->setAlignItems(Ling::Align::Center);
    seg0Box->setJustifyContent(Ling::Justify::Center);
    seg0Box->setBg(0);   // 未选中：透明，露出轨道 secondary

    // Tab0 整条 = 一个按钮（标题 + ▾）：左 12 文字留白 / 右 8 箭头外距，
    // 配合箭头 marginLeft 8 → 箭头左右各 8，间距一致。
    seg0TitleBtn = seg0Box->makeChild<Ling::Button>();
    seg0TitleBtn->setHeight(SettingTheme::ctrlH);
    seg0TitleBtn->setPadding(12.f, 0, 8.f, 0);
    seg0TitleBtn->setBorderRadius(SettingTheme::radiusLg);
    seg0TitleBtn->setFlexDirection(Ling::FlexDirection::Row);
    seg0TitleBtn->setJustifyContent(Ling::Justify::Center);
    seg0TitleBtn->setAlignItems(Ling::Align::Center);
    // 整条都是下拉触发：已是当前页 → 弹类型菜单；否则先切到该页
    seg0TitleBtn->onClick.add([this](Ling::Button*) {
        if (tabIndex == 0) this->tbShowKindMenu();
        else this->showTab(0);
    });
    // ▾ 图标：按钮内的一个 Label（不单独成按钮），左右各留 8px
    seg0Arrow = seg0TitleBtn->makeChild<Ling::Label>();
    seg0Arrow->setText(Icon::Dropdown);
    seg0Arrow->setFontFamily(Icon::Family);
    seg0Arrow->setFontSize(Icon::DropSize);
    seg0Arrow->setMarginLeft(8.f);
    seg0Arrow->setFlexShrink(0.f);

    // Tab1 / Tab2：普通胶囊按钮（样式与现有 segmentedPills 一致）
    auto makeSeg = [&](const std::wstring& text) -> Ling::Button* {
        auto* b = track->makeChild<Ling::Button>();
        b->setText(text);
        b->setHeight(SettingTheme::ctrlH);
        b->setPadding(SettingTheme::sp4, 0, SettingTheme::sp4, 0);
        b->setFontSize(SettingTheme::fontBase);
        b->setBorderRadius(SettingTheme::radiusLg);
        b->setFlexDirection(Ling::FlexDirection::Row);
        b->setJustifyContent(Ling::Justify::Center);
        b->setAlignItems(Ling::Align::Center);
        return b;
    };
    segBtn1 = makeSeg(Lang::get(L"setting.appTab"));
    segBtn1->onClick.add([this](Ling::Button*) { this->showTab(1); });
    segBtn2 = makeSeg(Lang::get(L"setting.otherTab"));
    segBtn2->onClick.add([this](Ling::Button*) { this->showTab(2); });

    tbUpdateKindTitle();
    segHighlight(0);
}

void WinSettingAppearance::segHighlight(int index)
{
    if (index < 0 || index > 2) index = 0;
    // Tab0 胶囊容器：整块画 primary 底（选中）或透明（未选中）
    if (seg0Box) {
        bool on = (index == 0);
        seg0Box->setBg(on ? SettingTheme::primary : 0);
        if (seg0TitleBtn) {
            seg0TitleBtn->setBorder(0.f, 0);
            seg0TitleBtn->setBg(0);
            seg0TitleBtn->setHoverBg(on ? SettingTheme::primary : SettingTheme::accent);
            seg0TitleBtn->setColor(on ? SettingTheme::primaryForeground : SettingTheme::textPrimary);
            seg0TitleBtn->setHoverColor(on ? SettingTheme::primaryForeground : SettingTheme::textPrimary);
        }
        // ▾ 图标跟随选中态取色（选中=深色上的浅字，未选中=次级灰）
        if (seg0Arrow) {
            seg0Arrow->setColor(on ? SettingTheme::primaryForeground : SettingTheme::textMuted);
        }
    }
    // Tab1 / Tab2 独立胶囊按钮
    auto styleBtn = [](Ling::Button* btn, bool on) {
        if (!btn) return;
        btn->setBorder(0.f, 0);
        if (on) {
            btn->setBg(SettingTheme::primary);
            btn->setHoverBg(SettingTheme::primary);
            btn->setColor(SettingTheme::primaryForeground);
            btn->setHoverColor(SettingTheme::primaryForeground);
        } else {
            btn->setBg(0);
            btn->setHoverBg(SettingTheme::accent);
            btn->setColor(SettingTheme::textPrimary);
            btn->setHoverColor(SettingTheme::textPrimary);
        }
    };
    styleBtn(segBtn1, index == 1);
    styleBtn(segBtn2, index == 2);
}

// 刷新 Tab0 标题：××工具栏外观（× = 当前工具栏类型）
void WinSettingAppearance::tbUpdateKindTitle()
{
    if (!seg0TitleBtn) return;
    std::wstring key = L"setting.tbKind" + std::to_wstring(tbKind);
    seg0TitleBtn->setText(Lang::get(key) + Lang::get(L"setting.tbKindAppend"));
}

// Tab0 ▾ 下拉：弹出工具栏类型菜单，选中后切换编辑区内容并刷新标题（不切换 Tab）
void WinSettingAppearance::tbShowKindMenu()
{
    if (tbKindPopup) { deferPopupReset(tbKindPopup); return; }   // 已打开则切换为关闭
    SettingUi::closePopup(win);
    std::vector<std::wstring> options = {
        Lang::get(L"setting.tbKind0"), Lang::get(L"setting.tbKind1"),
        Lang::get(L"setting.tbKind2"), Lang::get(L"setting.tbKind3")
    };
    const float itemH = 32.f;
    const float itemGap = 4.f;   // 选项之间 4px 间距
    const float itemN = (float)options.size();
    const float totalH = std::min(280.f, itemH * itemN + itemGap * std::max(0.f, itemN - 1.f) + 8.f);
    // 下拉宽度 = 导航胶囊宽度（与触发条等宽）
    float pw = SettingTheme::dropdownWidth;
    if (seg0TitleBtn && seg0TitleBtn->w > 0.f && win->dpi > 0.f) pw = seg0TitleBtn->w / win->dpi;
    tbKindPopup = std::make_unique<SettingUi::Popup>(win, pw, totalH);
    auto* box = tbKindPopup->box();
    box->setBg(SettingTheme::popover);
    box->setBorder(1.f, SettingTheme::border);
    box->setBorderRadius(SettingTheme::radiusSm);
    box->setPadding(4.f, 4.f, 4.f, 4.f);
    auto weakThis = getWeakThis();
    for (int i = 0; i < (int)options.size(); i++) {
        auto* item = box->makeChild<Ling::Button>();
        item->setText(options[i]);
        item->setHeight(itemH);
        item->setWidthPercent(100.f);
        item->setFlexDirection(Ling::FlexDirection::Row);
        item->setJustifyContent(Ling::Justify::Start);
        item->setAlignItems(Ling::Align::Center);
        item->setPadding(8.f, 0, 8.f, 0);
        item->setBorderRadius(SettingTheme::radiusInner);   // 内层小框：与外框(10)同心
        if (i + 1 < (int)options.size()) item->setMarginBottom(itemGap);
        bool isCurrent = (i == tbKind);
        // 当前项：底色取与其它项 hover 相同的浅灰，hover 底色/文字色取同值 → 悬停不产生任何变化。
        item->setColor(SettingTheme::textPrimary);
        item->setBg(isCurrent ? SettingTheme::accent : 0);
        item->setHoverBg(SettingTheme::accent);
        if (isCurrent) item->setHoverColor(SettingTheme::textPrimary);
        int idx = i;
        item->onClick.add([this, weakThis, idx](Ling::Button*) {
            if (!weakThis.lock()) return;
            this->tbSetKind(idx);
            this->tbUpdateKindTitle();
            if (this->tbKindPopup) this->tbKindPopup.reset();
        });
    }
    // 菜单左对齐胶囊整体（标题按钮贴胶囊左侧），避免跟随最右 ▾ 按钮导致偏右。
    // 锚定与触发都是整条胶囊：全局钩子在"点击触发按钮时"不当作外部点击先关闭，
    // 收起/展开交给按钮 onClick 自己切换（与其它下拉一致）。
    tbKindPopup->setAnchor(seg0TitleBtn);
    tbKindPopup->setTrigger(seg0TitleBtn);
    tbKindPopup->onDismiss = [this]() { deferPopupReset(this->tbKindPopup); };
    // 延迟 open：本回调运行在宿主 onMouseDown 派发循环内，推迟到调度队列避免增删订阅崩溃。
    auto* app = Ling::App::get();
    if (app) {
        auto* popup = tbKindPopup.get();
        app->dq.TryEnqueue([popup]() { popup->open(); });
    } else {
        tbKindPopup->open();
    }
}

void WinSettingAppearance::buildToolbarTab(Ling::Node* host)
{
    // 工具栏外观：类型切换 / 显隐 / 拖拽排序 / 恢复默认 / 预览 / 主题色板
    host->setFlexDirection(Ling::FlexDirection::Column);
    tbScroller = host->makeChild<Ling::ScrollerBox>();
    tbScroller->setFlexGrow(1.f);
    tbScroller->setWidthPercent(100.f);
    tbScroller->setScrollBarVisible(false);
    Ling::Node* content = tbScroller;

    // 预览容器（默认隐藏，点「预览效果」展开）：放在**最前面**，展开时一眼就能看到
    tbPreviewWrap = content->makeChild<Ling::Node>();
    tbPreviewWrap->setWidthPercent(100.f);
    tbPreviewWrap->hide();

    // 显示区（无标题，仅卡片分区，与参考图一致；内容区统一不描边）
    auto* activeCard = SettingUi::card(content);
    tbActiveBox = activeCard->makeChild<Ling::Node>();
    tbActiveBox->setFlexDirection(Ling::FlexDirection::Row);
    tbActiveBox->setFlexWrap(Ling::Wrap::Wrap);
    tbActiveBox->setAlignItems(Ling::Align::Center);

    // 隐藏区（无标题；空时由 tbBuildChips 追加居中的预留提示文字；不描边）
    auto* inactiveCard = SettingUi::card(content);
    tbInactiveBox = inactiveCard->makeChild<Ling::Node>();
    tbInactiveBox->setFlexDirection(Ling::FlexDirection::Row);
    tbInactiveBox->setFlexWrap(Ling::Wrap::Wrap);
    tbInactiveBox->setAlignItems(Ling::Align::Center);

    // 主题色板
    tbBuildTheme(content);

    // 初始数据（截图工具栏）
    auto st = ToolbarStore::loadLayout(0);
    tbActive = st.active;
    tbInactive = st.inactive;
    tbRebuildZones();
}

void WinSettingAppearance::buildAppTab(Ling::Node* host)
{
    // 内容滚动容器：软件外观控件超出窗口高度时可滚动
    host->setFlexDirection(Ling::FlexDirection::Column);
    auto* sc = host->makeChild<Ling::ScrollerBox>();
    sc->setFlexGrow(1.f);
    sc->setWidthPercent(100.f);
    sc->setScrollBarVisible(false);
    {
        // 软件主题 + 软件语言：双栏卡片（各自白底描边独立卡片）
        auto* g = SettingUi::grid2(sc);
        initThemeCtrls(g);
        initLangCtrls(g);
        SettingUi::grid2Cells(g);
        // grid2Cells 会把末行底部 margin 归零（给 trimTrailingGap 用的），但这里后面还有卡片，
        // 所以要自己补一个块间距，否则「软件主题/语言」与下面的卡片贴死。
        g->setMarginBottom(SettingTheme::blockGap);
    }
    initPinBorderCtrls(sc);
    initTrayCtrls(sc);
}

void WinSettingAppearance::buildOtherTab(Ling::Node* host)
{
    SettingUi::toggleRow(host,
        Lang::get(L"setting.featureTips"),
        Setting::get()->getFeatureTips(),
        [](bool on) { Setting::get()->setFeatureTips(on); }, true);
}

// ---- 软件语言下拉：与其他下拉选项样式一致（标准 selectRow） ----
void WinSettingAppearance::initLangCtrls(Ling::Node* p)
{
    auto langs = Lang::get()->getSupportedLang();
    std::vector<std::wstring> names;
    names.reserve(langs.size());
    for (auto& pair : langs) names.push_back(pair.first);
    auto langCode = Setting::get()->getLang();
    int cur = 0;
    for (int i = 0; i < (int)langs.size(); i++)
        if (langs[i].second == langCode) { cur = i; break; }
    SettingUi::selectRow(p, Lang::get(L"setting.softwareLang"), names, cur,
        [this, langs](int i, const std::wstring&) {
            if (i < 0 || i >= (int)langs.size()) return;
            std::wstring langCode = langs[i].second;
            // 语言切换需刷新整窗；延迟到浮层关闭后再执行，避免与 destroyPopup 的 body 访问冲突。
            Ling::App::get()->dq.TryEnqueue([this, langCode]() {
                Setting::get()->setLang(langCode);
                win->close();
                Ling::App::get()->dq.TryEnqueue([]() { WinSetting::init(); });
            });
        }, true);
}

// ---- 软件主题下拉：跟随系统/浅色/深色 ----
void WinSettingAppearance::initThemeCtrls(Ling::Node* p)
{
    const int cur = Setting::get()->getAppTheme();
    SettingUi::selectRow(p, Lang::get(L"setting.theme"),
        { Lang::get(L"setting.themeFollow"), Lang::get(L"setting.themeLight"), Lang::get(L"setting.themeDark") },
        cur < 0 || cur > 2 ? 0 : cur,
        [](int i, const std::wstring&) { Setting::get()->setAppTheme(i); }, true);
}

// ---- 贴图边框卡（颜色 + 滑条 + 右侧效果预览） ----
void WinSettingAppearance::initPinBorderCtrls(Ling::Node* p)
{
    auto* card = SettingUi::card(p);
    card->setBorderRadius(SettingTheme::radiusRow);   // 与本页「软件主题/软件语言」卡片同款大圆角(14)

    // 标题行：标题 + 右侧「默认启用」开关（开关只决定新建贴图是否带边框，下方样式/预览始终可改）
    auto* head = card->makeChild<Ling::Node>();
    head->setFlexDirection(Ling::FlexDirection::Row);
    head->setAlignItems(Ling::Align::Center);
    head->setWidthPercent(100.f);
    head->setMarginBottom(SettingTheme::blockGap);
    auto* title = head->makeChild<Ling::Label>();
    title->setText(Lang::get(L"setting.pinBorder"));
    title->setFontSize(13.f);
    title->setColor(SettingTheme::textPrimary);
    auto* headGrow = head->makeChild<Ling::Node>();
    headGrow->setFlexGrow(1.f);
    auto* sw = head->makeChild<Ling::Button>();
    SettingUi::styleToggle(sw, Setting::get()->getPinBorderDefaultEnabled());
    sw->onClick.add([sw](Ling::Button*) {
        const bool on = !Setting::get()->getPinBorderDefaultEnabled();
        Setting::get()->setPinBorderDefaultEnabled(on);
        SettingUi::styleToggle(sw, on);
        });

    // 主体：左列（颜色 + 粗细 + 圆角）+ 右侧效果预览
    auto* body = card->makeChild<Ling::Node>();
    body->setFlexDirection(Ling::FlexDirection::Row);
    body->setAlignItems(Ling::Align::FlexEnd);
    body->setWidthPercent(100.f);

    auto* left = body->makeChild<Ling::Node>();
    left->setFlexDirection(Ling::FlexDirection::Column);
    left->setFlexGrow(1.f);
    left->setFlexShrink(1.f);
    left->setMarginRight(SettingTheme::blockGap);

    constexpr float labW = 56.f;

    // 颜色行：标签 + 色块 + #hex
    {
        auto* row = left->makeChild<Ling::Node>();
        row->setFlexDirection(Ling::FlexDirection::Row);
        row->setAlignItems(Ling::Align::Center);
        row->setWidthPercent(100.f);
        row->setMarginBottom(10.f);   // 行距 10
        auto* lab = row->makeChild<Ling::Label>();
        lab->setText(Lang::get(L"setting.pinBorderColor"));
        lab->setFontSize(13.f);
        lab->setColor(SettingTheme::textPrimary);
        lab->setWidth(labW);
        lab->setFlexShrink(0.f);
        const uint32_t col = Setting::get()->getPinBorderColor();
        auto* swatch = row->makeChild<Ling::Button>();
        swatch->setSize(18.f, 18.f);
        swatch->setBorderRadius(2.f);
        swatch->setBg(col);
        swatch->setHoverBg(col);
        swatch->setFlexShrink(0.f);
        swatch->onClick.add([this](Ling::Button*) { this->pickPinColor(); });
        pinColorSwatch = swatch;
        auto* hex = row->makeChild<Ling::Label>();
        wchar_t buf[16];
        swprintf_s(buf, L"#%02X%02X%02X", (col >> 24) & 0xFF, (col >> 16) & 0xFF, (col >> 8) & 0xFF);
        hex->setText(buf);
        hex->setFontSize(11.f);
        hex->setColor(SettingTheme::textTertiary);
        hex->setMarginLeft(8.f);
        pinColorHex = hex;
    }

    // 滑条行：标签 + 拖动条 + 数值（Npx）
    auto addSliderRow = [left, labW](const std::wstring& name, int val, int max, Ling::Label** outVal) -> Ling::Slider* {
        auto* row = left->makeChild<Ling::Node>();
        row->setFlexDirection(Ling::FlexDirection::Row);
        row->setAlignItems(Ling::Align::Center);
        row->setWidthPercent(100.f);
        row->setMarginBottom(10.f);   // 行距 10
        auto* lab = row->makeChild<Ling::Label>();
        lab->setText(name);
        lab->setFontSize(13.f);
        lab->setColor(SettingTheme::textPrimary);
        lab->setWidth(labW);
        lab->setFlexShrink(0.f);
        auto* sl = row->makeChild<Ling::Slider>();
        sl->setFlexGrow(1.f);
        sl->setFlexShrink(1.f);
        sl->setHeight(20.f);
        sl->setRange(0.f, (float)max);
        sl->setStep(1.f);
        sl->setValue((float)val);
        // 滑条配色：浅色=浅槽 + 深柄 + 绿色已选段（白柄在浅底上会看不见）
        sl->setTrackColor(SettingTheme::zinc200);
        sl->setFillColor(0x34C759FFu);
        sl->setThumbColor(SettingTheme::primary);
        sl->setHoverThumbColor(SettingTheme::primary);
        auto* v = row->makeChild<Ling::Label>();
        v->setText(std::to_wstring(val) + L"px");
        v->setFontSize(12.f);
        v->setColor(SettingTheme::textTertiary);
        v->setWidth(36.f);
        v->setFlexShrink(0.f);
        v->setMarginLeft(8.f);
        *outVal = v;
        return sl;
        };

    auto* wSlider = addSliderRow(Lang::get(L"setting.pinBorderWidth"),
        Setting::get()->getPinBorderWidth(), 12, &pinWidthVal);
    auto* rSlider = addSliderRow(Lang::get(L"setting.pinBorderRadius"),
        Setting::get()->getPinBorderRadius(), 24, &pinRadiusVal);
    wSlider->onValueChanged.add([this](Ling::Slider*, float v) {
        const int iv = (int)(v + 0.5f);
        Setting::get()->setPinBorderWidth(iv);
        if (this->pinWidthVal) this->pinWidthVal->setText(std::to_wstring(iv) + L"px");
        this->refreshPinPreview();
        });
    rSlider->onValueChanged.add([this](Ling::Slider*, float v) {
        const int iv = (int)(v + 0.5f);
        Setting::get()->setPinBorderRadius(iv);
        if (this->pinRadiusVal) this->pinRadiusVal->setText(std::to_wstring(iv) + L"px");
        this->refreshPinPreview();
        });

    // 右侧效果预览（宽 136；高度改矮，与左侧三行齐平，避免卡片被撑高）
    auto* preview = body->makeChild<Ling::Node>();
    preview->setSize(136.f, 80.f);
    preview->setFlexShrink(0.f);
    preview->setAlignItems(Ling::Align::Center);
    preview->setJustifyContent(Ling::Justify::Center);
    pinPreview = preview;
    auto* plab = preview->makeChild<Ling::Label>();
    plab->setText(Lang::get(L"setting.pinPreview"));
    plab->setFontSize(11.f);
    plab->setColor(SettingTheme::textTertiary);
    plab->setFlexShrink(1.f);

    refreshPinPreview();
}

void WinSettingAppearance::refreshPinPreview()
{
    if (!pinPreview) return;
    const uint32_t col = Setting::get()->getPinBorderColor();
    const int w = Setting::get()->getPinBorderWidth();
    const int r = Setting::get()->getPinBorderRadius();
    pinPreview->setBorderRadius((float)r);
    pinPreview->setBg(SettingTheme::card);   // 白底：靠描边区分边界
    if (w > 0) pinPreview->setBorder((float)w, col);
    else pinPreview->setBorder(1.f, SettingTheme::border);   // 0 粗细：画 1px 通用描边，示意边框位置
}

void WinSettingAppearance::pickPinColor()
{
    const uint32_t cur = Setting::get()->getPinBorderColor();
    static COLORREF cust[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = win ? win->hwnd : nullptr;
    cc.lpCustColors = cust;
    cc.rgbResult = RGB((cur >> 24) & 0xFFu, (cur >> 16) & 0xFFu, (cur >> 8) & 0xFFu);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return;
    const uint32_t v = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 24)
        | (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 16)
        | (static_cast<uint32_t>(GetBValue(cc.rgbResult)) << 8) | 0xFFu;
    Setting::get()->setPinBorderColor(v);
    if (pinColorSwatch) { pinColorSwatch->setBg(v); pinColorSwatch->setHoverBg(v); }
    if (pinColorHex) {
        wchar_t buf[16];
        swprintf_s(buf, L"#%02X%02X%02X", (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF);
        pinColorHex->setText(buf);
    }
    refreshPinPreview();
}

// ---- 托盘图标卡（开关 + 5 样式预览 + 单选 + 上传） ----
void WinSettingAppearance::initTrayCtrls(Ling::Node* p)
{
    auto* card = SettingUi::card(p);
    card->setBorderRadius(SettingTheme::radiusRow);   // 与本页「软件主题/软件语言」卡片同款大圆角(14)

    // 标题行：标题 + ? + 右侧开关（是否显示系统托盘）
    auto* head = card->makeChild<Ling::Node>();
    head->setFlexDirection(Ling::FlexDirection::Row);
    head->setAlignItems(Ling::Align::Center);
    head->setWidthPercent(100.f);
    head->setMarginBottom(SettingTheme::blockGap);
    auto* title = head->makeChild<Ling::Label>();
    title->setText(Lang::get(L"setting.trayIcon"));
    title->setFontSize(13.f);
    title->setColor(SettingTheme::textPrimary);
    SettingUi::helpTipButton(head, Lang::get(L"setting.trayTip"));
    auto* headGrow = head->makeChild<Ling::Node>();
    headGrow->setFlexGrow(1.f);
    auto* sw = head->makeChild<Ling::Button>();
    SettingUi::styleToggle(sw, Setting::get()->getTrayEnabled());
    sw->onClick.add([sw](Ling::Button*) {
        const bool on = !Setting::get()->getTrayEnabled();
        Setting::get()->setTrayEnabled(on);
        SettingUi::styleToggle(sw, on);
        TrayIcon::apply();
        });

    auto* grid = card->makeChild<Ling::Node>();
    grid->setFlexDirection(Ling::FlexDirection::Column);
    grid->setWidthPercent(100.f);

    static const wchar_t* kKeys[4] = {
        L"setting.trayFollow", L"setting.trayTheme",
        L"setting.trayCustomColor", L"setting.trayCustomIcon"
    };

    // 单选外圈（实心圆）+ 白色内圆成环 + 中心绿点（与工具栏主题同一套做法，无描边噪点）
    auto makeRadio = [this](Ling::Node* parent, int idx) {
        auto* r = parent->makeChild<Ling::Button>();
        r->setSize(16.f, 16.f);
        r->setBorderRadius(SettingTheme::radiusFull);
        r->setBg(SettingTheme::input);
        r->setHoverBg(SettingTheme::input);
        r->setAlignItems(Ling::Align::Center);
        r->setJustifyContent(Ling::Justify::Center);
        r->setFlexShrink(0.f);
        auto* hole = r->makeChild<Ling::Node>();
        hole->setSize(12.f, 12.f);
        hole->setBorderRadius(SettingTheme::radiusFull);
        hole->setBg(SettingTheme::card);
        hole->setAlignItems(Ling::Align::Center);
        hole->setJustifyContent(Ling::Justify::Center);
        hole->setFlexShrink(0.f);
        auto* dot = hole->makeChild<Ling::Node>();
        dot->setSize(6.f, 6.f);
        dot->setBorderRadius(SettingTheme::radiusFull);
        dot->setBg(0);
        dot->setFlexShrink(0.f);
        r->onClick.add([this, idx](Ling::Button*) {
            if (idx == 3 && Setting::get()->getTrayCustomIconPath().empty()) {
                this->pickTrayIcon();   // 还没有图 → 直接弹选文件
                return;
            }
            Setting::get()->setTrayIconStyle(idx);
            TrayIcon::apply();
            this->refreshTrayRadios();
            this->refreshTrayPreviews();
            });
        this->trayRadios[idx] = r;
        this->trayDots[idx] = dot;
        };

    for (int rowIdx = 0; rowIdx < 2; rowIdx++) {
        auto* row = grid->makeChild<Ling::Node>();
        row->setFlexDirection(Ling::FlexDirection::Row);
        row->setWidthPercent(100.f);
        if (rowIdx < 1) row->setMarginBottom(8.f);
        for (int colIdx = 0; colIdx < 2; colIdx++) {
            const int i = rowIdx * 2 + colIdx;
            if (i > 3) break;
            auto* cell = row->makeChild<Ling::Node>();
            cell->setFlexGrow(1.f);
            cell->setFlexShrink(1.f);
            cell->setWidthPercent(0.f);
            if (colIdx == 1) cell->setMarginLeft(8.f);
            cell->setFlexDirection(Ling::FlexDirection::Row);
            cell->setAlignItems(Ling::Align::Center);
            cell->setPadding(8.f, 8.f, 12.f, 8.f);          // 内边距 (8,8,12,8)
            cell->setBorderRadius(SettingTheme::radiusCtl); // 8
            cell->setBorder(1.f, SettingTheme::border);

            // 图标槽：内置样式 = 上色 Logo 字形（字形本身自带方框，不再叠描边，免得显得又小又糊）
            auto* iconBox = cell->makeChild<Ling::Node>();
            iconBox->setSize(34.f, 34.f);
            iconBox->setAlignItems(Ling::Align::Center);
            iconBox->setJustifyContent(Ling::Justify::Center);
            iconBox->setFlexShrink(0.f);
            iconBox->setMarginRight(8.f);

            if (i == 2) {
                // 自定义颜色：图标本身可点 → 弹系统取色器
                auto* cb = iconBox->makeChild<Ling::Button>();
                cb->setSize(34.f, 34.f);
                cb->setText(Icon::Logo);
                cb->setFontFamily(Icon::Family);
                cb->setFontSize(34.f);
                cb->setColor(Setting::get()->getTrayCustomColor());
                cb->setHoverColor(Setting::get()->getTrayCustomColor());
                cb->setBg(0);
                cb->setHoverBg(0);
                cb->setFlexShrink(0.f);
                cb->onClick.add([this](Ling::Button*) { this->pickTrayColor(); });
                this->trayColorGlyph = cb;
            }
            else if (i == 3) {
                auto* img = iconBox->makeChild<Ling::ImageBox>();
                img->setSize(32.f, 32.f);
                img->setBorderRadius(SettingTheme::radiusInner);
                img->setBorder(1.f, SettingTheme::border);
                img->setFlexShrink(0.f);
                img->hide();
                this->trayCustomImg = img;
                auto* up = iconBox->makeChild<Ling::Button>();
                up->setSize(34.f, 34.f);
                up->setText(Lang::get(L"setting.trayUpload"));
                up->setFontSize(10.f);
                up->setColor(SettingTheme::textMuted);
                up->setBg(SettingTheme::secondary);
                up->setHoverBg(SettingTheme::secondary);
                up->setBorderRadius(SettingTheme::radiusInner);
                up->setBorder(1.f, SettingTheme::border);
                up->setFlexShrink(0.f);
                up->onClick.add([this](Ling::Button*) { this->pickTrayIcon(); });
                this->trayUploadBtn = up;
            }
            else {
                uint32_t tint = 0;
                TrayIcon::tintFor(i, tint);
                auto* g = iconBox->makeChild<Ling::Label>();
                g->setText(Icon::Logo);
                g->setFontFamily(Icon::Family);
                g->setFontSize(34.f);   // 撑满 34px 槽（字形 em 盒留白，20 显得比系统图标小）
                g->setColor(tint);
                g->setFlexShrink(0.f);
            }

            auto* lab = cell->makeChild<Ling::Label>();
            lab->setText(Lang::get(kKeys[i]));
            lab->setFontSize(13.f);
            lab->setColor(SettingTheme::textPrimary);
            lab->setFlexShrink(1.f);
            if (i == 3) SettingUi::helpTipButton(cell, Lang::get(L"setting.trayUploadTip"));
            auto* grow = cell->makeChild<Ling::Node>();
            grow->setFlexGrow(1.f);

            makeRadio(cell, i);
        }
    }

    refreshTrayRadios();
    refreshTrayPreviews();
}

void WinSettingAppearance::refreshTrayRadios()
{
    constexpr uint32_t kGreen = 0x34C759FFu;
    const int cur = Setting::get()->getTrayIconStyle();
    for (int i = 0; i < 4; i++) {
        if (!trayRadios[i]) continue;
        const bool on = (i == cur);
        const uint32_t ring = on ? kGreen : SettingTheme::input;
        trayRadios[i]->setBg(ring);
        trayRadios[i]->setHoverBg(ring);
        if (trayDots[i]) trayDots[i]->setBg(on ? kGreen : 0u);
    }
}

void WinSettingAppearance::refreshTrayPreviews()
{
    // 「自定义颜色」格：字形染成用户所选颜色
    if (trayColorGlyph) {
        const uint32_t c = Setting::get()->getTrayCustomColor();
        trayColorGlyph->setColor(c);
        trayColorGlyph->setHoverColor(c);
    }
    // 「自定义图标」格：有图显示图，没图显示「上传」
    if (!trayCustomImg || !trayUploadBtn) return;
    const std::wstring path = Setting::get()->getTrayCustomIconPath();
    const bool has = !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (has) {
        trayCustomImg->loadImg(path);
        trayCustomImg->show();
        trayUploadBtn->hide();
    }
    else {
        trayCustomImg->hide();
        trayUploadBtn->show();
    }
}

void WinSettingAppearance::pickTrayColor()
{
    const uint32_t cur = Setting::get()->getTrayCustomColor();
    static COLORREF cust[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = win ? win->hwnd : nullptr;
    cc.lpCustColors = cust;
    cc.rgbResult = RGB((cur >> 24) & 0xFFu, (cur >> 16) & 0xFFu, (cur >> 8) & 0xFFu);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return;
    const uint32_t v = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 24)
        | (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 16)
        | (static_cast<uint32_t>(GetBValue(cc.rgbResult)) << 8) | 0xFFu;
    Setting::get()->setTrayCustomColor(v);
    Setting::get()->setTrayIconStyle((int)TrayIcon::Style::CustomColor);   // 选色即切到「自定义颜色」
    TrayIcon::apply();
    refreshTrayRadios();
    refreshTrayPreviews();
}

void WinSettingAppearance::pickTrayIcon()
{
    static const COMDLG_FILTERSPEC kFilter[]{
        { L"图片 (*.png;*.jpg;*.bmp;*.gif;*.ico)", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico" }
    };
    std::wstring path = win->openFileDialog(kFilter);
    if (path.empty()) return;
    Setting::get()->setTrayCustomIconPath(path);
    Setting::get()->setTrayIconStyle((int)TrayIcon::Style::CustomIcon);   // 上传即切到「自定义图标」
    TrayIcon::apply();
    refreshTrayRadios();
    refreshTrayPreviews();
}

void WinSettingAppearance::setAutoStartBtn(Ling::Button* btn)
{
    auto setting = Setting::get();
    auto isAutoStart = setting->getAutoStart();
    if (isAutoStart) {
        btn->setText(Icon::Done);
        btn->setColor(SettingTheme::brand);
        btn->setHoverColor(SettingTheme::brand);
    }
    else {
        btn->setText(Icon::Cancel);
        btn->setColor(SettingTheme::textMuted);
        btn->setHoverColor(SettingTheme::textMuted);
    }
}

void WinSettingAppearance::hideSelectBox()
{
    if (!selectPopup) return;
    deferPopupReset(selectPopup);   // 延迟销毁，避免在钩子/事件回调内重入析构
}

void WinSettingAppearance::showSelectBox(Ling::Button* btn)
{
    auto weakThis = getWeakThis();
    if (selectPopup) selectPopup.reset();

    auto langs = Lang::get()->getSupportedLang();
    auto itemH{ 30.f };
    const float itemGap = 4.f;   // 选项之间 4px 间距
    const float itemN = (float)(langs.size() + 1);   // 语言项 + 末尾「获取更多语言」
    auto totalH = std::min(320.f, itemH * itemN + itemGap * std::max(0.f, itemN - 1.f));

    // 独立 WS_POPUP 弹层：宽度取锚定按钮宽度，不再被宿主客户区裁剪 / 底部上拉规避。
    auto pw = btn->w / win->dpi;
    selectPopup = std::make_unique<SettingUi::Popup>(win, pw, totalH);
    auto* box = selectPopup->box();
    box->setBg(SettingTheme::popover);
    box->setBorder(1.f, SettingTheme::border);
    box->setBorderRadius(SettingTheme::radius);
    box->setPadding(4.f, 4.f, 4.f, 4.f);
    auto curName = selectBtn->getText();
    for (auto& pair : langs)
    {
        auto it = box->makeChild<Ling::Button>();
        it->setText(pair.first);
        it->setHeight(itemH);
        it->setWidthPercent(100.f);
        it->setFlexDirection(Ling::FlexDirection::Row);
        it->setJustifyContent(Ling::Justify::Start);
        it->setAlignItems(Ling::Align::Center);
        it->setPadding(10.f, 0, 10.f, 0);
        it->setBorderRadius(SettingTheme::radiusInner);   // 内层小框：与外框(10)同心
        it->setMarginBottom(itemGap);   // 与后一项（含末尾「获取更多语言」）之间留 4px
        bool isCurrent = (pair.first == curName);
        // 当前项：底色取与其它项 hover 相同的浅灰，hover 底色/文字色取同值 → 悬停不产生任何变化。
        it->setColor(SettingTheme::textPrimary);
        it->setBg(isCurrent ? SettingTheme::accent : 0);
        it->setHoverBg(SettingTheme::accent);
        if (isCurrent) it->setHoverColor(SettingTheme::textPrimary);
        it->onClick.add([this, weakThis](Ling::Button* b) {
            if (!weakThis.lock()) return;
            auto lang = Lang::get();
            auto langName = b->getText();
            auto langs = lang->getSupportedLang();
            for (auto& pair : langs)
            {
                if (pair.first == langName) {
                    Setting::get()->setLang(pair.second);
                    this->hideSelectBox();
                    win->close();
                    Ling::App::get()->dq.TryEnqueue([this]() {
                        WinSetting::init();
                        });
                    break;
                }
            }
            });
    }
    auto lastItem = box->makeChild<Ling::Button>();
    lastItem->setText(Lang::get(L"setting.getMoreLang"));
    lastItem->setHeight(itemH);
    lastItem->setWidthPercent(100.f);
    lastItem->setFlexDirection(Ling::FlexDirection::Row);
    lastItem->setJustifyContent(Ling::Justify::Start);
    lastItem->setAlignItems(Ling::Align::Center);
    lastItem->setPadding(10.f, 0, 10.f, 0);
    lastItem->setBorderRadius(SettingTheme::radiusInner);   // 内层小框：与外框(10)同心
    lastItem->setHoverBg(SettingTheme::accent);
    lastItem->setHoverColor(SettingTheme::textPrimary);
    lastItem->setColor(SettingTheme::textSecondary);
    lastItem->onClick.add([this, weakThis](Ling::Button*) {
        if (!weakThis.lock()) return;
        this->hideSelectBox();
        std::wstring downloadUrl{ L"https://github.com/CM-idea/SnowAir/tree/main/Native/Lang" };
        ShellExecute(win->hwnd, L"open", downloadUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
        });
    selectPopup->setAnchor(btn);
    selectPopup->onDismiss = [this]() { hideSelectBox(); };
    selectPopup->open();
}

// ==================== 工具栏编辑器 ====================

void WinSettingAppearance::tbSetKind(int kind)
{
    if (kind < 0 || kind > ToolbarStore::kindCount - 1 || kind == tbKind) return;
    tbKind = kind;
    auto st = ToolbarStore::loadLayout(kind);
    tbActive = st.active;
    tbInactive = st.inactive;
    tbRebuildZones();
}

void WinSettingAppearance::tbRebuildZones()
{
    if (!tbActiveBox || !tbInactiveBox) return;
    tbHideChipClose();   // chip 重建：先收起旧的叉（它引用的 chip 即将销毁）
    tbBuildChips(tbActiveBox, tbActive, true);
    tbBuildChips(tbInactiveBox, tbInactive, false);
    tbRefreshRestore();
    tbRefreshPreview();
}

void WinSettingAppearance::tbPersist()
{
    ToolbarStore::saveLayout(tbKind, ToolbarStore::LayoutState{ tbActive, tbInactive });
}

void WinSettingAppearance::tbRefreshRestore()
{
    if (!tbRestoreBtn) return;
    auto def = ToolbarStore::defaultLayout(tbKind);
    bool isDef = (tbActive == def.active && tbInactive == def.inactive);
    tbRestoreBtn->setColor(isDef ? SettingTheme::textMuted : SettingTheme::textPrimary);
    tbRestoreBtn->setHoverColor(isDef ? SettingTheme::textMuted : SettingTheme::textPrimary);
}

void WinSettingAppearance::tbRestoreDefaults()
{
    ToolbarStore::restoreDefaults(tbKind);
    tbSetKind(tbKind);
    // 强制刷新（tbSetKind 里 kind==tbKind 会直接 return）
    auto st = ToolbarStore::loadLayout(tbKind);
    tbActive = st.active;
    tbInactive = st.inactive;
    tbRebuildZones();
}

void WinSettingAppearance::tbTogglePreview()
{
    if (!tbPreviewWrap) return;
    tbPreviewShown = !tbPreviewShown;
    if (tbPreviewShown) tbPreviewWrap->show();
    else tbPreviewWrap->hide();
    tbRefreshPreview();
}

void WinSettingAppearance::tbRefreshPreview()
{
    if (!tbPreviewWrap) return;
    tbPreviewWrap->removeAllChildren();
    if (!tbPreviewShown) return;
    ToolbarTheme::refresh();   // 预览条与实际工具栏共用同一套运行时色板
    // 预览条**镜像实际工具栏**（同一套 ToolbarTheme 常量）——所见即所得：
    // 实际是白底就画白底，另加 1px 描边（白底叠在白色卡片上才看得出边界）。
    auto* bar = tbPreviewWrap->makeChild<Ling::Node>();
    bar->setFlexDirection(Ling::FlexDirection::Row);
    bar->setAlignItems(Ling::Align::Center);
    bar->setWidthPercent(100.f);
    bar->setBg(ToolbarTheme::background);
    bar->setBorder(ToolbarTheme::borderWidth, ToolbarTheme::border);
    bar->setBorderRadius(ToolbarTheme::borderRadius);
    bar->setPadding(6.f, 6.f, 6.f, 6.f);
    bar->setMarginBottom(SettingTheme::blockGap);
    // 最左：拖拽手柄（与实际工具栏一致：半透明手柄、无悬停灰底）
    {
        auto* dh = bar->makeChild<Ling::Button>();
        dh->setSize(30.f, 30.f);
        dh->setText(Icon::DragHandle);
        dh->setFontFamily(Icon::Family);
        dh->setFontSize(18.f);
        dh->setColor(ToolbarTheme::dragHandleColor);
        dh->setHoverColor(ToolbarTheme::dragHandleColor);
        dh->setHoverBg(0);
        dh->setMarginRight(6.f);
        dh->setFlexShrink(0.f);
    }
    for (auto& id : tbActive) {
        if (tbIsSplitId(id)) {
            auto* s = bar->makeChild<Ling::Node>();
            s->setSize(1.f, 22.f);
            s->setBg(ToolbarTheme::splitter);
            s->setMargin(5.f, 0.f, 5.f, 0.f);
            continue;
        }
        auto* b = bar->makeChild<Ling::Button>();
        b->setSize(30.f, 30.f);
        b->setBorderRadius(SettingTheme::radius);
        b->setBg(0);
        b->setHoverBg(ToolbarTheme::hoverBg);
        b->setMarginRight(2.f);
        // 图标配色同实际工具栏：取消=红，确认=绿，其余=主题图标色（正常/停悬两态）
        const bool actCancel = (id == L"cancel" || id == L"close");
        const bool actDone = (id == L"confirm" || id == L"clipboard");
        b->setColor(actCancel ? Icon::ColorCancel : actDone ? Icon::ColorDone : ToolbarTheme::iconNormal);
        b->setHoverColor(actCancel ? Icon::ColorCancel : actDone ? Icon::ColorDone : ToolbarTheme::iconNormal);
        if (id == L"record-time") {
            b->setText(L"00:00");
            b->setFontSize(10.f);
        }
        else {
            auto ic = ToolbarStore::iconOf(id);
            b->setText(ic.empty() ? L"·" : ic);
            b->setFontFamily(Icon::Family);
            b->setFontSize(18.f);
        }
    }
}

Ling::Button* WinSettingAppearance::tbMakeChip(const std::wstring& id, bool active)
{
    auto* box = active ? tbActiveBox : tbInactiveBox;
    auto* b = box->makeChild<Ling::Button>();
    bool split = tbIsSplitId(id);
    b->setMargin(0.f, 0.f, 6.f, 6.f);
    b->setBorderRadius(SettingTheme::radiusSm);
    if (split) {
        // 分隔符：方形 chip + 居中的 1px 竖线（自绘，水平居中 → 悬停底色左右对称）
        b->setSize(16.f, 36.f);
        auto* line = b->makeChild<Ling::Node>();
        line->setPositionType(Ling::Position::Absolute);
        line->setPosition(Ling::Edge::Left, 7.5f);   // (16-1)/2：精确居中
        line->setPosition(Ling::Edge::Top, 8.f);
        line->setSize(1.f, 20.f);
        line->setBg(0x33333359u);                    // #333 @ 35%
    }
    else {
        // 工具图标统一方形
        b->setSize(36.f, 36.f);
        if (id == L"record-time") {
            b->setText(L"00:00");
            b->setFontSize(10.f);
        }
        else {
            auto ic = ToolbarStore::iconOf(id);
            b->setText(ic.empty() ? L"·" : ic);
            b->setFontFamily(Icon::Family);
            b->setFontSize(Icon::SizeSm);
        }
    }
    tbStyleChip(b, id, active);
    // 悬停文字提示
    if (tbTip_) {
        std::wstring tip = tbTipText(id);
        if (!tip.empty()) tbTip_->bind(b, tip);
    }
    // 显示区可移除的工具：悬停时右上角浮出一个红叉。
    // 叉**不能挂在 chip 下**（chip 有圆角 clip，会把伸到角外的部分裁掉），
    // 所以用页级共享的 tbChipClose，由这里在悬停时把它挪到本 chip 的角上。
    if (active && !split && !tbLocked(id) && id != L"record-time") {
        b->onEnter.add([this, b, id](Ling::Button*) { this->tbShowChipClose(b, id); });
        b->onLeave.add([this](Ling::Button*) { this->tbHideChipClose(); });
    }
    return b;
}

// 把共享红叉移到 chip 的右上角外侧（跨在角上）
void WinSettingAppearance::tbShowChipClose(Ling::Button* chip, const std::wstring& id)
{
    if (!tbChipClose || !chip) return;
    tbChipCloseId = id;
    const float scrollY = tbScroller ? tbScroller->getScrollY() : 0.f;
    // chip->x/y 是"未滚动"的窗口绝对坐标；本页内坐标系要减去页原点（并补回滚动偏移）
    // 贴紧右上角：只外伸 4px（14px 圆点 → 10px 压在图标上、4px 探出去），别悬得太远
    const float chipRight = chip->x + chip->w;
    tbChipClose->setPosition(Ling::Edge::Left, chipRight - this->x - 10.f);
    tbChipClose->setPosition(Ling::Edge::Top, chip->y - this->y + scrollY - 4.f);
    // 提到最前：chip 是在叉之后创建的，不提升会被 chip 盖住
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (it->get() == tbChipClose) {
            auto owned = std::move(*it);
            children.erase(it);
            children.push_back(std::move(owned));
            break;
        }
    }
    tbChipClose->show();
    win->refresh();
}

void WinSettingAppearance::tbHideChipClose()
{
    if (tbChipClose) tbChipClose->hide();
}

// chip 悬停文字
std::wstring WinSettingAppearance::tbTipText(const std::wstring& id) const
{
    if (id == L"record-time") return L"录制时长";
    if (tbIsSplitId(id)) return (id == L"split-extra") ? L"拖入工具栏添加分隔符" : L"分隔符";
    struct M { const wchar_t* id; const wchar_t* key; };
    static const M kMap[] = {
        { L"show-cursor", L"tool.showCursor" },
        { L"rect", L"tool.rect" },
        { L"ellipse", L"tool.ellipse" },
        { L"arrow", L"tool.arrow" },
        { L"pen", L"tool.pen" },
        { L"text", L"tool.text" },
        { L"serial-number", L"tool.number" },
        { L"mosaic", L"tool.mosaic" },
        { L"eraser", L"tool.eraser" },
        { L"undo", L"tool.undo" },
        { L"extra", L"cap.video" },
        { L"fixed", L"tool.pin" },
        { L"ocr", L"cap.ocr" },
        { L"translate", L"tool.translate" },
        { L"scroll-screenshot", L"cap.long" },
        { L"save", L"tool.save" },
        { L"cancel", L"tool.cancel" },
        { L"confirm", L"tool.clipboard" },
        { L"video-play", L"video.startRecord" },
        { L"video-audio", L"video.recordSystem" },
        { L"video-mic", L"video.recordMic" },
        { L"video-gif", L"video.outputGif" },
        { L"laser-pointer", L"tool.fadePen" },
        { L"reset-canvas", L"tool.resetCanvas" },
        { L"mouse-through", L"tool.mouseThrough" },
        { L"show-border", L"pin.showBorder" },
    };
    for (const auto& m : kMap) if (id == m.id) return Lang::get(m.key);
    return {};
}

// 拖拽幽灵：跟手显示被拖的图标（绝对定位在本页内；坐标从窗口客户区换算到页内）
void WinSettingAppearance::tbShowGhost(const std::wstring& id, POINT pos)
{
    if (!tbGhost || !tbGhostIcon) return;
    if (tbIsSplitId(id)) {
        tbGhostIcon->setText(L"｜");
        tbGhostIcon->setFontFamily(L"");
        tbGhostIcon->setFontSize(14.f);
    }
    else if (id == L"record-time") {
        tbGhostIcon->setText(L"00:00");
        tbGhostIcon->setFontFamily(L"");
        tbGhostIcon->setFontSize(10.f);
    }
    else {
        auto ic = ToolbarStore::iconOf(id);
        tbGhostIcon->setText(ic.empty() ? L"·" : ic);
        tbGhostIcon->setFontFamily(Icon::Family);
        tbGhostIcon->setFontSize(Icon::SizeSm);
    }
    const float scrollY = tbScroller ? tbScroller->getScrollY() : 0.f;
    tbGhost->setPosition(Ling::Edge::Left, (float)pos.x - this->x - 18.f);
    tbGhost->setPosition(Ling::Edge::Top, (float)pos.y - this->y + scrollY - 18.f);
    tbGhost->show();
    win->refresh();
}

void WinSettingAppearance::tbHideGhost()
{
    if (tbGhost) {
        tbGhost->hide();
        win->refresh();
    }
}

void WinSettingAppearance::tbStyleChip(Ling::Button* b, const std::wstring& id, bool active)
{
    bool split = tbIsSplitId(id);
    bool locked = active && tbLocked(id);
    if (split) {
        b->setBorder(0, 0);
        b->setBg(0);
        b->setColor(SettingTheme::textTertiary);
    }
    else {
        b->setBorder(1.f, locked ? SettingTheme::input : SettingTheme::border);
        b->setBg(locked ? SettingTheme::secondary : SettingTheme::cardBg);
        // 取消=红、完成=绿（同实际工具栏与预览条），其余常规灰
        b->setColor((id == L"cancel" || id == L"close") ? Icon::ColorCancel
            : (id == L"confirm" || id == L"clipboard") ? Icon::ColorDone
            : (locked ? SettingTheme::textMuted : SettingTheme::textPrimary));
    }
    b->setHoverBg(SettingTheme::accent);
    b->setHoverColor(SettingTheme::textPrimary);
}

void WinSettingAppearance::tbBuildChips(Ling::Node* box,
    const std::vector<std::wstring>& ids, bool active)
{
    box->removeAllChildren();
    auto& list = active ? tbChips : tbHidden;
    list.clear();
    for (auto& id : ids) {
        list.push_back(tbMakeChip(id, active));
    }
    // 隐藏区：只要没有"分隔线以外"的工具出现，就显示居中的预留提示文字
    if (!active) {
        bool anyTool = false;
        for (auto& id : ids) if (!tbIsSplitId(id)) { anyTool = true; break; }
        if (!anyTool) {
            auto* hint = box->makeChild<Ling::Label>();
            hint->setText(Lang::get(L"setting.tbDropHint"));
            hint->setFontSize(12.f);
            hint->setColor(SettingTheme::textMuted);
            hint->setFlexGrow(1.f);
            hint->setJustifyContent(Ling::Justify::Center);
            hint->setMarginLeft(8.f);
        }
    }
    // 插入位置光标：蓝色竖条，拖拽时显示；绝对定位，不参与 flex 布局
    auto* cursor = box->makeChild<Ling::Node>();
    cursor->setPositionType(Ling::Position::Absolute);
    cursor->setSize(2.f, 28.f);
    cursor->setBg(0x34C759FF);   // 绿色插入提示（0x34C759）
    cursor->setBorderRadius(SettingTheme::radius);
    cursor->hide();
    if (active) tbCursorActive = cursor; else tbCursorInactive = cursor;
}

void WinSettingAppearance::tbOnDown(POINT pos, bool right)
{
    tbDragging = false;
    tbDragId.clear();
    if (right || !tbScroller) return;
    // 其它弹层打开时（如类型下拉）不响应 chip
    if (SettingUi::hasPopup()) return;
    POINT c = tbToContent(pos);
    tbDragChip = nullptr;
    for (size_t i = 0; i < tbChips.size(); i++) {
        if (tbChips[i] && tbChips[i]->isPosIn(c)) {
            tbDragId = (i < tbActive.size()) ? tbActive[i] : std::wstring{};
            tbDragFrom = 0;
            tbDragChip = tbChips[i];
            break;
        }
    }
    if (tbDragId.empty()) {
        for (size_t i = 0; i < tbHidden.size(); i++) {
            if (tbHidden[i] && tbHidden[i]->isPosIn(c)) {
                tbDragId = (i < tbInactive.size()) ? tbInactive[i] : std::wstring{};
                tbDragFrom = 1;
                tbDragChip = tbHidden[i];
                break;
            }
        }
    }
    if (tbDragId.empty()) return;
    if (tbLocked(tbDragId)) { tbDragId.clear(); tbDragChip = nullptr; return; }
    tbDragStart = pos;
    tbDragLast = pos;
    tbDragTo = -1;
    tbDropIndex = -1;
    tbLiveDropTo = -1;
    tbLiveDropIndex = -1;
    tbHideGhost();
    tbHideChipClose();
    if (!tbDragHooked) {
        auto weakTb = getWeakThis();
        tbMoveTok = win->onMouseMove.add([this, weakTb](POINT p) {
            if (!weakTb.lock()) return;
            this->tbOnMove(p);
            });
        tbUpTok = win->onMouseUp.add([this, weakTb](POINT p, bool r) {
            if (!weakTb.lock()) return;
            this->tbOnUp(p, r);
            });
        tbDragHooked = true;
    }
}

void WinSettingAppearance::tbOnMove(POINT pos)
{
    if (tbDragId.empty()) return;
    tbDragLast = pos;
    if (!tbDragging) {
        int dx = std::abs(pos.x - tbDragStart.x);
        int dy = std::abs(pos.y - tbDragStart.y);
        if (dx + dy > 6) {
            tbDragging = true;   // 拖拽时原 chip 保持原样（不再加粗描边/淡蓝底，跟手幽灵已足够）
        }
    }
    if (!tbDragging) return;
    // 实时计算落点（区 + 插入位），并高亮目标插槽
    POINT c = tbToContent(pos);
    tbLiveDropTo = tbHitZone(c, tbDragFrom);
    tbLiveDropIndex = tbComputeDropIndex(c, tbLiveDropTo);
    tbUpdateDropHint();
    // 跟手的拖拽幽灵（图标随光标走）
    tbShowGhost(tbDragId, pos);
}

int WinSettingAppearance::tbComputeDropIndex(POINT c, int toZone) const
{
    const auto& chips = (toZone == 0) ? tbChips : tbHidden;
    int index = 0;
    if (!chips.empty()) {
        int best = 0;
        float bestDy = 1e9f;
        for (size_t i = 0; i < chips.size(); i++) {
            if (!chips[i]) continue;
            float dy = std::fabs((chips[i]->y + chips[i]->h / 2.f) - c.y);
            if (dy < bestDy) { bestDy = dy; best = (int)i; }
        }
        float rowY = chips[best] ? chips[best]->y : 0.f;
        float rowH = chips[best] ? chips[best]->h : 0.f;
        index = (int)chips.size();
        for (size_t i = 0; i < chips.size(); i++) {
            if (!chips[i]) continue;
            if (std::fabs(chips[i]->y - rowY) >= rowH / 2.f) continue;
            if (c.x < chips[i]->x + chips[i]->w / 2.f) { index = (int)i; break; }
        }
    }
    // 隐藏区与 tbDrop 保持一致：工具恒落在模板分隔符之后（索引 >= 1）
    if (toZone == 1) index = std::max(1, index);
    return index;
}

void WinSettingAppearance::tbUpdateDropHint()
{
    // 先隐藏上一次的光标
    if (tbCursorActive) tbCursorActive->hide();
    if (tbCursorInactive) tbCursorInactive->hide();
    const auto& chips = (tbLiveDropTo == 0) ? tbChips : tbHidden;
    auto* cursor = (tbLiveDropTo == 0) ? tbCursorActive : tbCursorInactive;
    auto* hostBox = (tbLiveDropTo == 0) ? tbActiveBox : tbInactiveBox;
    if (!cursor || !hostBox) return;
    int idx = tbLiveDropIndex;
    if (idx < 0) return;
    // 在插入位（该 chip 左侧 / 列表末尾）显示绿色竖条，高度与图标行对齐。
    // chip->x/y 是"未滚动"的窗口绝对坐标，而 cursor 是 hostBox 的子节点（绝对定位相对 hostBox），
    // 所以这里必须减去 hostBox 的原点 → 之前直接用绝对值，竖条被挪到了容器外（看不见）。
    float x = hostBox->x, y = hostBox->y;
    if (!chips.empty()) {
        if (idx >= (int)chips.size()) {
            auto* last = chips.back();
            if (!last) return;
            x = last->x + last->w + 3.f;
            y = last->y;
        }
        else {
            auto* c = chips[idx];
            if (!c) return;
            x = c->x - 3.f;
            y = c->y;
        }
    }
    cursor->setPosition(Ling::Edge::Left, x - hostBox->x);
    cursor->setPosition(Ling::Edge::Top, y - hostBox->y + 4.f);
    cursor->setHeight(28.f);
    cursor->show();
}

void WinSettingAppearance::tbClearDropHint()
{
    // 隐藏插入光标 + 恢复被拖动 chip 的原始样式（此时 chip 仍存活，可安全改样式）
    if (tbCursorActive) tbCursorActive->hide();
    if (tbCursorInactive) tbCursorInactive->hide();
    tbHideGhost();
    if (tbDragChip && !tbDragId.empty()) {
        tbStyleChip(tbDragChip, tbDragId, tbDragFrom);
    }
    tbDragChip = nullptr;
    tbLiveDropTo = -1;
    tbLiveDropIndex = -1;
}

void WinSettingAppearance::tbOnUp(POINT pos, bool right)
{
    if (tbDragId.empty()) return;
    bool wasDrag = tbDragging;
    int fromZone = tbDragFrom;
    if (tbDragHooked) {
        win->onMouseMove.remove(tbMoveTok);
        win->onMouseUp.remove(tbUpTok);
        tbDragHooked = false;
    }
    // 先清掉拖拽期间的插入高亮 / "被拿起"样式（chip 仍存活，安全），再走后续逻辑
    tbClearDropHint();
    if (right) { tbDragId.clear(); tbDragging = false; tbDragFrom = -1; return; }
    if (wasDrag) {
        POINT c = tbToContent(pos);
        int toZone = tbHitZone(c, fromZone);
        tbDragTo = toZone;
        tbDropIndex = tbComputeDropIndex(c, toZone);
        tbDrop();
    }
    else {
        tbToggleItem();
    }
}

void WinSettingAppearance::tbToggleItem()
{
    if (tbDragId.empty()) return;
    // 点击行为：跨区移动；显示区分隔符点击删除；隐藏区模板点击无操作
    if (tbDragFrom == 1 && tbIsSplitId(tbDragId)) {
        tbDragId.clear(); tbDragging = false; tbDragFrom = -1;
        return;
    }
    tbDragTo = 1 - tbDragFrom;
    tbDropIndex = (tbDragTo == 1) ? (int)tbInactive.size() : (int)tbActive.size();
    tbDrop();
}

void WinSettingAppearance::tbDrop()
{
    if (tbDragId.empty()) return;
    std::wstring id = tbDragId;
    int fromZone = tbDragFrom;
    int toZone = tbDragTo;
    int insertIndex = tbDropIndex;
    // 清拖拽态
    tbDragId.clear();
    tbDragging = false;
    tbDragFrom = -1;
    tbDragTo = -1;
    tbDropIndex = -1;

    if (toZone == 1 && tbLocked(id)) return;
    if (toZone == 1 && id == L"split-extra" && fromZone == 1) return;
    int dropIndex = insertIndex;
    if (toZone == 1 && id != L"split-extra") dropIndex = std::max(1, dropIndex);

    // 模板分隔符拖入显示区：复制新分隔符
    if (fromZone == 1 && id == L"split-extra" && toZone == 0) {
        std::wstring movingId = tbNextSplitId();
        dropIndex = std::clamp(dropIndex, 0, (int)tbActive.size());
        tbActive.insert(tbActive.begin() + dropIndex, movingId);
        tbInactive = tbNormalizeInactive(tbInactive);
        tbPersist();
        tbRebuildZones();
        return;
    }
    // 显示区分隔符拖到隐藏区：只删除
    if (fromZone == 0 && tbIsSplitId(id) && toZone == 1) {
        tbVecErase(tbActive, id);
        tbInactive = tbNormalizeInactive(tbInactive);
        tbPersist();
        tbRebuildZones();
        return;
    }

    auto& fromList = (fromZone == 0) ? tbActive : tbInactive;
    int fromIndex = -1;
    for (size_t i = 0; i < fromList.size(); i++) {
        if (fromList[i] == id) { fromIndex = (int)i; break; }
    }
    if (fromIndex < 0) return;
    fromList.erase(fromList.begin() + fromIndex);

    if (fromZone == toZone) {
        int insertAt = (fromIndex < dropIndex) ? dropIndex - 1 : dropIndex;
        insertAt = (toZone == 1) ? std::max(1, insertAt) : std::max(0, insertAt);
        insertAt = std::min(insertAt, (int)fromList.size());
        fromList.insert(fromList.begin() + insertAt, id);
        if (toZone == 1) tbInactive = tbNormalizeInactive(fromList);
    }
    else {
        auto& toList = (toZone == 0) ? tbActive : tbInactive;
        int insertAt = std::clamp(dropIndex, 0, (int)toList.size());
        if (toZone == 1) insertAt = std::max(1, insertAt);
        toList.insert(toList.begin() + insertAt, id);
        if (toZone == 1) tbInactive = tbNormalizeInactive(toList);
        else tbInactive = tbNormalizeInactive(fromList);
    }
    tbPersist();
    tbRebuildZones();
}

bool WinSettingAppearance::tbLocked(const std::wstring& id) const
{
    switch (tbKind) {
    case 0: return id == L"cancel" || id == L"confirm";
    case 1: return id == L"video-play" || id == L"video-pause" || id == L"record-time"
                || id == L"cancel" || id == L"confirm";
    case 2: return id == L"cancel";
    case 3: return id == L"cancel" || id == L"confirm";
    }
    return false;
}

bool WinSettingAppearance::tbIsSplit(const std::wstring& id)
{
    return tbIsSplitId(id);
}

std::wstring WinSettingAppearance::tbNextSplitId()
{
    int i = 0;
    while (tbVecHas(tbActive, L"split-" + std::to_wstring(i))) ++i;
    return L"split-" + std::to_wstring(i);
}

POINT WinSettingAppearance::tbToContent(POINT pos) const
{
    if (tbScroller) pos.y += (int)tbScroller->getScrollY();
    return pos;
}

int WinSettingAppearance::tbHitZone(POINT p, int fallback) const
{
    if (tbActiveBox && tbActiveBox->isPosIn(p)) return 0;
    if (tbInactiveBox && tbInactiveBox->isPosIn(p)) return 1;
    return fallback;
}

// ==================== 工具栏主题色板 ====================

void WinSettingAppearance::tbBuildTheme(Ling::Node* host)
{
    auto* card = SettingUi::card(host);
    auto* title = card->makeChild<Ling::Label>();
    title->setText(Lang::get(L"setting.tbTheme"));
    title->setFontSize(13.f);
    title->setColor(SettingTheme::textPrimary);
    title->setMarginBottom(SettingTheme::blockGap);

    const wchar_t* labels[] = {
        L"setting.tbThemeFollow", L"setting.tbThemeDark",
        L"setting.tbThemeLight", L"setting.tbThemeCustom"
    };
    // 2 列网格：4 个主题项各占一半宽度，紧凑排布
    auto* grid = card->makeChild<Ling::Node>();
    grid->setFlexDirection(Ling::FlexDirection::Column);
    grid->setWidthPercent(100.f);
    const float cellGap = 8.f;
    auto buildCell = [this, cellGap, &labels](Ling::Node* row, int i, const ToolbarStore::Colors& colors) {
        auto* cell = row->makeChild<Ling::Node>();
        cell->setFlexGrow(1.f);            // 两列等分剩余空间
        cell->setFlexShrink(1.f);          // 允许收缩：窄窗口时压缩到可用宽度，配合内部色块换行，不被固定最小宽度剪裁
        cell->setWidthPercent(0.f);        // 基础宽度归零，配合 flexGrow 让两列严格等宽而非按内容长短分配
        cell->setMarginLeft((i % 2 == 1) ? cellGap : 0.f);   // 列间留缝；等分布局下右列右缘正好贴着卡片内容右缘，不溢出
        cell->setFlexDirection(Ling::FlexDirection::Row);
        cell->setAlignItems(Ling::Align::Center);
        cell->setPadding(6.f, 8.f, 6.f, 8.f);
        cell->setBorderRadius(SettingTheme::radiusSm);
        cell->setBorder(1.f, SettingTheme::border);

        auto* swatches = cell->makeChild<Ling::Node>();
        swatches->setFlexDirection(Ling::FlexDirection::Row);
        swatches->setAlignItems(Ling::Align::Center);
        swatches->setFlexGrow(1.f);
        swatches->setFlexShrink(1.f);      // 允许收缩
        swatches->setFlexWrap(Ling::Wrap::Wrap);   // 色块空间不足时自动换行，避免溢出卡片被剪裁
        // 色块：外圈 = 描边色「实心圆」，内圆 = 色槽色「实心圆」→ 叠出 1px 圆环。
        // 不用 setBorder 描边：1px 描边外沿与圆角 clip 边缘重合会被二次抗锯齿，正是"灰描边噪点"的来源。
        for (int s = 0; s < 5; s++) {   // 5 个色槽：背景 / 停悬 / 图标 / 选中 / 框选
            const bool editable = (i == 3);
            Ling::Node* ring = nullptr;
            if (editable) {
                auto* sw = swatches->makeChild<Ling::Button>();
                sw->setHoverBg(SettingTheme::border);   // 悬停不变色
                int slot = s;
                sw->onClick.add([this, slot](Ling::Button*) { this->tbPickColor(slot); });
                // 悬停文字提示：告诉用户点的是哪个颜色（背景颜色 / 图标颜色 …），与 chip 提示同一套 Tip
                if (tbTip_) tbTip_->bind(sw, std::wstring(ToolbarStore::slotName(s)) + L"颜色");
                ring = sw;
            }
            else {
                ring = swatches->makeChild<Ling::Node>();
            }
            ring->setSize(16.f, 16.f);
            ring->setBorderRadius(SettingTheme::radiusFull);   // 正圆
            ring->setBg(SettingTheme::border);
            ring->setAlignItems(Ling::Align::Center);
            ring->setJustifyContent(Ling::Justify::Center);
            ring->setFlexShrink(0.f);
            ring->setMarginRight(4.f);

            auto* inner = ring->makeChild<Ling::Node>();
            inner->setSize(14.f, 14.f);
            inner->setBorderRadius(SettingTheme::radiusFull);
            inner->setBg(tbSlotColor(colors, s));
            inner->setFlexShrink(0.f);
            if (editable) tbCustomSwatches[s] = inner;
        }

        auto* lab = cell->makeChild<Ling::Label>();
        lab->setText(Lang::get(labels[i]));
        lab->setFontSize(12.f);
        lab->setColor(SettingTheme::textPrimary);
        lab->setFlexShrink(1.f);       // 允许收缩：窄窗口时随可用宽度收缩，避免被固定最小宽度剪裁
        lab->setMarginRight(8.f);

        // 单选按钮「大圆套小圆」：外层实心圆（环色）+ 中层白色实心圆（挖空成圆环）+ 中心实心圆点。
        // 全程实心圆、不用描边 → 无描边/clip 双重抗锯齿噪点；选中态整体绿色（外环 + 内点）。
        auto* radio = cell->makeChild<Ling::Button>();
        radio->setSize(16.f, 16.f);
        radio->setBorderRadius(SettingTheme::radiusFull);   // 正圆
        radio->setBg(SettingTheme::input);
        radio->setHoverBg(SettingTheme::input);
        radio->setAlignItems(Ling::Align::Center);
        radio->setJustifyContent(Ling::Justify::Center);
        radio->setFlexShrink(0.f);
        auto* hole = radio->makeChild<Ling::Node>();        // 白色内圆 → 与外圈叠成圆环
        hole->setSize(12.f, 12.f);
        hole->setBorderRadius(SettingTheme::radiusFull);
        hole->setBg(SettingTheme::card);
        hole->setAlignItems(Ling::Align::Center);
        hole->setJustifyContent(Ling::Justify::Center);
        hole->setFlexShrink(0.f);
        auto* dot = hole->makeChild<Ling::Node>();          // 中心点（选中=绿）
        dot->setSize(6.f, 6.f);
        dot->setBorderRadius(SettingTheme::radiusFull);
        dot->setBg(0);
        dot->setFlexShrink(0.f);
        int idx = i;
        radio->onClick.add([this, idx](Ling::Button*) {
            ToolbarStore::setModeId(idx);
            this->tbRefreshTheme();
            this->tbRefreshPreview();
            });
        tbThemeRadios[i] = radio;
        tbThemeDots[i] = dot;
    };

    // 两行 × 两列
    for (int row = 0; row < 2; row++) {
        auto* rowNode = grid->makeChild<Ling::Node>();
        rowNode->setFlexDirection(Ling::FlexDirection::Row);
        rowNode->setWidthPercent(100.f);
        if (row == 0) rowNode->setMarginBottom(cellGap);
        for (int col = 0; col < 2; col++) {
            int i = row * 2 + col;
            auto colors = (i == 0) ? ToolbarStore::systemPreset()
                : (i == 1)          ? ToolbarStore::darkPreset()
                : (i == 2)          ? ToolbarStore::lightPreset()
                                    : ToolbarStore::custom();
            buildCell(rowNode, i, colors);
        }
    }
    tbRefreshTheme();
}

void WinSettingAppearance::tbRefreshTheme()
{
    // 主题变了 → 把新色板刷进 ToolbarTheme（预览条与后续工具栏共用同一套运行时颜色）
    ToolbarTheme::refresh();
    constexpr uint32_t kGreen = 0x34C759FFu;   // 与主题 accent/选中绿一致
    const int cur = ToolbarStore::modeId();
    for (int i = 0; i < 4; i++) {
        if (!tbThemeRadios[i]) continue;
        const bool on = (i == cur);
        const uint32_t ring = on ? kGreen : SettingTheme::input;
        // 大圆套小圆：外圈实心圆 = 环色；中心点 = 绿（选中）/ 透明（未选）。
        // 注意：Button::setBg 在 isHover(鼠标悬停) 时只更新内部缓存、不写入 visual，
        // 故把 hover 态也设成同一颜色，点击瞬间(必然悬停)即立刻刷新出选中外观。
        tbThemeRadios[i]->setBg(ring);
        tbThemeRadios[i]->setHoverBg(ring);
        if (tbThemeDots[i]) tbThemeDots[i]->setBg(on ? kGreen : 0u);
    }
    auto c = ToolbarStore::custom();
    for (int s = 0; s < 5; s++) {
        if (!tbCustomSwatches[s]) continue;
        tbCustomSwatches[s]->setBg(tbSlotColor(c, s));
    }
}

void WinSettingAppearance::tbPickColor(int slot)
{
    if (slot < 0 || slot > 5) return;
    SettingUi::closePopup(win);   // 先收起可能打开的其它弹层

    // 直接调用系统颜色选择器（commdlg 的 ChooseColor）。
    // 应用内颜色是 RGBA(0xRRGGBBAA)，系统用 COLORREF(0x00BBGGRR)，这里互转（alpha 固定不透明）。
    const uint32_t curCol = tbSlotColor(ToolbarStore::custom(), slot);
    static COLORREF custColors[16] = {};   // 系统对话框的"自定义颜色"槽，进程内记忆
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = win ? win->hwnd : nullptr;
    cc.lpCustColors = custColors;
    cc.rgbResult = RGB((curCol >> 24) & 0xFFu, (curCol >> 16) & 0xFFu, (curCol >> 8) & 0xFFu);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return;   // 用户取消

    const uint32_t v = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 24)
        | (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 16)
        | (static_cast<uint32_t>(GetBValue(cc.rgbResult)) << 8) | 0xFFu;

    auto c = ToolbarStore::custom();
    switch (slot) {
    case 0: c.primary = v; break;
    case 1: c.hover = v; break;
    case 2: c.icon = v; break;
    case 3: c.accent = v; break;
    case 4: c.selection = v; break;
    }
    ToolbarStore::applyCustom(c, true);
    tbRefreshTheme();
    tbRefreshPreview();
}
