#include "pch.h"
#include "WinSettingFeatures.h"
#include "SettingWidgets.h"
#include "SettingTheme.h"
#include "../Setting.h"
#include "../Lang.h"
#include "../Util.h"
#include "../App.h"
#include "../Tool/IconCodes.h"
#include "../Ocr/OcrModelManager.h"
#include "../Ocr/OcrRuntimeManager.h"
#include "../Ocr/OcrPackVariant.h"
#include "../Ocr/LwPpOcrEngine.h"
#include "../Translate/TranslateTypes.h"
#include "WinSetting.h"
#include <shobjidl.h>
#include <commdlg.h>

namespace {
	// 某个子 Tab 是否仍是出厂值：0 基本功能 / 1 视频录制 / 2 文件输出 / 3 文本识别 / 4 翻译
	//（与 build() 里给 segmentedPills 的标签顺序一致，也与 restoreBtn 的动作分派一致）
	bool functionAtDefaults(int tab)
	{
		auto* s = Setting::get();
		switch (tab) {
		case 1:  return s->functionRecordAtDefaults();
		case 2:  return s->functionOutputAtDefaults();
		case 3:  return s->functionOcrAtDefaults();
		case 4:  return s->functionTranslateAtDefaults();
		default: return s->functionScreenshotAtDefaults();
		}
	}

	// 色号文本（#RRGGBB）：高亮色块旁显示用
	std::wstring colorCode(UINT32 rgba)
	{
		return wfmt(L"#%02X%02X%02X",
			(rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu);
	}

	// 解析键入的色号：接受 #RRGGBB / RRGGBB（忽略空白）；非法则返回 fallback
	UINT32 parseColorCode(const std::wstring& text, UINT32 fallback)
	{
		std::wstring s;
		for (wchar_t c : text) if (!iswspace(c)) s += c;
		if (!s.empty() && s[0] == L'#') s.erase(0, 1);
		if (s.size() != 6) return fallback;
		for (wchar_t c : s) if (!iswxdigit(c)) return fallback;
		return ((UINT32)wcstoul(s.c_str(), nullptr, 16) & 0xFFFFFFu) << 8 | 0xFFu;
	}

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

}

WinSettingFeatures::WinSettingFeatures(Ling::WinBase* parent) : Ling::Node(parent)
{
	setFlexDirection(Ling::FlexDirection::Column);
	build();
	// 悬停预览只在窗口级鼠标移动里更新（页面重建不影响订阅）
	mouseMoveTok = win->onMouseMove.add([this](POINT pos) {
		updateMouseHlPreview(pos);
		// 各行的改动不触发本页重建，「恢复默认」的置灰借这个高频回调同步：
		// 值没变时 setColor 会被 Text 内部的相等判断吃掉，不产生多余重绘
		syncRestoreState();
	});
}

WinSettingFeatures::~WinSettingFeatures()
{
	SettingUi::closePopup(win);
	win->onMouseMove.remove(mouseMoveTok);
	if (mouseHlPreview) {
		if (win && win->body) win->body->removeChild(mouseHlPreview);
		mouseHlPreview = nullptr;
	}
}

// 「鼠标高亮提醒」的悬停预览：命中该行时把圆跟着光标走，离开就收起。
// 尺寸与透明度取与实际绘制相同的值（见 WinCap::mouseHlRadius / kMouseHlAlpha）。
void WinSettingFeatures::updateMouseHlPreview(const POINT& pos)
{
	if (!mouseHlPreview) return;
	// 只在「视频录制」Tab 的卡片中部空白处触发：标题文本、色块/色号/开关都不算
	//（其它 Tab 下该行是隐藏的，坐标判定不可靠，故一并排除）
	const bool inArea = mouseHlArea && mouseHlArea->isPosIn(pos)
		&& !(mouseHlLabel && mouseHlLabel->isPosIn(pos));
	if (tabIndex != 1 || !inArea) {
		mouseHlPreview->hide();
		return;
	}
	constexpr float kD = 42.f;   // 直径 = 半径 21 逻辑像素 × 2
	// 事件坐标是物理像素，而 setPosition 收的是逻辑像素（内部 ×dpi）；圆心同样取光标本体中心
	const float k = (win && win->dpi > 0.f) ? win->dpi : 1.f;
	float ox = 0.f, oy = 0.f;
	Util::cursorBodyOffset(ox, oy);
	mouseHlPreview->setPosition(Ling::Edge::Left, ((float)pos.x + ox) / k - kD * 0.5f);
	mouseHlPreview->setPosition(Ling::Edge::Top, ((float)pos.y + oy) / k - kD * 0.5f);
	mouseHlPreview->show();
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
	syncRestoreState();
}

// 「恢复默认」的置灰：当前子页各项都还是出厂值 → 灰字（点击也不做事，见按钮回调）。
// 与「外观设置 > 工具栏外观」的 tbRefreshRestore 同款口径。
void WinSettingFeatures::syncRestoreState()
{
	if (!restoreBtn) return;
	const bool grey = functionAtDefaults(tabIndex);
	restoreBtn->setColor(grey ? SettingTheme::textMuted : SettingTheme::textPrimary);
	restoreBtn->setHoverColor(grey ? SettingTheme::textMuted : SettingTheme::textPrimary);
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
	// 「恢复默认」与外观页保持一致（Outline 变体：白底 + 边框），非 Ghost。
	// 作用域 = **当前子 Tab**：把该子页的每一项写回出厂值，再整页重建以刷新控件；
	// 该子页没改过时按钮置灰、点击不做事（见 syncRestoreState）。
	restoreBtn = SettingUi::btnOutline(tabRow, Lang::get(L"setting.restoreDefault"), [this]() {
		if (functionAtDefaults(tabIndex)) return;
		auto* s = Setting::get();
		switch (tabIndex) {
		case 0:  s->restoreFunctionScreenshotDefaults(); break;
		case 1:  s->restoreFunctionRecordDefaults();     break;
		case 2:  s->restoreFunctionOutputDefaults();     break;
		case 3:  s->restoreFunctionOcrDefaults();        break;
		default: s->restoreFunctionTranslateDefaults();  break;
		}
		rebuildTab(tabIndex);
		syncRestoreState();
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

		// 「以管理员身份运行」不用 toggleRow：它的开关状态由组件内部自己翻转，一旦遇到
		// "锁定为开" 或 "取消后回滚"，内部状态就和视觉对不上，表现为要点两下才轮到"关"。
		// 这里开关恒由 getRunAsAdmin() 决定，点击后按真实值重画，不依赖组件内部状态。
		{
			auto r = SettingUi::optionRow(g, Lang::get(L"setting.runAsAdmin"), L"", true, SettingUi::kToggleH);
			auto* btn = r.ctrlBox->makeChild<Ling::Button>();
			SettingUi::styleToggle(btn, s->getRunAsAdmin());
			const bool locked = Util::isBuiltinAdminAccount();
			btn->onClick.add([this, btn, locked](Ling::Button*) {
				if (locked) {
					// 内置 Administrator 账户天然就是管理员：开关锁定为开，点它只弹通知、不翻转
					SettingUi::ConfirmDialog::ask(this->win, Lang::get(L"setting.runAsAdminLockedTip"),
						[](bool) {});
					return;
				}
				auto* set = Setting::get();
				if (set->getRunAsAdmin()) {                        // 开 → 关：只落配置
					set->setRunAsAdmin(false);
					SettingUi::styleToggle(btn, set->getRunAsAdmin());
					return;
				}
				if (Util::isElevated()) {                          // 开且已是管理员：只落配置
					set->setRunAsAdmin(true);
					SettingUi::styleToggle(btn, set->getRunAsAdmin());
					return;
				}
				// 开且当前是普通权限：确认后立即以管理员身份重启
				SettingUi::ConfirmDialog::ask(this->win, Lang::get(L"setting.runAsAdminConfirm"),
					[btn](bool ok) {
						if (!ok) return;                           // 取消：状态没变，视觉本就不用动
						Setting::get()->setRunAsAdmin(true);
						SettingUi::styleToggle(btn, Setting::get()->getRunAsAdmin());
						// 丢到调度队列再重启：别在弹窗自身的点击回调里同步退出进程
						if (auto* a = Ling::App::get()) a->dq.TryEnqueue([] { ::App::restartAsAdmin(); });
						else ::App::restartAsAdmin();
					});
			});
		}
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
			{ Lang::get(L"setting.demoEdgeSnap"), Lang::get(L"setting.demoEdgeHide"), Lang::get(L"setting.demoNone") },
			std::clamp(s->getDemoEdgeBehavior(), 0, 2),
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
		{ L"24", L"30", L"60", L"15", L"10", L"5" }, s->getVideoFps(),
		[](int i, const std::wstring&) { Setting::get()->setVideoFps(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecGifQuality"),
		{ L"480P", L"720P", L"1080P" }, s->getGifQuality(),
		[](int i, const std::wstring&) { Setting::get()->setGifQuality(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecGifFps"),
		{ L"10", L"15", L"24" }, s->getGifFps(),
		[](int i, const std::wstring&) { Setting::get()->setGifFps(i); }, true);
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecGifFormat"),
		{ L"GIF (*.gif)", L"WebP (*.webp)", L"APNG (*.png)" }, std::clamp(s->getGifFormat(), 0, 2),
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
	// 录屏默认开启鼠标穿透（开录即穿，录制中可临时开关）
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecMousePierce"), L"", s->getRecordDefaultPierce(),
		[](bool on) { Setting::get()->setRecordDefaultPierce(on); }, true,
		Lang::get(L"setting.fnRecMousePierceTip"));
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecHideToolbar"), s->getHideToolbarInRecord(),
		[](bool on) { Setting::get()->setHideToolbarInRecord(on); }, true);
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecKeystrokes"), s->getShowKeystrokes(),
		[](bool on) { Setting::get()->setShowKeystrokes(on); }, true);
	// 行为见 ToolVideo：手动把录屏工具栏拖到屏幕上/下边缘后自动缩进边缘，悬停滑出
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecToolbarPeek"), L"", s->getRecordToolbarPeek(),
		[](bool on) { Setting::get()->setRecordToolbarPeek(on); }, true,
		Lang::get(L"setting.fnRecToolbarPeekTip"));
	// 行为见 WinCap::updateSelFollowCursor（录制中 + 穿透态）
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecSelFollowMouse"), s->getSelFollowMouse(),
		[](bool on) { Setting::get()->setSelFollowMouse(on); }, true);
	// 行为见 WinCap::paintMouseHighlight；色块 + 色号点击弹系统调色板（与「托盘自定义颜色」同款）
	{
		auto r = SettingUi::optionRow(g, Lang::get(L"setting.fnRecMouseHighlight"), L"", true, SettingUi::kToggleH);
		// 操作提示排在标题右侧的空白处，与标题同一行（不占标题下方、不动 label 区布局）：
		// 挂到标题行上，吞掉剩余宽度并在其中居中。Label 内部是纵向容器，横向居中要用 alignItems
		//（justifyContent 管的是纵向主轴，对单行文本无效果）。
		{
			auto* hint = r.labRow->makeChild<Ling::Label>();
			hint->setText(Lang::get(L"setting.fnRecMouseHighlightHint"));
			hint->setFontSize(SettingTheme::fontSm);
			hint->setColor(SettingTheme::border);   // 与卡片描边同色：能看见即可，不抢主体
			hint->setFlexGrow(1.f);
			hint->setFlexShrink(1.f);
			YGNodeStyleSetMinWidth(hint->node, 0.f);
			hint->setAlignItems(Ling::Align::Center);
		}
		// 悬停预览圆：挂在 win->body 上（窗口坐标），跟着光标走；重建本页时先撤旧的
		if (mouseHlPreview && win && win->body) {
			win->body->removeChild(mouseHlPreview);
			mouseHlPreview = nullptr;
		}
		auto* hl = win->body->makeChild<Ling::Node>();
		hl->setPositionType(Ling::Position::Absolute);
		hl->setSize(42.f, 42.f);
		hl->setBorderRadius(21.f);
		hl->setBg((s->getMouseHighlightColor() & 0xFFFFFF00u) | 0x59u);   // 与绘制同透明度
		hl->hide();
		mouseHlPreview = hl;
		mouseHlArea = r.labelBox;   // 中部空白（标题右侧到控件之间）才算触发区
		mouseHlLabel = r.label;     // 标题文本本身不算

		// 色块（点击弹系统调色板）与色号输入框各自独立：色号可直接键入
		auto* swatch = r.ctrlBox->makeChild<Ling::Button>();
		swatch->setSize(20.f, 20.f);
		swatch->setBorderRadius(4.f);
		swatch->setBorder(1.f, SettingTheme::input);
		swatch->setBg(s->getMouseHighlightColor());
		swatch->setHoverBg(s->getMouseHighlightColor());   // 悬停不改底色（否则会盖掉颜色本身）
		swatch->setText(L"");
		swatch->setMarginRight(6.f);

		auto* code = r.ctrlBox->makeChild<Ling::TextBox>();
		code->setWidth(84.f);
		code->setHeight(SettingTheme::ctrlH);
		code->setBorderRadius(SettingTheme::radiusCtl);
		code->setBg(SettingTheme::inputBg);
		code->setPadding(SettingTheme::sp2, SettingTheme::sp1, SettingTheme::sp2, SettingTheme::sp1);
		code->setFontSize(SettingTheme::fontSm);
		code->setVerticalCenter(true);
		code->setPlaceholder(L"#RRGGBB");
		code->setPlaceholderColor(SettingTheme::placeholder);
		code->setText(colorCode(s->getMouseHighlightColor()));
		code->setMarginRight(SettingTheme::sp3);

		// 常态不描边、文字走灰；悬停或编辑中才出现描边并把文字提亮
		auto hovered = std::make_shared<bool>(false);
		auto styleCodeBox = [code, hovered](bool focused) {
			const bool framed = *hovered || focused;
			code->setBorder(framed ? 1.f : 0.f, framed ? SettingTheme::input : 0u);
			code->setColor(framed ? SettingTheme::textPrimary : SettingTheme::textMuted);
		};
		styleCodeBox(code->isFocused());
		code->onEnter.add([code, styleCodeBox, hovered](Ling::TextBox*) {
			*hovered = true; styleCodeBox(code->isFocused());
		});
		code->onLeave.add([code, styleCodeBox, hovered](Ling::TextBox*) {
			*hovered = false; styleCodeBox(code->isFocused());
		});

		// 两处入口共用同一段落值逻辑：色块选色、色号键入（回车或失焦提交）
		auto applyColor = [this, swatch, code](UINT32 v) {
			Setting::get()->setMouseHighlightColor(v);
			swatch->setBg(v);
			code->setText(colorCode(v));
			if (mouseHlPreview) mouseHlPreview->setBg((v & 0xFFFFFF00u) | 0x59u);
		};
		const HWND owner = win ? win->hwnd : nullptr;   // 调色板以设置窗为宿主，避免落到设置窗背后
		swatch->onClick.add([applyColor, owner](Ling::Button*) {
			const UINT32 cur = Setting::get()->getMouseHighlightColor();
			static COLORREF cust[16]{};
			CHOOSECOLORW cc{};
			cc.lStructSize = sizeof(cc);
			cc.hwndOwner = owner;
			cc.lpCustColors = cust;
			cc.rgbResult = RGB((cur >> 24) & 0xFFu, (cur >> 16) & 0xFFu, (cur >> 8) & 0xFFu);
			cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
			if (!ChooseColorW(&cc)) return;
			applyColor(((UINT32)GetRValue(cc.rgbResult) << 24)
				| ((UINT32)GetGValue(cc.rgbResult) << 16)
				| ((UINT32)GetBValue(cc.rgbResult) << 8) | 0xFFu);
		});
		code->onEnter.add([applyColor, code](Ling::TextBox*) {
			applyColor(parseColorCode(code->getText(), Setting::get()->getMouseHighlightColor()));
		});
		code->onFocusChanged.add([applyColor, styleCodeBox](Ling::TextBox* tb, bool focused) {
			styleCodeBox(focused);
			if (focused) return;
			// 失焦：非法输入回落到当前值（applyColor 会把文本改回规范写法）
			applyColor(parseColorCode(tb->getText(), Setting::get()->getMouseHighlightColor()));
		});

		auto* sw = r.ctrlBox->makeChild<Ling::Button>();
		SettingUi::styleToggle(sw, s->getMouseHighlight());
		bool on0 = s->getMouseHighlight();
		sw->onClick.add([sw, on0](Ling::Button*) mutable {
			on0 = !on0;
			SettingUi::styleToggle(sw, on0);
			Setting::get()->setMouseHighlight(on0);
		});
	}
	SettingUi::grid2Cells(g);
}

void WinSettingFeatures::buildOutput(Ling::Node* p)
{
	auto* s = Setting::get();
	const std::wstring defDir = Util::defaultOutputDir().wstring();

	// 顶部：历史相关——文件保留天数 / 停用截图历史
	{
		auto* g = SettingUi::grid2(p);
		const std::vector<std::wstring> retOpts = {
			Lang::get(L"setting.retention1d"),
			Lang::get(L"setting.retention3d"),
			Lang::get(L"setting.retention7d"),
			Lang::get(L"setting.retention30d"),
			Lang::get(L"setting.retentionForever"),
		};
		static const int kRetDays[] = { 1, 3, 7, 30, -1 };
		int retIdx = 4;   // 认不出的值按「永久」
		for (int i = 0; i < 5; ++i)
			if (kRetDays[i] == s->getHistoryRetentionDays()) { retIdx = i; break; }

		SettingUi::selectRow(g, Lang::get(L"setting.retentionDays"), retOpts, retIdx,
			[](int i, const std::wstring&) {
				static const int d[] = { 1, 3, 7, 30, -1 };
				Setting::get()->setHistoryRetentionDays(d[i < 0 || i > 4 ? 4 : i]);
				Setting::get()->pruneExpiredHistory();
			}, true);
		SettingUi::toggleRow(g, Lang::get(L"setting.disableHistory"), s->getDisableCaptureHistory(),
			[](bool on) {
				Setting::get()->setDisableCaptureHistory(on);
				WinSetting::applyHistoryMenuVisibility();
			}, true);
		SettingUi::grid2Cells(g);
	}

	// 截图输出组：手动/自动 文件名格式 + 截图文件路径
	SettingUi::groupLabel(p, Lang::get(L"setting.screenshotOutput"), Lang::get(L"setting.outputSectionTip"));
	outputRow(p, Lang::get(L"setting.manualSaveFmt"),
		s->getManualSaveFormat(), Setting::defaultManualSaveFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setManualSaveFormat(v); });
	outputRow(p, Lang::get(L"setting.autoSaveFmt"),
		s->getAutoSaveFormat(), Setting::defaultAutoSaveFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setAutoSaveFormat(v); });
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
	// Lite 版不提供云端 / API 识图：只保留本地 PP-OCRv6 与系统 OCR。
	// PP-OCRv6 有轻量/标准两档，已装哪档就在「模型规格」里列出哪档。
	std::vector<OcrPackVariant> installed;
	for (int i = 0; i < OcrPack::kCount; ++i) {
		const auto v = OcrPack::clampVariant(i);
		if (OcrModelManager::instance().isInstalled(v)
			&& OcrRuntimeManager::instance().isInstalled(v))
			installed.push_back(v);
	}

	std::vector<std::wstring> labels;
	std::vector<int> engines;
	if (!installed.empty()) { labels.push_back(L"PP-OCRv6"); engines.push_back(0); }
	// 文本识别模型默认只有「系统 OCR」
	labels.push_back(Lang::get(L"setting.fnOcrSystem")); engines.push_back(4);
	int cur = 0;
	for (int i = 0; i < (int)engines.size(); i++) {
		if (engines[i] == s->getOcrEngine()) { cur = i; break; }
	}
	{
		// 固定 50% 列容器：每列 = 半栏 − 半个列距，落单时也保持半宽
		// （grid2Cells 按 flex 分配，单项会被拉满整行）
		auto makeRow = [p]() {
			auto* row = p->makeChild<Ling::Node>();
			row->setFlexDirection(Ling::FlexDirection::Row);
			row->setWidthPercent(100.f);
			return row;
		};
		auto makeCol = [](Ling::Node* row, bool right) {
			auto* col = row->makeChild<Ling::Node>();
			col->setWidthPercent(50.f);
			col->setFlexDirection(Ling::FlexDirection::Column);
			if (right) col->setPaddingLeft(SettingTheme::optionGap * 0.5f);
			else col->setPaddingRight(SettingTheme::optionGap * 0.5f);
			return col;
		};
		auto addResultRow = [s](Ling::Node* parent) {
			SettingUi::selectRow(parent, Lang::get(L"setting.fnOcrResultMode"),
				{ Lang::get(L"setting.fnOcrResultOverlay"), Lang::get(L"setting.fnOcrResultReflow") },
				std::clamp(s->getOcrResultMode(), 0, 1),
				[](int i, const std::wstring&) { Setting::get()->setOcrResultMode(i); }, true);
		};

		const bool tierShown = (s->getOcrEngine() == 0 && !installed.empty());

		auto* row1 = makeRow();
		SettingUi::selectRow(makeCol(row1, false), Lang::get(L"setting.fnOcrModel"), labels, cur,
			[this, engines](int i, const std::wstring&) {
				if (i < 0 || i >= (int)engines.size()) return;
				const bool changed = Setting::get()->getOcrEngine() != engines[i];
				Setting::get()->setOcrEngine(engines[i]);
				// 切到系统 OCR 时「模型规格」要收起来（反之亦然），按需重建本页
				if (changed) this->deferRebuildTab(3);
			}, true);

		// 模型规格：只有选中 PP-OCRv6 且装了模型才出现；系统 OCR 不需要模型，不显示
		if (tierShown) {
			// 识别结果版式放第一行右列（与文本识别模型并排）
			addResultRow(makeCol(row1, true));

			// 模型规格独占第二行左列（半栏）
			std::vector<std::wstring> tiers;
			std::vector<int> tierIds;
			for (auto v : installed) {
				tiers.push_back(OcrPack::tierLabel(v));
				tierIds.push_back((int)v);
			}
			int curTier = 0;
			for (int i = 0; i < (int)tierIds.size(); ++i)
				if (tierIds[i] == s->getOcrPackVariant()) { curTier = i; break; }
			// 「?」放在标题「模型规格」右侧（optionRow 的 helpText 就是干这个的）
			auto tierRow = SettingUi::optionRow(makeCol(makeRow(), false), Lang::get(L"setting.fnOcrTier"), L"", true,
				SettingTheme::ctrlH, Lang::get(L"setting.plugOcrTierTip"));
			auto* ddl = SettingUi::selectDdl(tierRow.ctrlBox, tiers, curTier,
				[tierIds](int i, const std::wstring&) {
					if (i < 0 || i >= (int)tierIds.size()) return;
					Setting::get()->setOcrPackVariant(tierIds[i]);
					LwPpOcrEngine::release();   // 换档要重建引擎
				});
			ddl->setWidth(SettingTheme::dropdownWidth);
			ddl->setFlexShrink(0.f);
		}
		else {
			// 系统 OCR：没有模型规格，识别结果版式直接并到第一行右列
			addResultRow(makeCol(row1, true));
		}
	}
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
