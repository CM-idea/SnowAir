#include "pch.h"
#include <filesystem>
#include "WinSettingPlugins.h"
#include "SettingWidgets.h"
#include "SettingTheme.h"
#include "../Ocr/OcrModelManager.h"
#include "../Ocr/OcrRuntimeManager.h"
#include "../Ocr/OcrPackVariant.h"
#include "../Ocr/PluginPaths.h"
#include "../Translate/TranslateModelManager.h"
#include "../Lang.h"
#include "../Tool/IconCodes.h"

namespace {
	// 目录存在且非空
	bool dirNonEmpty(const std::filesystem::path& p)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(p, ec)) return false;
		auto it = std::filesystem::directory_iterator(p, ec);
		return !ec && it != std::filesystem::directory_iterator();
	}
	// 离线文字识别只提供 PP-OCRv5 一个方案
	constexpr OcrPackVariant kOcrVariant = OcrPackVariant::Embedded;

	// 「正在下载」省略号动画的定时器（本页独用；WM_APP + id 交给 onTimer）。
	// 取 0x3C00：低于 TextBox 的 timerId 基数 0x4200，绝不与其自增 id 撞车。
	constexpr UINT kDotsTimerId{ 0x3C00 };
	constexpr UINT kDotsIntervalMs{ 420 };

	// 离线翻译语言包
	struct LangPack {
		const wchar_t* id;
		const wchar_t* labelKey;
	};
	constexpr LangPack kLangPacks[] = {
		{ L"zh-en",   L"setting.plugLangZhEn"   },
		{ L"zh-Hant", L"setting.plugLangZhHant" },
		{ L"ru",      L"setting.plugLangRu"     },
		{ L"ko",      L"setting.plugLangKo"     },
		{ L"ja",      L"setting.plugLangJa"     },
		{ L"fr",      L"setting.plugLangFr"     },
		{ L"es",      L"setting.plugLangEs"     },
		{ L"de",      L"setting.plugLangDe"     },
	};
	constexpr int kLangPackCount = (int)(sizeof(kLangPacks) / sizeof(kLangPacks[0]));
}

WinSettingPlugins::WinSettingPlugins(Ling::WinBase* parent) : Ling::Node(parent)
{
	setFlexDirection(Ling::FlexDirection::Column);
	build();
	// 下载按钮非常驻：跟随窗口鼠标移动判断光标是否落在某张卡片上
	auto weakThis = getWeakThis();
	hoverTok = win->onMouseMove.add([this, weakThis](POINT pos) {
		if (!weakThis.lock()) return;
		this->updateHover(pos);
		});
	// 「正在下载」省略号动画
	timerTok = win->onTimer.add([this, weakThis](UINT id) {
		if (id != kDotsTimerId) return;
		if (!weakThis.lock()) return;
		this->dotsPhase = (this->dotsPhase + 1) % 4;
		this->applyDots();
		});
}

WinSettingPlugins::~WinSettingPlugins()
{
	stopDots();
	win->onMouseMove.remove(hoverTok);
	win->onTimer.remove(timerTok);
}

// 生成一张插件卡片：卡片容器 + 顶部行（左说明列 / 右方形图标按钮）+ 徽章行
WinSettingPlugins::PackUi WinSettingPlugins::makePack(Ling::Node* parent,
	const std::wstring& title, const std::wstring& desc)
{
	PackUi ui;
	ui.card = parent->makeChild<Ling::Node>();
	ui.card->setFlexDirection(Ling::FlexDirection::Column);
	ui.card->setWidthPercent(100.f);
	ui.card->setBg(0);   // 卡片不用背景色
	ui.card->setBorder(1.f, SettingTheme::border);
	ui.card->setBorderRadius(SettingTheme::radiusLg);
	ui.card->setPadding(SettingTheme::sp3, SettingTheme::sp3, SettingTheme::sp3, SettingTheme::sp3);
	ui.card->setMarginBottom(SettingTheme::blockGap);

	auto* top = ui.card->makeChild<Ling::Node>();
	top->setFlexDirection(Ling::FlexDirection::Row);
	top->setWidthPercent(100.f);
	top->setAlignItems(Ling::Align::FlexStart);

	// 左列：标题行（标题 + 内嵌下拉）/ 说明 / 进度
	auto* col = top->makeChild<Ling::Node>();
	col->setFlexDirection(Ling::FlexDirection::Column);
	col->setWidthPercent(0.f);
	col->setFlexGrow(1.f);
	col->setFlexShrink(1.f);
	col->setMarginRight(SettingTheme::sp3);

	ui.titleRow = col->makeChild<Ling::Node>();
	ui.titleRow->setFlexDirection(Ling::FlexDirection::Row);
	ui.titleRow->setWidthPercent(100.f);
	ui.titleRow->setAlignItems(Ling::Align::Center);

	auto* t = ui.titleRow->makeChild<Ling::Label>();
	t->setText(title);
	t->setFontSize(SettingTheme::fontBase);
	t->setColor(SettingTheme::textPrimary);
	t->setFlexShrink(0.f);

	ui.desc = col->makeChild<Ling::Label>();
	ui.desc->setText(desc);
	ui.desc->setFontSize(SettingTheme::fontSm);
	ui.desc->setColor(SettingTheme::textSecondary);
	ui.desc->setWidthPercent(100.f);
	ui.desc->setMarginTop(4.f);
	ui.desc->setJustifyContent(Ling::Justify::Start);
	ui.desc->setAlignItems(Ling::Align::FlexStart);

	ui.progress = col->makeChild<Ling::Label>();
	ui.progress->setFontSize(SettingTheme::fontSm);
	ui.progress->setColor(SettingTheme::textTertiary);
	ui.progress->setWidthPercent(100.f);
	ui.progress->setMarginTop(4.f);
	ui.progress->setJustifyContent(Ling::Justify::Start);
	ui.progress->setAlignItems(Ling::Align::FlexStart);
	ui.progress->hide();

	// 右列：右上角方形图标按钮（36×36）
	auto* actions = top->makeChild<Ling::Node>();
	actions->setFlexDirection(Ling::FlexDirection::Column);
	actions->setWidth(36.f);
	actions->setFlexShrink(0.f);
	actions->setAlignItems(Ling::Align::FlexEnd);

	ui.dlBtn = actions->makeChild<Ling::Button>();
	ui.dlBtn->setText(Icon::Download);
	ui.dlBtn->setFontFamily(Icon::Family);
	ui.dlBtn->setFontSize(18.f);
	ui.dlBtn->setSize(36.f, 36.f);
	ui.dlBtn->setPadding(0.f, 0.f, 0.f, 0.f);
	ui.dlBtn->setBorderRadius(SettingTheme::radiusLg);
	ui.dlBtn->setBorder(0.f, 0);
	// 固定配色：#F5F4F4 底 + 黑图标；悬停色取同值 → 没有任何额外的停悬效果
	ui.dlBtn->setBg(0xF5F4F4FF);
	ui.dlBtn->setHoverBg(0xF5F4F4FF);
	ui.dlBtn->setColor(0x000000FF);
	ui.dlBtn->setHoverColor(0x000000FF);
	ui.dlBtn->setFlexDirection(Ling::FlexDirection::Row);
	ui.dlBtn->setJustifyContent(Ling::Justify::Center);
	ui.dlBtn->setAlignItems(Ling::Align::Center);
	ui.dlBtn->hide();                       // 非常驻：鼠标停在卡片上才显示

	// 删除按钮（同款方形图标，默认隐藏）：已安装 + 鼠标停在卡片上时显示，
	// 出现在下载按钮同一位置（下载按钮此时已隐藏）
	ui.delBtn = actions->makeChild<Ling::Button>();
	ui.delBtn->setText(Icon::Remove);
	ui.delBtn->setFontFamily(Icon::Family);
	ui.delBtn->setFontSize(18.f);
	ui.delBtn->setSize(36.f, 36.f);
	ui.delBtn->setPadding(0.f, 0.f, 0.f, 0.f);
	ui.delBtn->setBorderRadius(SettingTheme::radiusLg);
	ui.delBtn->setBorder(0.f, 0);
	// 删除按钮：红底（20% 红）+ 红图标，与其它按钮一致不做停悬变化
	ui.delBtn->setBg(0xFF383C33);          // #FF383C @ 20%
	ui.delBtn->setHoverBg(0xFF383C33);
	ui.delBtn->setColor(0xFF383CFF);       // #FF383C
	ui.delBtn->setHoverColor(0xFF383CFF);
	ui.delBtn->setFlexDirection(Ling::FlexDirection::Row);
	ui.delBtn->setJustifyContent(Ling::Justify::Center);
	ui.delBtn->setAlignItems(Ling::Align::Center);
	ui.delBtn->hide();

	// 徽章行（默认隐藏）：由 refreshStatus 按安装情况填充绿色徽章
	ui.badgeRow = ui.card->makeChild<Ling::Node>();
	ui.badgeRow->setFlexDirection(Ling::FlexDirection::Row);
	ui.badgeRow->setWidthPercent(100.f);
	ui.badgeRow->setAlignItems(Ling::Align::Center);
	ui.badgeRow->setMarginTop(10.f);
	ui.badgeRow->hide();
	return ui;
}

// 用一组标签重填徽章行（先清空）；空列表 = 隐藏
void WinSettingPlugins::setBadges(Ling::Node* row, const std::vector<std::wstring>& labels)
{
	if (!row) return;
	row->removeAllChildren();
	if (labels.empty()) { row->hide(); return; }
	for (size_t i = 0; i < labels.size(); i++) {
		auto* badge = row->makeChild<Ling::Label>();
		badge->setText(labels[i]);
		badge->setFontSize(SettingTheme::fontXs);
		badge->setColor(SettingTheme::success);
		badge->setBg(0x5EC3222A);            // rgba(94,195,34,0.16)
		badge->setBorderRadius(SettingTheme::radiusSm);
		badge->setPadding(7.f, 2.f, 7.f, 2.f);
		badge->setFlexShrink(0.f);
		if (i + 1 < labels.size()) badge->setMarginRight(6.f);
	}
	row->show();
}

void WinSettingPlugins::refreshStatus()
{
	// —— 离线文字识别 ——
	const bool ocrOk = OcrModelManager::instance().isInstalled(kOcrVariant)
		&& OcrRuntimeManager::instance().isInstalled(kOcrVariant);
	setBadges(ocrBadgeRow, ocrOk ? std::vector<std::wstring>{ OcrPack::label(kOcrVariant) }
		: std::vector<std::wstring>{});

	// —— 离线翻译：徽章列出**所有已安装语言包**（与下拉当前选择无关）——
	// 用户口径：换到别的（未安装）语言时，已装好的语言包提示不能消失。
	auto& trMgr = TranslateModelManager::instance();
	std::vector<std::wstring> trBadges;
	for (const auto& id : trMgr.installedPacks()) {
		std::wstring label = id;
		for (int i = 0; i < kLangPackCount; i++)
			if (id == kLangPacks[i].id) { label = Lang::get(kLangPacks[i].labelKey); break; }
		trBadges.push_back(label);
	}
	setBadges(trBadgeRow, trBadges);
	// 当前下拉选中项是否已装（决定显示下载还是删除）
	const std::wstring trPackId = kLangPacks[trPackIdx].id;
	const bool trOk = trMgr.isDllInstalled() && trMgr.isPackInstalled(trPackId);

	// —— 悬停状态（下载/删除按钮本身由 updateHover 控制显隐）——
	if (cards.size() >= 3) {
		cards[CardOcr].installed = ocrOk;
		cards[CardTr].installed = trOk;
		cards[CardGh].installed = false;   // 果核看图是外链，不提供删除
		cards[CardOcr].offer = !ocrOk && !cards[CardOcr].busy;
		cards[CardTr].offer = !trOk && !cards[CardTr].busy;
		cards[CardGh].offer = true;      // 果核看图始终提供（外链）
		for (auto& c : cards) {
			// 下载中按钮常驻显示进度，不在这里隐藏
			if (!c.offer && c.dlBtn && !c.busy) c.dlBtn->hide();
			// 未安装就没有删除项可显示
			if (!c.installed && c.delBtn) c.delBtn->hide();
		}
	}

	// —— 统计（按插件目录计数组件，不计内部版本/份数）——
	int n = 0;
	if (dirNonEmpty(PluginPaths::ocrModels()) || dirNonEmpty(PluginPaths::ocrBin())) ++n;
	if (dirNonEmpty(PluginPaths::translateRoot())) ++n;
	if (summary) {
		std::wstring t = Lang::get(L"setting.pluginsSummary");
		auto pos = t.find(L"%1");
		if (pos != std::wstring::npos) t.replace(pos, 2, std::to_wstring(n));
		summary->setText(t);
	}
}

void WinSettingPlugins::build()
{
	// 注：滚动已由外层 WinSetting 的内容 ScrollerBox 统一管理，本页不再自建滚动容器。
	SettingUi::sectionHeader(this, Lang::get(L"setting.plugins"),
		Lang::get(L"setting.pluginsDesc"), Lang::get(L"setting.pluginsTip"));

	// —— 离线文字识别（固定 PP-OCRv5，无方案下拉）——
	auto ocr = makePack(this, Lang::get(L"setting.plugOcrTitle"), Lang::get(L"setting.plugOcrDesc"));
	ocrBadgeRow = ocr.badgeRow;
	ocrProgress = ocr.progress;
	ocr.dlBtn->onClick.add([this](Ling::Button*) { this->startOcrDownload(); });
	ocr.delBtn->onClick.add([this](Ling::Button*) { this->deleteCard(CardOcr); });
	cards.push_back({ ocr.card, ocr.dlBtn, ocr.delBtn, false, false, false });

	// —— 离线翻译（标题行内嵌语言包下拉，选中项决定下载目标与安装态）——
	auto tr = makePack(this, Lang::get(L"setting.plugTransTitle"), Lang::get(L"setting.plugTransDesc"));
	trBadgeRow = tr.badgeRow;
	trProgress = tr.progress;
	{
		std::vector<std::wstring> labels;
		labels.reserve(kLangPackCount);
		for (const auto& p : kLangPacks) labels.push_back(Lang::get(p.labelKey));
		auto* sel = SettingUi::selectDdl(tr.titleRow, labels, trPackIdx,
			[this](int i, const std::wstring&) {
				this->trPackIdx = i;
				this->refreshStatus();
			});
		sel->setWidth(120.f);              // 宽度 120
		sel->setMarginLeft(6.f);
		sel->setFlexShrink(0.f);
		sel->setBorder(0.f, 0);            // 无边框
		// 值文字改灰（值 Label 是按钮的第一个 Label 子节点，内部 Text 不是 Label）
		for (auto& c : sel->children) {
			if (auto* lab = dynamic_cast<Ling::Label*>(c.get())) {
				lab->setColor(SettingTheme::textSecondary);
				break;
			}
		}
	}
	tr.dlBtn->onClick.add([this](Ling::Button*) { this->startTrDownload(); });
	tr.delBtn->onClick.add([this](Ling::Button*) { this->deleteCard(CardTr); });
	cards.push_back({ tr.card, tr.dlBtn, tr.delBtn, false, false, false });

	// —— 果核看图（作者推荐，外链，不内置下载）——
	auto gh = makePack(this, Lang::get(L"setting.plugGhTitle"), Lang::get(L"setting.plugGhDesc"));
	// 「?」说明：走统一的设置项问号（圆形按钮 + 停悬气泡，受「功能提示」总开关控制）
	SettingUi::helpTipButton(gh.titleRow, Lang::get(L"setting.plugGhTip"));
	gh.dlBtn->onClick.add([](Ling::Button*) {
		ShellExecute(nullptr, L"open", L"https://pic.ghxi.com/download",
			nullptr, nullptr, SW_SHOWNORMAL);
	});
	cards.push_back({ gh.card, gh.dlBtn, gh.delBtn, false, false, false });

	// —— 底部：左「打开组件目录」，右「已安装组件：N」（对齐截图历史页右下角的计数文案）——
	auto* row = makeChild<Ling::Node>();
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setWidthPercent(100.f);
	row->setAlignItems(Ling::Align::Center);
	SettingUi::btnOutline(row, Lang::get(L"setting.pluginsOpenDir"), []() {
		auto dir = PluginPaths::root();
		std::filesystem::create_directories(dir);
		ShellExecute(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	});
	auto* spacer = row->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);
	spacer->setFlexShrink(1.f);

	summary = row->makeChild<Ling::Label>();
	summary->setFontSize(SettingTheme::fontSm);
	summary->setColor(SettingTheme::textMuted);
	summary->setFlexShrink(0.f);
	summary->setMarginLeft(SettingTheme::sp3);

	refreshStatus();
	// 内容区只有外层 content 提供底部留白：清掉末尾元素的 marginBottom，使底部与左右一致。
	SettingUi::trimTrailingGap(this);
}

// 鼠标移动：光标落在哪张卡片上就显示哪张卡片的按钮，其余隐藏。
// 未安装 → 下载；已安装 → 删除；下载中 → 常驻显示进度按钮。
void WinSettingPlugins::updateHover(POINT pos)
{
	for (auto& c : cards) {
		if (!c.card) continue;
		if (c.busy) {
			if (c.dlBtn) c.dlBtn->show();
			if (c.delBtn) c.delBtn->hide();
			continue;
		}
		const bool inside = c.card->isPosIn(pos);
		const bool showDel = inside && c.installed && c.delBtn != nullptr;
		const bool showDl = inside && !c.installed && c.offer && c.dlBtn != nullptr;
		if (c.dlBtn) { if (showDl) c.dlBtn->show(); else c.dlBtn->hide(); }
		if (c.delBtn) { if (showDel) c.delBtn->show(); else c.delBtn->hide(); }
	}
}

// 把回调切回 UI 线程（下载回调在工作线程）
namespace {
	void onUi(std::function<void()> fn)
	{
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(std::move(fn));
		else fn();
	}
}

void WinSettingPlugins::startOcrDownload()
{
	if (cards.size() < 3) return;
	if (cards[CardOcr].busy) return;   // 下载中再点不重开（按钮此时显示进度）
	CardSlot& c = cards[CardOcr];
	c.busy = true;
	c.offer = false;
	if (c.delBtn) c.delBtn->hide();
	if (c.dlBtn) c.dlBtn->show();          // 下载中按钮常驻显示进度
	if (ocrBadgeRow) ocrBadgeRow->hide();
	setCardProgress(c, 0.f);
	startDots(ocrProgress, Lang::get(L"setting.plugDownloading"));

	auto weak = getWeakThis();
	// 两段式（串行）：运行时 0~50% → 模型 50~100%（并行时进度会互相覆盖，进度条没意义）
	OcrRuntimeManager::instance().download(kOcrVariant,
		[this, weak](float p) {
			onUi([this, weak, p]() {
				if (!weak.lock() || cards.size() < 3) return;
				setCardProgress(cards[CardOcr], 0.5f * p);
			});
		},
		[this, weak](bool ok, std::wstring err) {
			onUi([this, weak, ok, err]() {
				if (!weak.lock() || cards.size() < 3) return;
				if (!ok) { finishDownload(CardOcr, err); return; }
				setCardProgress(cards[CardOcr], 0.5f);
				OcrModelManager::instance().download(kOcrVariant,
					[this, weak](float p) {
						onUi([this, weak, p]() {
							if (!weak.lock() || cards.size() < 3) return;
							setCardProgress(cards[CardOcr], 0.5f + 0.5f * p);
						});
					},
					[this, weak](bool ok2, std::wstring err2) {
						onUi([this, weak, ok2, err2]() {
							if (!weak.lock()) return;
							finishDownload(CardOcr, ok2 ? std::wstring{} : err2);
						});
					});
			});
		});
}

void WinSettingPlugins::startTrDownload()
{
	if (cards.size() < 3) return;
	if (cards[CardTr].busy) return;   // 下载中再点不重开（按钮此时显示进度）
	CardSlot& c = cards[CardTr];
	c.busy = true;
	c.offer = false;
	if (c.delBtn) c.delBtn->hide();
	if (c.dlBtn) c.dlBtn->show();
	if (trBadgeRow) trBadgeRow->hide();
	setCardProgress(c, 0.f);
	// 进度行带语言包名（正在下载「英语」...）
	startDots(trProgress, Lang::get(L"setting.plugDownloading") + L"「"
		+ Lang::get(kLangPacks[trPackIdx].labelKey) + L"」");

	const std::wstring packId = kLangPacks[trPackIdx].id;
	auto weak = getWeakThis();
	TranslateModelManager::instance().download(packId,
		[this, weak](float p) {
			onUi([this, weak, p]() {
				if (!weak.lock() || cards.size() < 3) return;
				setCardProgress(cards[CardTr], p);
			});
		},
		[this, weak](bool ok, std::wstring err) {
			onUi([this, weak, ok, err]() {
				if (!weak.lock()) return;
				finishDownload(CardTr, ok ? std::wstring{} : err);
			});
		});
}

void WinSettingPlugins::setCardProgress(CardSlot& c, float p)
{
	if (!c.dlBtn) return;
	int pct = (int)(p * 100.f + 0.5f);
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	// 数字要默认 UI 字体（图标字体没有数字字形）
	c.dlBtn->setFontFamily(L"");
	c.dlBtn->setFontSize(12.f);
	c.dlBtn->setText(std::to_wstring(pct) + L"%");
}

void WinSettingPlugins::finishDownload(int idx, const std::wstring& err)
{
	if (idx < 0 || idx >= (int)cards.size()) return;
	stopDots();
	CardSlot& c = cards[idx];
	c.busy = false;
	if (c.dlBtn) {
		c.dlBtn->setFontFamily(Icon::Family);
		c.dlBtn->setFontSize(18.f);
		c.dlBtn->setText(Icon::Download);
		c.dlBtn->hide();
	}
	Ling::Label* lab = (idx == CardOcr) ? ocrProgress : (idx == CardTr) ? trProgress : nullptr;
	if (lab) {
		if (err.empty()) lab->hide();
		else { lab->setText(err); lab->show(); }   // 失败：进度行改显错误
	}
	refreshStatus();
	// 刷新后按当前光标位置重算按钮显隐
	POINT pt{}; GetCursorPos(&pt); ScreenToClient(win->hwnd, &pt);
	updateHover(pt);
}

void WinSettingPlugins::deleteCard(int idx)
{
	if (idx < 0 || idx >= (int)cards.size()) return;
	std::wstring msg;
	if (idx == CardOcr) msg = Lang::get(L"setting.plugDelOcrConfirm");
	else if (idx == CardTr) msg = Lang::get(L"setting.plugDelTrConfirm");
	else return;   // 果核看图是外链，不提供删除
	auto weak = getWeakThis();
	SettingUi::ConfirmDialog::ask(win, msg, [this, weak, idx](bool ok) {
		if (!ok || !weak.lock()) return;
		if (idx == CardOcr) {
			OcrModelManager::instance().uninstall(kOcrVariant);
			OcrRuntimeManager::instance().uninstall(kOcrVariant);
		}
		else if (idx == CardTr) {
			TranslateModelManager::instance().uninstallPack(kLangPacks[trPackIdx].id);
		}
		refreshStatus();
		POINT pt{}; GetCursorPos(&pt); ScreenToClient(win->hwnd, &pt);
		updateHover(pt);
	});
}

void WinSettingPlugins::startDots(Ling::Label* lab, const std::wstring& prefix, const std::wstring& suffix)
{
	dotsLabel = lab;
	dotsPrefix = prefix;
	dotsSuffix = suffix;
	dotsPhase = 0;
	dotsRunning = true;
	applyDots();
	if (lab) lab->show();
	win->setTimer(kDotsIntervalMs, kDotsTimerId);
}

void WinSettingPlugins::stopDots()
{
	if (!dotsRunning && !dotsLabel) return;
	dotsRunning = false;
	win->killTimer(kDotsTimerId);
	dotsLabel = nullptr;
	dotsPrefix.clear();
	dotsSuffix.clear();
}

void WinSettingPlugins::applyDots()
{
	if (!dotsLabel) return;
	static const wchar_t* kDots[] = { L"", L".", L"..", L"..." };
	dotsLabel->setText(dotsPrefix + kDots[dotsPhase % 4] + dotsSuffix);
}
