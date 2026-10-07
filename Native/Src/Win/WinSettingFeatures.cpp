#include "pch.h"
#include "WinSettingFeatures.h"
#include "SettingWidgets.h"
#include "SettingTheme.h"
#include "../Setting.h"
#include "../Lang.h"
#include "../Util.h"
#include "../Tool/IconCodes.h"
#include "../Ocr/OcrModelManager.h"
#include "../Ocr/OcrRuntimeManager.h"
#include "../Ocr/OcrPackVariant.h"
#include "../Translate/TranslateTypes.h"
#include <shobjidl.h>

namespace {
	// 文件夹选择
	std::wstring pickFolder(HWND owner, const std::wstring& start)
	{
		std::wstring out;
		IFileOpenDialog* dlg = nullptr;
		if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
			__uuidof(IFileOpenDialog), (void**)&dlg)) || !dlg)
			return out;
		DWORD opts = 0;
		if (SUCCEEDED(dlg->GetOptions(&opts)))
			dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		if (!start.empty()) {
			IShellItem* dir = nullptr;
			if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&dir)))) {
				dlg->SetFolder(dir);
				dir->Release();
			}
		}
		if (SUCCEEDED(dlg->Show(owner))) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item))) {
				PWSTR p = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
					out = p;
					CoTaskMemFree(p);
				}
				item->Release();
			}
		}
		dlg->Release();
		return out;
	}

	// 文件输出页标签列固定宽，使各输入框左缘整齐
	constexpr float kOutputLabelW{ 150.f };

	// 「删除 / 重置」类（垃圾桶图标）按钮：红底（20% 红）+ 红图标
	constexpr uint32_t kDangerRedBg{ 0xFF383C33 };   // #FF383C @ 20%
	constexpr uint32_t kDangerRed{ 0xFF383CFF };     // #FF383C
	bool isTrashGlyph(const wchar_t* glyph) { return glyph && _wcsicmp(glyph, Icon::Remove) == 0; }

	// 「文件输出」行右侧辅助按钮（方形图标按钮，与输入框同高）
	// 样式对齐「插件集成 → 下载按钮」：平铺浅灰(#F5F4F4)、无描边、无停悬变化；
	// 垃圾桶图标（重置）用红色图标。
	Ling::Button* outIconBtn(Ling::Node* row, const wchar_t* glyph)
	{
		const bool danger = isTrashGlyph(glyph);
		auto* b = row->makeChild<Ling::Button>();
		b->setText(glyph);
		b->setFontFamily(Icon::Family);
		b->setFontSize(Icon::SizeSm);
		b->setColor(danger ? kDangerRed : 0x000000FF);
		b->setHoverColor(danger ? kDangerRed : 0x000000FF);
		b->setWidth(SettingTheme::ctrlH);
		b->setHeight(SettingTheme::ctrlH);
		b->setFlexShrink(0.f);
		b->setBorderRadius(SettingTheme::radiusCtl);
		b->setBorder(0.f, 0);
		b->setBg(danger ? kDangerRedBg : 0xF5F4F4FF);
		b->setHoverBg(danger ? kDangerRedBg : 0xF5F4F4FF);
		b->setMarginLeft(SettingTheme::sp2);
		b->hide();   // 默认不显示：点进输入框（或已有自定义值）才浮出
		return b;
	}

	enum class OutputRowKind { Format, Dir };

	// 「文件输出」行：软灰底 + 1px 描边 + 圆角 8；左标签定宽，右输入框吃满剩余宽度。
	// 交互：
	//   · 空 / 恰等于默认值 = 没改过 → 只显示**灰色占位符**（默认模板 / 默认目录）；
	//   · 点进输入框（此时自动填入默认值方便编辑，只显示不落盘）→ 右侧浮出辅助按钮：
	//       Format = 「重置」；Dir = 「选择文件夹」+「重置」；
	//   · 失焦：自动填入的默认值原样收回成灰占位符（不写设置），手打的值才提交。
	Ling::TextBox* outputRow(Ling::Node* p, const std::wstring& label,
		const std::wstring& storedValue, const std::wstring& defaultValue, OutputRowKind kind,
		std::function<void(const std::wstring&)> onCommit)
	{
		auto* row = p->makeChild<Ling::Node>();
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setWidthPercent(100.f);
		row->setAlignItems(Ling::Align::Center);
		row->setBg(SettingTheme::card);   // 白底，与其它卡片保持一致（只靠 1px 描边分界）
		row->setBorder(1.f, SettingTheme::border);
		// 行外框 = 控件圆角(8) + 行内边距(sp2)，与输入框同心
		row->setBorderRadius(SettingTheme::radiusCtl + SettingTheme::sp2);
		// 右内边距取与控件上下留白相同的 sp2：输入框上/下/右三侧留白一致
		row->setPadding(SettingTheme::sp3, SettingTheme::sp2, SettingTheme::sp2, SettingTheme::sp2);
		row->setMarginBottom(SettingTheme::optionGap);
		YGNodeStyleSetMinWidth(row->node, 0.f);
		YGNodeStyleSetMinHeight(row->node, 0.f);

		auto* lab = row->makeChild<Ling::Label>();
		lab->setText(label);
		lab->setFontSize(SettingTheme::fontBase);
		lab->setColor(SettingTheme::textPrimary);
		lab->setWidth(kOutputLabelW);
		lab->setFlexShrink(0.f);

		// 空 / 恰等于默认值 = 没改过 → 显示成灰占位符
		const bool isDefault = storedValue.empty() ||
			_wcsicmp(storedValue.c_str(), defaultValue.c_str()) == 0;

		auto* box = row->makeChild<Ling::TextBox>();
		box->setHeight(SettingTheme::ctrlH);
		box->setWidthPercent(0.f);
		box->setFlexGrow(1.f);
		box->setFlexShrink(1.f);
		YGNodeStyleSetMinWidth(box->node, 0.f);
		box->setBorderRadius(SettingTheme::radiusCtl);   // 与外层容器同心（比外框小一档）
		box->setBorder(1.f, SettingTheme::input);
		box->setBg(SettingTheme::card);
		box->setColor(SettingTheme::textPrimary);
		box->setPadding(SettingTheme::sp2, 0, SettingTheme::sp2, 0);
		box->setFontSize(SettingTheme::fontBase);
		box->setVerticalCenter(true);
		box->setMarginLeft(SettingTheme::sp3);
		box->setPlaceholder(defaultValue);
		box->setPlaceholderColor(SettingTheme::placeholder);
		box->setText(isDefault ? L"" : storedValue);

		// 顺序：选择文件夹在前、重置在后（浏览 → 垃圾桶）
		auto* browseBtn = (kind == OutputRowKind::Dir)               // 选择文件夹
			? outIconBtn(row, Icon::Folder) : nullptr;
		auto* resetBtn = outIconBtn(row, Icon::Remove);              // 重置（恢复默认）

		auto autoFilled = std::make_shared<bool>(false);
		auto syncButtons = [box, resetBtn, browseBtn]() {
			const bool on = box->isFocused() || !box->getText().empty();
			if (on) {
				resetBtn->show();
				if (browseBtn) browseBtn->show();
			}
			else {
				resetBtn->hide();
				if (browseBtn) browseBtn->hide();
			}
		};

		box->onFocusChanged.add([box, defaultValue, autoFilled, syncButtons, onCommit](
			Ling::TextBox*, bool focused) {
			if (focused) {
				if (box->getText().empty()) {
					// 先 setText（会触发 onTextChanged 清掉标记），再把标记置真
					box->setText(defaultValue);
					*autoFilled = true;
				}
				else {
					*autoFilled = false;
				}
				syncButtons();
				return;
			}
			// 失焦：自动填入的默认值原样收回（不落盘）→ 回到灰占位符
			bool doCommit = true;
			if (*autoFilled) {
				*autoFilled = false;
				box->setText(L"");
				doCommit = false;
			}
			else if (_wcsicmp(box->getText().c_str(), defaultValue.c_str()) == 0) {
				// 手打了一遍默认值 = 没改过
				box->setText(L"");
				doCommit = false;
			}
			syncButtons();
			if (doCommit && onCommit) onCommit(box->getText());
		});
		box->onTextChanged.add([autoFilled, syncButtons](Ling::TextBox*, const std::wstring&) {
			*autoFilled = false;   // 内容被改过 → 不再是"自动填入的默认值"
			syncButtons();
		});

		resetBtn->onClick.add([box, autoFilled, syncButtons, onCommit](Ling::Button*) {
			*autoFilled = false;
			box->setText(L"");
			box->blur();                 // 清空后退出编辑态 → 回到灰占位符
			if (onCommit) onCommit(L"");
			syncButtons();
		});
		if (browseBtn) {
			browseBtn->onClick.add([box, defaultValue, autoFilled, syncButtons, onCommit](Ling::Button*) {
				std::wstring start = box->getText();
				if (start.empty()) start = defaultValue;
				const std::wstring picked = pickFolder(box->win ? box->win->hwnd : nullptr, start);
				if (picked.empty()) return;          // 取消选择 → 保持原样
				*autoFilled = false;
				box->setText(picked);
				box->focus();                        // 保持编辑态，两个按钮继续可见
				if (onCommit) onCommit(picked);
				syncButtons();
			});
		}

		// 悬停提示：
		//   · 格式行 → 把当前模板展开成示例文件名预览（编辑态不预览）；
		//   · 目录行 → 目录说明。
		// 离开即收（带 owner，避免误关别人的提示）。
		if (auto* tip = SettingUi::helpTip()) {
			if (kind == OutputRowKind::Format) {
				box->onEnter.add([box, tip, defaultValue](Ling::TextBox*) {
					if (box->isFocused()) return;
					const std::wstring fmt = box->getText().empty() ? defaultValue : box->getText();
					const std::wstring name = Util::previewFileName(fmt, defaultValue, L"");
					if (name.empty()) return;
					tip->showFor(box, Lang::get(L"setting.previewPrefix") + name);
				});
			}
			else {
				box->onEnter.add([box, tip](Ling::TextBox*) {
					tip->showFor(box, Lang::get(L"setting.outputDirTip"));
				});
			}
			box->onLeave.add([box, tip](Ling::TextBox*) { tip->hideFor(box); });
		}
		return box;
	}

	// API 卡片内的「字段」：label 在上、输入框在下的竖排小块
	Ling::Node* apiField(Ling::Node* parent, const std::wstring& label,
		const std::wstring& value,
		std::function<void(const std::wstring&)> onCommit, Ling::TextBox** out = nullptr)
	{
		auto* col = parent->makeChild<Ling::Node>();
		col->setFlexDirection(Ling::FlexDirection::Column);
		col->setFlexShrink(1.f);
		YGNodeStyleSetMinWidth(col->node, 0.f);

		auto* lab = col->makeChild<Ling::Label>();
		lab->setText(label);
		lab->setFontSize(SettingTheme::fontBase);
		lab->setColor(SettingTheme::textPrimary);
		lab->setMarginBottom(SettingTheme::sp2);

		auto* box = col->makeChild<Ling::TextBox>();
		box->setHeight(SettingTheme::ctrlH);
		box->setWidthPercent(100.f);
		box->setBorderRadius(SettingTheme::radiusCtl);   // 与外层容器同心（比外框小一档）
		box->setBorder(1.f, SettingTheme::input);
		box->setBg(SettingTheme::card);
		box->setColor(SettingTheme::textPrimary);
		box->setPadding(SettingTheme::sp2, 0, SettingTheme::sp2, 0);
		box->setFontSize(SettingTheme::fontBase);
		box->setVerticalCenter(true);
		box->setPlaceholder(Lang::get(L"setting.fnInputHint"));
		box->setPlaceholderColor(SettingTheme::placeholder);
		box->setText(value);
		box->onFocusChanged.add([onCommit](Ling::TextBox* tb, bool focused) {
			if (!focused && onCommit) onCommit(tb->getText());
		});
		if (out) *out = box;
		return col;
	}

	// 方形图标按钮（36×36，圆角 10）：样式对齐「插件集成 → 下载按钮」——
	// 平铺浅灰(#F5F4F4)、无描边、无停悬变化；垃圾桶图标（删除）走红底(20% 红)+红图标。
	Ling::Button* squareIconBtn(Ling::Node* parent, const wchar_t* glyph,
		std::function<void()> onClick)
	{
		const bool danger = isTrashGlyph(glyph);
		auto* btn = parent->makeChild<Ling::Button>();
		btn->setSize(36.f, 36.f);
		btn->setBorderRadius(SettingTheme::radiusLg);
		btn->setBorderWidth(0.f);
		btn->setBg(danger ? kDangerRedBg : 0xF5F4F4FF);
		btn->setHoverBg(danger ? kDangerRedBg : 0xF5F4F4FF);
		btn->setFlexDirection(Ling::FlexDirection::Row);
		btn->setJustifyContent(Ling::Justify::Center);
		btn->setAlignItems(Ling::Align::Center);
		btn->setText(glyph);
		btn->setFontFamily(Icon::Family);
		btn->setFontSize(18.f);
		btn->setColor(danger ? kDangerRed : 0x000000FF);
		btn->setHoverColor(danger ? kDangerRed : 0x000000FF);
		btn->onClick.add([onClick](Ling::Button*) { if (onClick) onClick(); });
		return btn;
	}
}

std::wstring WinSettingFeatures::newId()
{
	return L"id-" + std::to_wstring(GetTickCount64());
}

WinSettingFeatures::WinSettingFeatures(Ling::WinBase* parent) : Ling::Node(parent)
{
	setFlexDirection(Ling::FlexDirection::Column);
	build();
}

WinSettingFeatures::~WinSettingFeatures()
{
	SettingUi::closePopup(win);
}

void WinSettingFeatures::showTab(int index)
{
	if (index < 0 || index > 4) return;
	tabIndex = index;
	// 同步胶囊选中态（不触发 onChange，避免点击后递归）
	segPills.select(index);
	for (int i = 0; i < 5; i++) {
		if (tabs[i]) {
			if (i == index) tabs[i]->show();
			else tabs[i]->hide();
		}
	}
	if (restoreBtn) {
		restoreBtn->setColor(index == 0 ? SettingTheme::textPrimary : SettingTheme::textMuted);
		restoreBtn->setHoverColor(index == 0 ? SettingTheme::textPrimary : SettingTheme::textMuted);
	}
}

void WinSettingFeatures::rebuildTab(int index)
{
	if (index < 0 || index > 4 || !tabs[index]) return;
	tabs[index]->removeAllChildren();
	if (index == 0) buildBasic(tabs[index]);
	else if (index == 1) buildRecord(tabs[index]);
	else if (index == 2) buildOutput(tabs[index]);
	else if (index == 3) buildOcr(tabs[index]);
	else buildTranslate(tabs[index]);
	// 内容区只有外层 content 提供底部留白：清掉该 tab 末尾元素的 marginBottom。
	SettingUi::trimTrailingGap(tabs[index]);
}

void WinSettingFeatures::deferRebuildTab(int index)
{
	auto weak = getWeakThis();
	auto task = [this, weak, index]() { if (weak.lock()) rebuildTab(index); };
	if (auto* app = Ling::App::get()) app->dq.TryEnqueue(task);
	else task();
}

void WinSettingFeatures::build()
{
	const std::vector<std::wstring> labels = {
		Lang::get(L"setting.fnTabBasic"), Lang::get(L"setting.fnTabVideo"),
		Lang::get(L"setting.fnTabOutput"), Lang::get(L"setting.fnTabOcr"),
		Lang::get(L"setting.fnTabTrans")
	};
	// 分段胶囊 Tab（点击滑块滑动）＋ 右侧「恢复默认」
	auto tabRow = makeChild<Ling::Node>();
	tabRow->setFlexDirection(Ling::FlexDirection::Row);
	tabRow->setWidthPercent(100.f);
	tabRow->setAlignItems(Ling::Align::Center);
	tabRow->setMarginBottom(SettingTheme::blockGap);
	segPills = SettingUi::segmentedPills(tabRow, labels, 0,
		[this](int i) { showTab(i); });
	// 胶囊自适应宽度 + 中间弹性空白，把右侧「恢复默认」推到最右
	auto* spacer = tabRow->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);
	spacer->setFlexShrink(1.f);
	// 「恢复默认」与外观页保持一致（Outline 变体：白底 + 边框），非 Ghost
	restoreBtn = SettingUi::btnOutline(tabRow, Lang::get(L"setting.restoreDefault"), [this]() {
		if (tabIndex != 0) return;
		Setting::get()->restoreFunctionScreenshotDefaults();
		rebuildTab(0);
	});
	restoreBtn->setMarginLeft(SettingTheme::sp2);

	// 注：滚动已由外层 WinSetting 的内容 ScrollerBox 统一管理，本页不再自建滚动容器。
	for (int i = 0; i < 5; i++) {
		tabs[i] = makeChild<Ling::Node>();
		tabs[i]->setFlexDirection(Ling::FlexDirection::Column);
		tabs[i]->setWidthPercent(100.f);
	}
	buildBasic(tabs[0]);    SettingUi::trimTrailingGap(tabs[0]);
	buildRecord(tabs[1]);   SettingUi::trimTrailingGap(tabs[1]);
	buildOutput(tabs[2]);   SettingUi::trimTrailingGap(tabs[2]);
	buildOcr(tabs[3]);      SettingUi::trimTrailingGap(tabs[3]);
	buildTranslate(tabs[4]); SettingUi::trimTrailingGap(tabs[4]);
	showTab(0);
}

void WinSettingFeatures::buildBasic(Ling::Node* p)
{
	auto* s = Setting::get();
	// 开机自启 + 以管理员身份运行：双栏（由外观设置 > 软件外观 迁移而来，样式与其他开关一致）。
	{
		auto* g = SettingUi::grid2(p);
		SettingUi::toggleRow(g, Lang::get(L"setting.autoStart"), s->getAutoStart(),
			[](bool on) { Setting::get()->setAutoStart(on); }, true);
		SettingUi::toggleRow(g, Lang::get(L"setting.runAsAdmin"), s->getRunAsAdmin(),
			[](bool on) { Setting::get()->setRunAsAdmin(on); }, true);
		SettingUi::grid2Cells(g);
	}
	SettingUi::groupLabel(p, Lang::get(L"setting.fnGrpCapture"));
	{
		auto* g = SettingUi::grid2(p);
		SettingUi::toggleRow(g, Lang::get(L"setting.findWindowElements"), s->getFindWindowElements(),
			[](bool on) { Setting::get()->setFindWindowElements(on); }, true);
		SettingUi::selectRow(g, Lang::get(L"setting.trayClickAction"),
			{ Lang::get(L"setting.trayClickCapture"), Lang::get(L"setting.trayClickSetting"), Lang::get(L"setting.trayClickNone") }, s->getTrayClickAction(),
			[](int i, const std::wstring&) { Setting::get()->setTrayClickAction(i); }, true);
		// 保存组已取消，其内容并入截图组
		SettingUi::toggleRow(g, Lang::get(L"setting.autoSaveAfterCapture"), s->getAutoSaveAfterCapture(),
			[](bool on) { Setting::get()->setAutoSaveAfterCapture(on); }, true);
		// 名称 + 落地扩展名
		const std::vector<std::wstring> fmts = {
			L"PNG (*.png)", L"JPEG (*.jpg)", L"WebP (*.webp)", L"BMP (*.bmp)"
		};
		int fi = 0;
		for (int i = 0; i < (int)fmts.size(); i++)
			if (fmts[i] == s->getSaveFormat()) fi = i;
		SettingUi::selectRow(g, Lang::get(L"setting.saveFormat"), fmts, fi,
			[fmts](int i, const std::wstring&) {
				if (i >= 0 && i < (int)fmts.size()) Setting::get()->setSaveFormat(fmts[i]);
			}, true);
		SettingUi::toggleRow(g, Lang::get(L"setting.copyAsFile"), s->getCopyAsFile(),
			[](bool on) { Setting::get()->setCopyAsFile(on); }, true);
		SettingUi::toggleRow(g, Lang::get(L"setting.cancelConfirmDialog"), s->getCancelConfirmDialog(),
			[](bool on) { Setting::get()->setCancelConfirmDialog(on); }, true);
		SettingUi::grid2Cells(g);
	}

	SettingUi::groupLabel(p, Lang::get(L"setting.fnGrpPin"));
	{
		auto* g = SettingUi::grid2(p);
		SettingUi::toggleRow(g, Lang::get(L"setting.pinZoomAtCursor"), s->getPinZoomAtCursor(),
			[](bool on) { Setting::get()->setPinZoomAtCursor(on); }, true);
		SettingUi::toggleRow(g, Lang::get(L"setting.pinAutoOcr"), s->getPinAutoOcr(),
			[](bool on) { Setting::get()->setPinAutoOcr(on); }, true);
		SettingUi::toggleRow(g, Lang::get(L"setting.pinAutoFit"), s->getPinAutoFit(),
			[](bool on) { Setting::get()->setPinAutoFit(on); }, true);
		SettingUi::selectRow(g, Lang::get(L"setting.pinDoubleClick"),
			{ Lang::get(L"setting.pinDoubleThumb"), Lang::get(L"setting.pinDoubleClose") }, s->getPinDoubleClickAction(),
			[](int i, const std::wstring&) { Setting::get()->setPinDoubleClickAction(i); }, true);
		SettingUi::grid2Cells(g);
	}

	SettingUi::groupLabel(p, Lang::get(L"setting.fnGrpDemo"));
	{
		auto* g = SettingUi::grid2(p);
		SettingUi::selectRow(g, Lang::get(L"setting.demoDefaultTool"),
			{ Lang::get(L"setting.demoFadeBrush"), Lang::get(L"setting.demoBrush"), Lang::get(L"setting.demoNone") }, s->getDemoDefaultTool(),
			[](int i, const std::wstring&) { Setting::get()->setDemoDefaultTool(i); }, true);
		SettingUi::selectRow(g, Lang::get(L"setting.demoEdge"),
			{ Lang::get(L"setting.demoEdgeSnap"), Lang::get(L"setting.demoEdgeHide") }, s->getDemoEdgeBehavior(),
			[](int i, const std::wstring&) { Setting::get()->setDemoEdgeBehavior(i); }, true);
		SettingUi::grid2Cells(g);
	}
}

void WinSettingFeatures::buildRecord(Ling::Node* p)
{
	auto* s = Setting::get();
	auto* g = SettingUi::grid2(p);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecQuality"),
		{ L"480P", L"720P", L"1080P", L"2K", L"4K" }, s->getVideoQuality(),
		[](int i, const std::wstring&) { Setting::get()->setVideoQuality(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecFps"),
		{ L"24", L"30", L"60" }, s->getVideoFps(),
		[](int i, const std::wstring&) { Setting::get()->setVideoFps(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecGifQuality"),
		{ L"480P", L"720P", L"1080P" }, s->getGifQuality(),
		[](int i, const std::wstring&) { Setting::get()->setGifQuality(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecGifFps"),
		{ L"10", L"15", L"24" }, s->getGifFps(),
		[](int i, const std::wstring&) { Setting::get()->setGifFps(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecGifFormat"),
		{ L"GIF (*.gif)", L"WebP (*.webp)", L"APNG (*.png)" }, s->getGifFormat(),
		[](int i, const std::wstring&) { Setting::get()->setGifFormat(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecMic"),
		{ Lang::get(L"setting.fnRecMicDefault"), Lang::get(L"setting.fnRecMicOff") }, s->getRecordMic(),
		[](int i, const std::wstring&) { Setting::get()->setRecordMic(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecEncoder"),
		{ Lang::get(L"setting.fnRecEncoderAuto"), L"H.264", L"H.265" }, s->getVideoEncoder(),
		[](int i, const std::wstring&) { Setting::get()->setVideoEncoder(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecBitrate"),
		{ Lang::get(L"setting.fnRecBitrateLow"), Lang::get(L"setting.fnRecBitrateMid"), Lang::get(L"setting.fnRecBitrateHigh") }, s->getEncodeSpeed(),
		[](int i, const std::wstring&) { Setting::get()->setEncodeSpeed(i); }, true);
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecHwAccel"), s->getHwAccel(),
		[](bool on) { Setting::get()->setHwAccel(on); }, true);
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecHideToolbar"), s->getHideToolbarInRecord(),
		[](bool on) { Setting::get()->setHideToolbarInRecord(on); }, true);
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecKeystrokes"), s->getShowKeystrokes(),
		[](bool on) { Setting::get()->setShowKeystrokes(on); }, true);
	SettingUi::grid2Cells(g);
}

void WinSettingFeatures::buildOutput(Ling::Node* p)
{
	auto* s = Setting::get();
	const std::wstring defDir = Util::defaultOutputDir().wstring();

	// 截图输出组：手动/自动/焦点窗口 文件名格式 + 截图文件路径
	SettingUi::groupLabel(p, Lang::get(L"setting.screenshotOutput"), Lang::get(L"setting.outputSectionTip"));
	outputRow(p, Lang::get(L"setting.manualSaveFmt"),
		s->getManualSaveFormat(), Setting::defaultManualSaveFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setManualSaveFormat(v); });
	outputRow(p, Lang::get(L"setting.autoSaveFmt"),
		s->getAutoSaveFormat(), Setting::defaultAutoSaveFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setAutoSaveFormat(v); });
	outputRow(p, Lang::get(L"setting.focusedWindowFmt"),
		s->getFocusedWindowFormat(), Setting::defaultFocusedWindowFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setFocusedWindowFormat(v); });
	outputRow(p, Lang::get(L"setting.fnScreenshotDir"), s->getScreenshotDir(), defDir,
		OutputRowKind::Dir,
		[](const std::wstring& v) { Setting::get()->setScreenshotDir(v); });

	// 视频输出组：视频录制文件名格式 + 录屏文件路径
	SettingUi::groupLabel(p, Lang::get(L"setting.videoOutput"), Lang::get(L"setting.outputSectionTip"));
	outputRow(p, Lang::get(L"setting.videoRecordFmt"),
		s->getVideoRecordFormat(), Setting::defaultVideoRecordFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setVideoRecordFormat(v); });
	outputRow(p, Lang::get(L"setting.fnRecordDir"), s->getRecordDir(), defDir,
		OutputRowKind::Dir,
		[](const std::wstring& v) { Setting::get()->setRecordDir(v); });
}

void WinSettingFeatures::buildOcr(Ling::Node* p)
{
	auto* s = Setting::get();
	std::vector<std::wstring> labels;
	std::vector<int> engines;
	std::vector<std::wstring> cloudIds;
	if (OcrModelManager::instance().isInstalled(OcrPackVariant::Official)
		&& OcrRuntimeManager::instance().isInstalled(OcrPackVariant::Official)) {
		labels.push_back(L"PP-OCRv6"); engines.push_back(0); cloudIds.push_back(L"");
	}
	if (OcrModelManager::instance().isInstalled(OcrPackVariant::Embedded)
		&& OcrRuntimeManager::instance().isInstalled(OcrPackVariant::Embedded)) {
		labels.push_back(L"PP-OCRv5"); engines.push_back(1); cloudIds.push_back(L"");
	}
	if (OcrModelManager::instance().isInstalled(OcrPackVariant::StableV4)
		&& OcrRuntimeManager::instance().isInstalled(OcrPackVariant::StableV4)) {
		labels.push_back(L"PP-OCRv4"); engines.push_back(2); cloudIds.push_back(L"");
	}
	// 文本识别模型默认只有「系统 OCR」（Tesseract 已不再提供）
	labels.push_back(Lang::get(L"setting.fnOcrSystem")); engines.push_back(4); cloudIds.push_back(L"");
	for (auto& c : s->getOcrCloudConfigs()) {
		if (!c.isConfigured()) continue;
		labels.push_back(c.displayName());
		engines.push_back(5);
		cloudIds.push_back(c.id);
	}
	int cur = 0;
	for (int i = 0; i < (int)engines.size(); i++) {
		if (engines[i] == s->getOcrEngine()
			&& (engines[i] != 5 || cloudIds[i] == s->getOcrCloudConfigId())) {
			cur = i; break;
		}
	}
	{
		auto* g = SettingUi::grid2(p);
		SettingUi::selectRow(g, Lang::get(L"setting.fnOcrModel"), labels, cur,
			[engines, cloudIds](int i, const std::wstring&) {
				if (i < 0 || i >= (int)engines.size()) return;
				Setting::get()->setOcrEngine(engines[i]);
				Setting::get()->setOcrCloudConfigId(cloudIds[i]);
			}, true);
		SettingUi::grid2Cells(g);
	}

	SettingUi::groupLabel(p, Lang::get(L"setting.fnApiGroup"));
	auto clouds = s->getOcrCloudConfigs();
	for (size_t i = 0; i < clouds.size(); i++) {
		const auto cfg = clouds[i];
		const bool youdao = cfg.provider == L"youdao";

		// 卡片容器：白底 + 1px 描边 + 圆角 8（与文件输出卡片一致，保持整体统一）
		auto* card = p->makeChild<Ling::Node>();
		card->setFlexDirection(Ling::FlexDirection::Column);
		card->setWidthPercent(100.f);
		card->setBg(SettingTheme::card);
		card->setBorder(1.f, SettingTheme::border);
		card->setBorderRadius(SettingTheme::radiusLg);
		card->setPadding(SettingTheme::sp3, SettingTheme::sp3, SettingTheme::sp3, SettingTheme::sp3);
		card->setMarginBottom(SettingTheme::blockGap);

		auto saveField = [i](const std::function<void(OcrCloudConfig&)>& apply) {
			auto list = Setting::get()->getOcrCloudConfigs();
			if (i >= list.size()) return;
			apply(list[i]);
			Setting::get()->setOcrCloudConfigs(list);
		};

		// 首行：左「识图服务」下拉，右「显示名称」输入 + 复制/删除方形图标按钮
		auto* top = card->makeChild<Ling::Node>();
		top->setFlexDirection(Ling::FlexDirection::Row);
		top->setWidthPercent(100.f);
		top->setAlignItems(Ling::Align::FlexEnd);
		YGNodeStyleSetMinWidth(top->node, 0.f);

		auto* svcCol = top->makeChild<Ling::Node>();
		svcCol->setFlexDirection(Ling::FlexDirection::Column);
		svcCol->setFlexGrow(1.f);
		svcCol->setFlexShrink(1.f);
		svcCol->setMarginRight(SettingTheme::sp3);
		YGNodeStyleSetMinWidth(svcCol->node, 0.f);
		auto* svcLab = svcCol->makeChild<Ling::Label>();
		svcLab->setText(Lang::get(L"setting.fnOcrProvider"));
		svcLab->setFontSize(SettingTheme::fontBase);
		svcLab->setColor(SettingTheme::textPrimary);
		svcLab->setMarginBottom(SettingTheme::sp2);
		SettingUi::selectDdl(svcCol,
			{ Lang::get(L"setting.fnProviderBaidu"), Lang::get(L"setting.fnProviderYoudao"), Lang::get(L"setting.fnProviderCustom") },
			youdao ? 1 : (cfg.provider == L"custom" ? 2 : 0),
			[this, i](int idx, const std::wstring&) {
				auto list = Setting::get()->getOcrCloudConfigs();
				if (i >= list.size()) return;
				list[i].provider = idx == 1 ? L"youdao" : idx == 2 ? L"custom" : L"baidu";
				Setting::get()->setOcrCloudConfigs(list);
				deferRebuildTab(3);   // 服务商变化会换字段标签/显隐，整卡重建
			});

		auto* nameCol = top->makeChild<Ling::Node>();
		nameCol->setFlexDirection(Ling::FlexDirection::Column);
		nameCol->setFlexGrow(1.f);
		nameCol->setFlexShrink(1.f);
		YGNodeStyleSetMinWidth(nameCol->node, 0.f);
		auto* nameLab = nameCol->makeChild<Ling::Label>();
		nameLab->setText(Lang::get(L"setting.fnDisplayName"));
		nameLab->setFontSize(SettingTheme::fontBase);
		nameLab->setColor(SettingTheme::textPrimary);
		nameLab->setMarginBottom(SettingTheme::sp2);
		auto* nameRow = nameCol->makeChild<Ling::Node>();
		nameRow->setFlexDirection(Ling::FlexDirection::Row);
		nameRow->setWidthPercent(100.f);
		nameRow->setAlignItems(Ling::Align::FlexEnd);
		YGNodeStyleSetMinWidth(nameRow->node, 0.f);
		{
			auto* nameBox = nameRow->makeChild<Ling::TextBox>();
			nameBox->setHeight(SettingTheme::ctrlH);
			nameBox->setWidthPercent(0.f);
			nameBox->setFlexGrow(1.f);
			nameBox->setFlexShrink(1.f);
			YGNodeStyleSetMinWidth(nameBox->node, 0.f);
			nameBox->setBorderRadius(SettingTheme::radiusCtl);   // 与外层卡片同心（比外框小一档）
			nameBox->setBorder(1.f, SettingTheme::input);
			nameBox->setBg(SettingTheme::card);
			nameBox->setColor(SettingTheme::textPrimary);
			nameBox->setPadding(SettingTheme::sp2, 0, SettingTheme::sp2, 0);
			nameBox->setFontSize(SettingTheme::fontBase);
			nameBox->setVerticalCenter(true);
			nameBox->setPlaceholder(Lang::get(L"setting.fnInputHint"));
			nameBox->setPlaceholderColor(SettingTheme::placeholder);
			nameBox->setText(cfg.name);
			nameBox->onFocusChanged.add([saveField](Ling::TextBox* tb, bool focused) {
				if (!focused) saveField([&](OcrCloudConfig& c) { c.name = tb->getText(); });
			});
		}
		{
			auto* copyBtn = squareIconBtn(nameRow, Icon::Copy, [this, i, cfg]() {
				auto list = Setting::get()->getOcrCloudConfigs();
				if (i >= list.size()) return;
				OcrCloudConfig dup = cfg;
				dup.id = newId();
				dup.name = cfg.displayName() + L" 2";
				list.insert(list.begin() + (int)i + 1, dup);
				Setting::get()->setOcrCloudConfigs(list);
				deferRebuildTab(3);
			});
			copyBtn->setMarginLeft(SettingTheme::sp2);
			auto* delBtn = squareIconBtn(nameRow, Icon::Remove, [this, i]() {
				auto list = Setting::get()->getOcrCloudConfigs();
				if (i >= list.size()) return;
				list.erase(list.begin() + (int)i);
				Setting::get()->setOcrCloudConfigs(list);
				deferRebuildTab(3);
			});
			delBtn->setMarginLeft(SettingTheme::sp2);
		}

		// 次行：App ID / API Key 双栏（有道时隐藏 API Key，只留「应用 ID」）
		auto* row2 = card->makeChild<Ling::Node>();
		row2->setFlexDirection(Ling::FlexDirection::Row);
		row2->setWidthPercent(100.f);
		row2->setMarginTop(SettingTheme::sp3);
		YGNodeStyleSetMinWidth(row2->node, 0.f);
		auto* appIdCol = apiField(row2, Lang::get(youdao ? L"setting.fnAppIdStar" : L"setting.fnAppIdOptional"),
			cfg.appId, [saveField](const std::wstring& v) {
				saveField([&](OcrCloudConfig& c) { c.appId = v; });
			});
		appIdCol->setFlexGrow(1.f);
		if (!youdao) {
			auto* keyCol = apiField(row2, Lang::get(L"setting.fnApiKeyStar"), cfg.apiKey,
				[saveField](const std::wstring& v) {
					saveField([&](OcrCloudConfig& c) { c.apiKey = v; });
				});
			keyCol->setFlexGrow(1.f);
			keyCol->setMarginLeft(SettingTheme::sp3);
		}

		// 第三行：Secret Key 独占整行
		auto* secretCol = apiField(card, Lang::get(youdao ? L"setting.fnSecretYd" : L"setting.fnSecretStar"),
			cfg.secret, [saveField](const std::wstring& v) {
				saveField([&](OcrCloudConfig& c) { c.secret = v; });
			});
		secretCol->setWidthPercent(100.f);
		secretCol->setMarginTop(SettingTheme::sp3);

		// 帮助文案（有道/百度各一套）
		auto* hint = card->makeChild<Ling::Label>();
		hint->setText(Lang::get(youdao ? L"setting.fnOcrHintYoudao" : L"setting.fnOcrHintBaidu"));
		hint->setFontSize(SettingTheme::fontSm);
		hint->setColor(SettingTheme::textSecondary);
		hint->setMarginTop(SettingTheme::sp3);
	}
	SettingUi::dashedAdd(p, Lang::get(L"setting.fnAddConfig"), [this]() {
		auto list = Setting::get()->getOcrCloudConfigs();
		OcrCloudConfig c;
		c.id = newId();
		c.provider = L"baidu";
		list.push_back(c);
		Setting::get()->setOcrCloudConfigs(list);
		deferRebuildTab(3);
	});
}

void WinSettingFeatures::buildTranslate(Ling::Node* p)
{
	auto* s = Setting::get();
	// 只提供非 AI 的翻译引擎：在线免费接口（微软/谷歌/有道/百度）+ 离线 Bergamot。
	// （已移除 AI 模型翻译与「自定义 API」配置：不再有 URL / Key / 模型 / 提示词等设置。）
	struct Item { const wchar_t* id; const wchar_t* labKey; };
	const Item items[] = {
		{ L"microsoft", L"setting.fnTransMs" },
		{ L"google",    L"setting.fnTransGoogle" },
		{ L"youdao",    L"setting.fnTransYoudao" },
		{ L"baidu",     L"setting.fnTransBaidu" },
		{ L"offline",   L"setting.fnTransOffline" },
	};
	std::vector<std::wstring> svcLabs, svcIds;
	for (auto& it : items) {
		svcLabs.push_back(Lang::get(it.labKey));
		svcIds.push_back(it.id);
	}
	int si = 0;
	auto curP = s->getTranslateProvider();
	for (int i = 0; i < (int)svcIds.size(); i++)
		if (_wcsicmp(svcIds[i].c_str(), curP.c_str()) == 0) si = i;

	// 翻译服务 + 字典模式：双栏
	auto* g = SettingUi::grid2(p);
	SettingUi::selectRow(g, Lang::get(L"setting.fnTransService"), svcLabs, si,
		[svcIds](int i, const std::wstring&) {
			if (i >= 0 && i < (int)svcIds.size())
				Setting::get()->setTranslateProvider(svcIds[i]);
		}, true);
	SettingUi::toggleRow(g, Lang::get(L"setting.fnDictMode"), s->getTranslateDictMode(),
		[](bool on) { Setting::get()->setTranslateDictMode(on); }, true);
	SettingUi::grid2Cells(g);
}
