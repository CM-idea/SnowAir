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
#include "../Plugin/PluginHost.h"
#include "WinCap.h"
#include "WinSetting.h"
#include "CapVideo.h"
#include "CursorImage.h"
#include "MagLens.h"
#include <shobjidl.h>
#include <commdlg.h>
#include <cmath>

namespace {
	// 半栏布局：整行 100%，列固定 50% − 半个列距；落单时也保持半宽
	//（grid2Cells 按 flex 分配，单项会被拉满整行）
	Ling::Node* halfRow(Ling::Node* parent)
	{
		auto* row = parent->makeChild<Ling::Node>();
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setWidthPercent(100.f);
		return row;
	}
	Ling::Node* halfCol(Ling::Node* row, bool right)
	{
		auto* col = row->makeChild<Ling::Node>();
		col->setWidthPercent(50.f);
		col->setFlexDirection(Ling::FlexDirection::Column);
		if (right) col->setPaddingLeft(SettingTheme::optionGap * 0.5f);
		else col->setPaddingRight(SettingTheme::optionGap * 0.5f);
		return col;
	}
	// 某个子 Tab 是否仍是出厂值：0 基本功能 / 1 视频录制 / 2 文件输出 / 3 识别模型 / 4 其他功能
	//（与 build() 里给 segmentedPills 的标签顺序一致，也与 restoreBtn 的动作分派一致）
	bool functionAtDefaults(int tab)
	{
		auto* s = Setting::get();
		switch (tab) {
		case 1:  return s->functionRecordAtDefaults();
		case 2:  return s->functionOutputAtDefaults();
		case 3:  return s->functionOcrAtDefaults() && s->functionTranslateAtDefaults();
		case 4:  return s->functionOtherAtDefaults();
		default: return s->functionScreenshotAtDefaults();
		}
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
	// 「文件输出」标题列宽：按当前语言实际文字宽度算（见 buildOutput），中文比英文短，
	// 输入框能多占一点；所有行仍按同一宽度左缘对齐。
	float gOutputLabelW{ 150.f };

	// 「删除 / 重置」类（垃圾桶图标）按钮：红底（20% 红）+ 红图标
	constexpr uint32_t kDangerRedBg{ 0xFF383C33 };   // #FF383C @ 20%
	constexpr uint32_t kDangerRed{ 0xFF383CFF };     // #FF383C
	bool isTrashGlyph(const wchar_t* glyph) { return glyph && _wcsicmp(glyph, Icon::Remove) == 0; }

	// 「鼠标轨迹效果」下拉的显示顺序：高亮 / 线条 / 局部放大 / 无。
	int trailEffectFromIndex(int i)
	{
		static const int kOrder[]{ Setting::kMouseTrailHighlight, Setting::kMouseTrailLine,
			Setting::kMouseTrailMagnifier, Setting::kMouseTrailNone };
		return kOrder[std::clamp(i, 0, 3)];
	}
	int trailIndexFromEffect(int effect)
	{
		static const int kOrder[]{ Setting::kMouseTrailHighlight, Setting::kMouseTrailLine,
			Setting::kMouseTrailMagnifier, Setting::kMouseTrailNone };
		for (int i = 0; i < 4; ++i)
			if (kOrder[i] == effect) return i;
		return 3;
	}

	// 「鼠标单击效果」下拉的显示顺序：波纹 / 放大 / 组合 / 无。
	// 存储值保持 0/1/2（无）/3（组合），不能直接拿下标当配置值（旧配置 2 = 无）。
	int clickEffectFromIndex(int i)
	{
		static const int kOrder[]{ Setting::kMouseClickRipple, Setting::kMouseClickZoom,
			Setting::kMouseClickCombo, Setting::kMouseClickNone };
		return kOrder[std::clamp(i, 0, 3)];
	}
	int clickIndexFromEffect(int effect)
	{
		static const int kOrder[]{ Setting::kMouseClickRipple, Setting::kMouseClickZoom,
			Setting::kMouseClickCombo, Setting::kMouseClickNone };
		for (int i = 0; i < 4; ++i)
			if (kOrder[i] == effect) return i;
		return 3;
	}

	// 取色色块：浅色（含白色）在深色卡片上补一圈描边，其余颜色不描边（避免圆角灰锯齿）
	void styleColorSwatch(Ling::Button* b, UINT32 v)
	{
		if (!b) return;
		const float lum = (0.299f * ((v >> 24) & 0xFF) + 0.587f * ((v >> 16) & 0xFF)
			+ 0.114f * ((v >> 8) & 0xFF)) / 255.f;
		if (lum > 0.82f) b->setBorder(1.f, SettingTheme::input);
		else b->setBorder(0.f, 0);
		b->setBg(v);
		b->setHoverBg(v);   // 悬停不改底色（否则会盖掉颜色本身）
	}

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
		std::function<void(const std::wstring&)> onCommit,
		const wchar_t* tipKey = L"setting.outputDirTip",
		const wchar_t* helpKey = nullptr)
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

		// 标题 + 可选「?」：固定宽度保证各行输入框左缘对齐，问号紧跟标题文字
		auto* titleRow = row->makeChild<Ling::Node>();
		titleRow->setWidth(gOutputLabelW);
		titleRow->setFlexShrink(0.f);
		titleRow->setFlexDirection(Ling::FlexDirection::Row);
		titleRow->setAlignItems(Ling::Align::Center);

		auto* lab = titleRow->makeChild<Ling::Label>();
		lab->setText(label);
		lab->setFontSize(SettingTheme::fontBase);
		lab->setColor(SettingTheme::textPrimary);
		lab->setFlexShrink(0.f);
		if (helpKey) SettingUi::helpTipButton(titleRow, Lang::get(helpKey));

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
		box->setScrollBarVisible(false);   // 高度不够时靠加高，不出现滚动条

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
				box->fitHeightToText(SettingTheme::ctrlH * 3.5f, SettingTheme::ctrlH);   // 长路径折行显示，不加滚动条
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
			box->setHeightAndReflow(SettingTheme::ctrlH);   // 失焦收回单行高度
			if (doCommit && onCommit) onCommit(box->getText());
		});
		box->onTextChanged.add([box, autoFilled, syncButtons](Ling::TextBox*, const std::wstring&) {
			*autoFilled = false;   // 内容被改过 → 不再是"自动填入的默认值"
			syncButtons();
			if (box->isFocused()) box->fitHeightToText(SettingTheme::ctrlH * 3.5f, SettingTheme::ctrlH);
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
					tip->showFor(box, Lang::get(L"setting.previewPrefix") + name, true);
				});
			}
			else if (tipKey && *tipKey) {
				box->onEnter.add([box, tip, tipKey](Ling::TextBox*) {
					tip->showFor(box, Lang::get(tipKey));
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
	initPreviewOverlay();
	// 预览只在窗口级鼠标事件里更新（页面重建不影响订阅）
	mouseMoveTok = win->onMouseMove.add([this](POINT pos) {
		updatePreviewOnMouseMove(pos);
		// 各行的改动不触发本页重建，「恢复默认」的置灰借这个高频回调同步：
		// 值没变时 setColor 会被 Text 内部的相等判断吃掉，不产生多余重绘
		syncRestoreState();
	});
	mouseDownTok_ = win->onMouseDown.add([this](POINT pos, bool isRight) {
		if (!isRight) updatePreviewOnMouseDown(pos);
	});
	timerTok_ = win->onTimer.add([this](UINT id) {
		if (id == kPreviewTimerId) updatePreviewFrame();
	});
}

WinSettingFeatures::~WinSettingFeatures()
{
	SettingUi::closePopup(win);
	MagLens::get().hide();
	restorePreviewCursor();
	if (!win) return;
	win->onMouseMove.remove(mouseMoveTok);
	win->onMouseDown.remove(mouseDownTok_);
	win->onTimer.remove(timerTok_);
	win->killTimer(kPreviewTimerId);
	if (previewCanvas_) {
		if (win->body) win->body->removeChild(previewCanvas_);
		previewCanvas_ = nullptr;
	}
}

// 预览层：盖在设置页最上面的透明 Canvas。几何与 WinCap 里的录制绘制保持一致，
// 所以停悬/单击看到的就是录进视频里的样子。
void WinSettingFeatures::initPreviewOverlay()
{
	if (!win || !win->body || previewCanvas_) return;
	previewCanvas_ = win->body->makeChild<Ling::Canvas>();
	previewCanvas_->setPositionType(Ling::Position::Absolute);
	previewCanvas_->setPosition(Ling::Edge::Left, 0.f);
	previewCanvas_->setPosition(Ling::Edge::Top, 0.f);
	previewCanvas_->setPosition(Ling::Edge::Right, 0.f);
	previewCanvas_->setPosition(Ling::Edge::Bottom, 0.f);
}

// 悬停轨迹行：
//   高亮提醒 → 圆跟着光标走（同色同径，与录制时一致）
//   线条跟随 → 记录最近一段光标位置，画一条会变短、渐隐的笔迹
void WinSettingFeatures::updatePreviewOnMouseMove(const POINT& pos)
{
	// 鼠标离开窗口：如果正在「光标放大」预览，立刻把系统光标放回来，
	// 否则用户移出窗口后会有最多 320ms 看不到光标。
	if (pos.x == INT_MAX || pos.y == INT_MAX) {
		restorePreviewCursor();
		// 放大镜是置顶窗，鼠标进入它时卡片可能收到 WM_MOUSELEAVE；
		// 此时真实光标还在卡片上，不能 hide，否则会 hide→show→leave 疯狂闪烁。
		bool cursorInCard = false;
		POINT cur{};
		if (GetCursorPos(&cur)) {
			if (win && win->hwnd) ScreenToClient(win->hwnd, &cur);
			cursorInCard = trailPreviewArea_ && trailPreviewArea_->isPosIn(cur);
		}
		if (!cursorInCard) MagLens::get().hide();
		previewHoverOn_ = false;
		previewTrail_ = {};
		previewClicks_.clear();
		paintPreview();
		return;
	}

	auto* s = Setting::get();
	if (!s || tabIndex != 1) {
		const bool wasActive = previewHoverOn_ || !previewTrail_.points.empty()
			|| !previewClicks_.empty();
		previewHoverOn_ = false;
		previewTrail_ = {};
		MagLens::get().hide();
		if (wasActive) paintPreview();
		return;
	}

	const bool wasActive = previewHoverOn_ || !previewTrail_.points.empty()
		|| !previewClicks_.empty();
	const bool inArea = trailPreviewArea_ && trailPreviewArea_->isPosIn(pos);
	const int trailMode = s->getMouseTrailEffect();

	// 局部放大用独立的圆形放大镜窗口预览（实时放大卡片本身），不在 Canvas 上画
	if (inArea && trailMode == Setting::kMouseTrailMagnifier) {
		POINT p = pos;
		if (win && win->hwnd) ClientToScreen(win->hwnd, &p);
		// 与录制一致：镜头圆心取光标本体中心，不是热点尖角
		float ox = 0.f, oy = 0.f;
		Util::cursorBodyOffset(ox, oy);
		p.x += (LONG)ox;
		p.y += (LONG)oy;
		// 设置页预览：镜头盖在下拉菜单/悬停气泡上面
		MagLens::get().raiseTop();
		MagLens::get().showAt(p, win ? win->dpi : 1.f);
		previewHoverOn_ = false;
		previewTrail_ = {};
		paintPreview();
		return;
	}
	MagLens::get().hide();

	if (inArea && trailMode != Setting::kMouseTrailNone) {
		previewHoverOn_ = true;
		previewHoverPt_ = pos;
		if (trailMode == Setting::kMouseTrailLine) {
			// 与录制时同一套渐隐画笔：点带时间戳，笔宽随点龄收细；
			// 寿命取一半，预览里看到的长度也和录制一致。
			previewTrail_.width = 5.f * (win && win->dpi > 0.f ? win->dpi : 1.f);
			previewTrail_.lifeMs = 240;   // 与录制一致
			LaserStroke::addPoint(previewTrail_, (float)pos.x, (float)pos.y,
				(int64_t)GetTickCount64());
			startPreviewTimer();
		}
	}
	else {
		previewHoverOn_ = false;
	}
	// 切回高亮/无时把上一次的线条清掉
	if (trailMode != Setting::kMouseTrailLine) previewTrail_ = {};

	const bool isActive = previewHoverOn_ || !previewTrail_.points.empty()
		|| !previewClicks_.empty();
	if (isActive || wasActive) paintPreview();
}

// 单击「鼠标单击效果」行左侧（标题/空白处）直接预览该效果；点右侧下拉只是选值。
void WinSettingFeatures::updatePreviewOnMouseDown(const POINT& pos)
{
	auto* s = Setting::get();
	if (!s || tabIndex != 1) return;
	if (!clickPreviewArea_ || !clickPreviewArea_->isPosIn(pos)) return;
	const int mode = s->getMouseClickEffect();
	if (mode == Setting::kMouseClickNone) return;
	if (mode == Setting::kMouseClickZoom || mode == Setting::kMouseClickCombo) {
		// 每次单击都重新抓一次"当前"系统光标，不能一直用第一次缓存下来的样子：
		// 箭头 / 手型 / 工字 / 自定义光标会随控件变化，预览必须跟当前样式一致。
		if (previewCursorHidden_) restorePreviewCursor();   // 隐藏状态下 GetCursorInfo 抓不到光标
		previewCursorBmp_.Reset();
		previewCursorTried_ = false;
		// 先抓光标再藏，否则抓到的是"正在被我们藏掉"的空光标；
		// 抓不到就干脆不藏，免得光标消失却没有放大的光标顶上。
		if (!ensurePreviewCursorBitmap()) return;
		if (!previewCursorHidden_) {
			ShowCursor(FALSE);
			previewCursorHidden_ = true;
		}
	}
	previewClicks_.push_back({ pos, GetTickCount64() });
	if (previewClicks_.size() > 6) previewClicks_.erase(previewClicks_.begin());
	startPreviewTimer();
	paintPreview();
}

void WinSettingFeatures::pickMouseFxColor()
{
	auto* s = Setting::get();
	if (!s) return;
	const UINT32 cur = s->getMouseHighlightColor();
	static COLORREF cust[16]{};
	CHOOSECOLORW cc{};
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = win ? win->hwnd : nullptr;   // 调色板以设置窗为宿主，避免落到设置窗背后
	cc.lpCustColors = cust;
	cc.rgbResult = RGB((cur >> 24) & 0xFFu, (cur >> 16) & 0xFFu, (cur >> 8) & 0xFFu);
	cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
	if (!ChooseColorW(&cc)) return;
	s->setMouseHighlightColor(((UINT32)GetRValue(cc.rgbResult) << 24)
		| ((UINT32)GetGValue(cc.rgbResult) << 16)
		| ((UINT32)GetBValue(cc.rgbResult) << 8) | 0xFFu);
	syncColorSwatches();
	paintPreview();   // 颜色变了立刻重画预览（笔刷按新色号重建）
}

void WinSettingFeatures::syncColorSwatches()
{
	auto* s = Setting::get();
	if (!s) return;
	const UINT32 v = s->getMouseHighlightColor();
	styleColorSwatch(trailSwatch_, v);
	styleColorSwatch(clickSwatch_, v);
}

// 色块只在对应效果真的用色时出现：
//   轨迹：高亮提醒 / 线条跟随；单击：仅单色波纹（光标放大没有颜色可调）
void WinSettingFeatures::updateSwatchVisibility()
{
	auto* s = Setting::get();
	if (!s) return;
	const int trail = s->getMouseTrailEffect();
	const bool useTrailColor = trail == Setting::kMouseTrailHighlight
		|| trail == Setting::kMouseTrailLine;
	if (trailSwatch_) {
		if (useTrailColor) trailSwatch_->show();
		else trailSwatch_->hide();
	}
	const int clickMode = s->getMouseClickEffect();
	const bool useClickColor = clickMode == Setting::kMouseClickRipple
		|| clickMode == Setting::kMouseClickCombo;
	if (clickSwatch_) {
		if (useClickColor) clickSwatch_->show();
		else clickSwatch_->hide();
	}
}

void WinSettingFeatures::restorePreviewCursor()
{
	if (!previewCursorHidden_) return;
	// ShowCursor 是计数式的：隐藏一次就恢复一次
	ShowCursor(TRUE);
	previewCursorHidden_ = false;
}

void WinSettingFeatures::startPreviewTimer()
{
	if (!win || previewTimerOn_) return;
	win->setTimer(kPreviewTickMs, kPreviewTimerId);
	previewTimerOn_ = true;
}

void WinSettingFeatures::stopPreviewTimerIfIdle()
{
	if (!win || !previewTimerOn_ || !previewTrail_.points.empty()
		|| !previewClicks_.empty()) return;
	win->killTimer(kPreviewTimerId);
	previewTimerOn_ = false;
}

void WinSettingFeatures::updatePreviewFrame()
{
	const ULONGLONG now = GetTickCount64();
	const int64_t nowMs = (int64_t)now;
	// 只在整条轨迹过期时清空：刚采到 1 个点时 visible 仍是 false，
	// 若按 visible 清掉就永远攒不到第 2 个点。
	const int64_t life = std::max<int64_t>(1, previewTrail_.lifeMs);
	if (!previewTrail_.points.empty() && nowMs - previewTrail_.points.back().t >= life)
		previewTrail_ = {};
	while (!previewClicks_.empty() && now - previewClicks_.front().t > 520)
		previewClicks_.erase(previewClicks_.begin());
	// 放大动画（320ms）结束就恢复系统光标；不能等 click 记录 520ms 清掉
	bool zoomActive = false;
	const int clickMode = Setting::get() ? Setting::get()->getMouseClickEffect()
		: Setting::kMouseClickNone;
	if (clickMode == Setting::kMouseClickZoom || clickMode == Setting::kMouseClickCombo) {
		for (const auto& fx : previewClicks_)
			if (now - fx.t < 320) { zoomActive = true; break; }
	}
	if (!zoomActive) restorePreviewCursor();
	paintPreview();
	stopPreviewTimerIfIdle();
}

ID2D1SolidColorBrush* WinSettingFeatures::ensurePreviewBrush()
{
	auto* s = Setting::get();
	if (!s) return nullptr;
	const UINT32 rgba = s->getMouseHighlightColor();
	if (!previewBrush_ || previewBrushColor_ != rgba) {
		previewBrush_.Reset();
		auto* d2d = Ling::D2D::get();
		if (!d2d || !d2d->deviceContext) return nullptr;
		d2d->deviceContext->CreateSolidColorBrush(
			D2D1::ColorF(((rgba >> 24) & 0xFFu) / 255.f, ((rgba >> 16) & 0xFFu) / 255.f,
				((rgba >> 8) & 0xFFu) / 255.f, 1.f),
			previewBrush_.GetAddressOf());
		previewBrushColor_ = rgba;
	}
	return previewBrush_.Get();
}

// 抓一次当前系统光标，供「光标放大」预览使用。抓不到（远程桌面/隐藏光标）就只显示波纹兜底。
bool WinSettingFeatures::ensurePreviewCursorBitmap()
{
	if (previewCursorBmp_) return true;
	if (previewCursorTried_) return false;
	previewCursorTried_ = true;
	// 失败时允许下一次单击再试（远程桌面/光标暂时隐藏都可能第一次抓不到）
	auto fail = [this]() { previewCursorTried_ = false; return false; };

	CURSORINFO ci{ sizeof(CURSORINFO) };
	if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return fail();
	ICONINFO ii{};
	if (!GetIconInfo(ci.hCursor, &ii)) return fail();
	int width = 0, height = 0;
	auto cleanup = [&] {
		if (ii.hbmColor) DeleteObject(ii.hbmColor);
		if (ii.hbmMask) DeleteObject(ii.hbmMask);
		ii.hbmColor = ii.hbmMask = nullptr;
	};
	if (!CursorImage::ChooseSize(ii, &width, &height)) {
		cleanup();
		previewCursorTried_ = false;
		return false;
	}
	previewCursorHotspot_ = POINT{ (LONG)ii.xHotspot, (LONG)ii.yHotspot };

	HDC screenDc = GetDC(nullptr);
	HDC memDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
	std::vector<BYTE> px, xorMask;
	const bool got = memDc && CursorImage::Extract(memDc, ii, width, height, px, &xorMask)
		&& px.size() == (size_t)width * height * 4;
	if (memDc) DeleteDC(memDc);
	if (screenDc) ReleaseDC(nullptr, screenDc);
	cleanup();
	if (!got) {
		previewCursorTried_ = false;
		return false;
	}
	// 单色光标的"反色"像素按白色给出，预览卡片是浅色底也能看见
	std::vector<BYTE> pm(px.size());
	for (size_t i = 0; i + 3 < pm.size(); i += 4) {
		const BYTE a = px[i + 3];
		pm[i] = (BYTE)(px[i] * a / 255);
		pm[i + 1] = (BYTE)(px[i + 1] * a / 255);
		pm[i + 2] = (BYTE)(px[i + 2] * a / 255);
		pm[i + 3] = a;
	}
	auto* d2d = Ling::D2D::get();
	if (!d2d || !d2d->deviceContext) return false;
	D2D1_BITMAP_PROPERTIES1 prop{
		.pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_NONE }
	};
	if (FAILED(d2d->deviceContext->CreateBitmap(
		D2D1::SizeU((UINT32)width, (UINT32)height), pm.data(),
		(UINT32)width * 4, &prop, previewCursorBmp_.GetAddressOf()))) {
		previewCursorTried_ = false;
		return false;
	}
	previewCursorW_ = width;
	previewCursorH_ = height;
	return true;
}

void WinSettingFeatures::paintPreview()
{
	if (!previewCanvas_ || !win) return;
	if (previewCanvas_->w <= 0.f || previewCanvas_->h <= 0.f) return;
	auto* s = Setting::get();
	if (!s) return;
	ID2D1DeviceContext* ctx = previewCanvas_->startPaint();
	if (!ctx) return;
	ctx->Clear(0);

	const float d = win->dpi > 0.f ? win->dpi : 1.f;
	auto* brush = ensurePreviewBrush();
	if (!brush) {
		previewCanvas_->finishPaint();
		return;
	}

	// 悬停预览：高亮圆 / 跟随线条
	const int trailMode = s->getMouseTrailEffect();
	if (trailMode == Setting::kMouseTrailHighlight && previewHoverOn_) {
		float ox = 0.f, oy = 0.f;
		Util::cursorBodyOffset(ox, oy);
		const float r = MouseFx::highlightRadius * d;   // 与录制/屏幕一致（直径 50）
		brush->SetOpacity(MouseFx::highlightAlpha);    // 与录制/屏幕一致
		ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)previewHoverPt_.x + ox,
			(float)previewHoverPt_.y + oy), r, r), brush);
	}
	else if (trailMode == Setting::kMouseTrailLine) {
		const int64_t nowMs = (int64_t)GetTickCount64();
		if (LaserStroke::visible(previewTrail_, nowMs)) {
			brush->SetOpacity(0.5f);   // 与录制时一致：半透明
			LaserStroke::paint(ctx, brush, previewTrail_, nowMs);
		}
	}

	// 单击预览：单色波纹 / 光标放大
	const int clickMode = s->getMouseClickEffect();
	if (clickMode == Setting::kMouseClickRipple || clickMode == Setting::kMouseClickCombo) {
		const ULONGLONG now = GetTickCount64();
		for (const auto& fx : previewClicks_) {
			const ULONGLONG age = now - fx.t;
			if (age >= MouseFx::rippleLifeMs) continue;
			const float k = (float)age / (float)MouseFx::rippleLifeMs;
			const float r = MouseFx::rippleRadius(k) * d;
			brush->SetOpacity(MouseFx::rippleAlpha(k));
			ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)fx.pt.x, (float)fx.pt.y), r, r),
				brush, MouseFx::rippleWidth(k) * d);
		}
	}
	if ((clickMode == Setting::kMouseClickZoom || clickMode == Setting::kMouseClickCombo)
		&& previewCursorBmp_ && win && win->hwnd) {
		const ULONGLONG now = GetTickCount64();
		const PreviewClickFx* latest = nullptr;
		for (const auto& fx : previewClicks_)
			if (now - fx.t < 320) latest = &fx;
		if (latest) {
			// 跟着鼠标实时位置画，不把光标钉在点击点：动效期间鼠标仍可自由移动。
			POINT p{};
			if (GetCursorPos(&p)) {
				ScreenToClient(win->hwnd, &p);
				RECT rc{};
				GetClientRect(win->hwnd, &rc);
				if (p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom) {
					const float k = (float)(now - latest->t) / 320.f;
					const float grow = std::sin(k * 3.14159265f);
					const float scale = 1.f + 0.8f * grow;   // 与 WinCap::clickZoomScale 一致
					// 系统光标在这段时间被藏起来了，所以这里画实心光标，不能再用半透明叠层
					const float l = (float)p.x - previewCursorHotspot_.x * scale;
					const float t = (float)p.y - previewCursorHotspot_.y * scale;
					ctx->DrawBitmap(previewCursorBmp_.Get(),
						D2D1::RectF(l, t, l + previewCursorW_ * scale, t + previewCursorH_ * scale),
						1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
				}
			}
		}
	}

	brush->SetOpacity(1.f);
	previewCanvas_->finishPaint();
}

void WinSettingFeatures::showTab(int index)
{
	if (index < 0 || index > 4) return;
	// 切页时收起上一个 Tab 的预览（尤其线条/波纹还没走完的动画）
	MagLens::get().hide();
	restorePreviewCursor();
	previewHoverOn_ = false;
	previewHoverPt_ = POINT{ -1, -1 };
	previewTrail_ = {};
	previewClicks_.clear();
	stopPreviewTimerIfIdle();
	paintPreview();
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
	// 重建会销毁行节点：预览触发区/动画先全部收掉，避免旧指针和新状态残留
	MagLens::get().hide();
	restorePreviewCursor();
	previewHoverOn_ = false;
	previewHoverPt_ = POINT{ -1, -1 };
	previewTrail_ = {};
	previewClicks_.clear();
	trailPreviewArea_ = nullptr;
	clickPreviewArea_ = nullptr;
	trailSwatch_ = clickSwatch_ = nullptr;
	stopPreviewTimerIfIdle();
	paintPreview();
	tabs[index]->removeAllChildren();
	if (index == 0) buildBasic(tabs[index]);
	else if (index == 1) buildRecord(tabs[index]);
	else if (index == 2) buildOutput(tabs[index]);
	else if (index == 3) buildRecognize(tabs[index]);
	else buildOther(tabs[index]);
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
		Lang::get(L"setting.fnTabOutput"), Lang::get(L"setting.fnTabRecognize"),
		Lang::get(L"setting.fnTabOther")
	};
	// 分段胶囊 Tab（点击滑块滑动）＋ 右侧「恢复默认」
	auto tabRow = makeChild<Ling::Node>();
	tabRow->setFlexDirection(Ling::FlexDirection::Row);
	tabRow->setWidthPercent(100.f);
	tabRow->setAlignItems(Ling::Align::Center);
	tabRow->setMarginBottom(SettingTheme::sp3);
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
		case 3:
			s->restoreFunctionOcrDefaults();
			s->restoreFunctionTranslateDefaults();
			break;
		default:
			s->restoreFunctionOtherDefaults();
			SettingUi::applyFeatureTipsVisibility();
			break;
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
	buildRecognize(tabs[3]); SettingUi::trimTrailingGap(tabs[3]);
	buildOther(tabs[4]);    SettingUi::trimTrailingGap(tabs[4]);
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
						// 先投递到调度队列再重启：不要在弹窗自身的点击回调中同步退出进程
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
		// 右列三行都是下拉：托盘单击行为 / 保存格式 / 截图完成执行，视觉上对齐
		SettingUi::toggleRow(g, Lang::get(L"setting.cancelConfirmDialog"), s->getCancelConfirmDialog(),
			[](bool on) { Setting::get()->setCancelConfirmDialog(on); }, true);
		const std::vector<std::wstring> finishOpts{
			Lang::get(L"setting.finishSaveCopyImage"),
			Lang::get(L"setting.finishSaveCopyFile"),
			Lang::get(L"setting.finishSaveCutFile"),
			Lang::get(L"setting.finishCopyOnly") };
		SettingUi::selectRow(g, Lang::get(L"setting.captureFinishAction"), L"", finishOpts,
			std::clamp(s->getCaptureFinishAction(), 0, 3),
			[](int i, const std::wstring&) { Setting::get()->setCaptureFinishAction(i); }, true,
			Lang::get(L"setting.captureFinishActionTip"));
		SettingUi::grid2Cells(g);
	}

	SettingUi::groupLabel(p, Lang::get(L"setting.fnGrpPin"));
	{
		auto* g = SettingUi::grid2(p);
		// 顺序：自动文本识别在前，「滚动后执行」紧随其后（用户要求）。
		SettingUi::toggleRow(g, Lang::get(L"setting.pinAutoOcr"), s->getPinAutoOcr(),
			[](bool on) { Setting::get()->setPinAutoOcr(on); }, true);
		SettingUi::selectRow(g, Lang::get(L"setting.pinWheelAction"),
			{ Lang::get(L"setting.pinWheelZoomAtCursor"),
			  Lang::get(L"setting.pinWheelZoomEdge"),
			  Lang::get(L"setting.pinWheelOpacity") },
			std::clamp(s->getPinWheelAction(), 0, 2),
			[](int i, const std::wstring&) { Setting::get()->setPinWheelAction(i); }, true);
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
	{
		// 本机缺少 H.265 编码器时标记该选项：选中后录制必然失败，避免用户自行试错
		std::vector<std::wstring> encOpts{
			Lang::get(L"setting.fnRecEncoderAuto"), L"H.264", L"H.265" };
		if (!CapVideo::hasHevcEncoder()) encOpts[2] += Lang::get(L"setting.fnRecHevcUnavailable");
		SettingUi::selectRow(g, Lang::get(L"setting.fnRecEncoder"), encOpts, s->getVideoEncoder(),
			[](int i, const std::wstring&) { Setting::get()->setVideoEncoder(i); }, true);
	}
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
	// 「记录操作按键」：位置可选 视窗左侧 / 视窗右侧 / 无（无 = 关闭，默认）。
	// 显示与分体式绘制见 WinCap::paintKeyOverlay
	SettingUi::selectRow(g, Lang::get(L"setting.fnRecKeystrokes"), L"",
		{ Lang::get(L"setting.fnRecKeyOverlayLeft"), Lang::get(L"setting.fnRecKeyOverlayRight"),
		  Lang::get(L"setting.fnRecKeyOverlayNone") },
		std::clamp(s->getKeyOverlayPos(), 0, 2),
		[](int i, const std::wstring&) { Setting::get()->setKeyOverlayPos(i); }, true,
		Lang::get(L"setting.fnRecKeystrokesTip"));
	// 行为见 ToolVideo：手动把录屏工具栏拖到屏幕上/下边缘后自动缩进边缘，悬停滑出
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecToolbarPeek"), L"", s->getRecordToolbarPeek(),
		[](bool on) { Setting::get()->setRecordToolbarPeek(on); }, true,
		Lang::get(L"setting.fnRecToolbarPeekTip"));
	// 行为见 WinCap::updateSelFollowCursor（录制中 + 穿透态）
	SettingUi::toggleRow(g, Lang::get(L"setting.fnRecSelFollowMouse"), L"", s->getSelFollowMouse(),
		[](bool on) { Setting::get()->setSelFollowMouse(on); }, true,
		Lang::get(L"setting.fnRecSelFollowMouseTip"));
	// 「鼠标轨迹效果」：高亮提醒（原来的鼠标高亮）/ 线条跟随（变短渐隐的画笔）/ 无（默认）。
	// 选中效果后整张卡片都生效：悬停卡片即在预览层里跟着光标画出来（见 WinCap::paintMouseTrail）。
	// 色块在下拉左侧，只在用色时出现：高亮提醒 / 线条跟随；点开是系统调色板。
	{
		auto r = SettingUi::optionRow(g, Lang::get(L"setting.fnRecMouseTrail"), L"", true,
			SettingTheme::ctrlH, Lang::get(L"setting.fnRecMouseTrailTip"));

		auto* swatch = r.ctrlBox->makeChild<Ling::Button>();
		swatch->setSize(20.f, 20.f);
		swatch->setBorderRadius(4.f);
		swatch->setMarginRight(6.f);
		swatch->setText(L"");
		swatch->onClick.add([this](Ling::Button*) { pickMouseFxColor(); });
		trailSwatch_ = swatch;

		auto* ddl = SettingUi::selectDdl(r.ctrlBox,
			{ Lang::get(L"setting.fnRecMouseTrailHighlight"),
			  Lang::get(L"setting.fnRecMouseTrailLine"),
			  Lang::get(L"setting.fnRecMouseTrailMagnifier"),
			  Lang::get(L"setting.fnRecMouseTrailNone") },
			trailIndexFromEffect(s->getMouseTrailEffect()),
			[this](int i, const std::wstring&) {
				Setting::get()->setMouseTrailEffect(trailEffectFromIndex(i));
				// 录制中用快捷键临时叠加的镜头：下拉一改就作废，按新选择来
				if (auto* cap = WinCap::get()) cap->onTrailEffectChanged();
				updateSwatchVisibility();
				MagLens::get().hide();
			});
		// 这一行还要放色块：下拉比通用宽稍窄（124），否则半栏里标题会被挤到换行
		ddl->setWidth(124.f);
		ddl->setFlexShrink(0.f);
		trailPreviewArea_ = r.row;
	}
	// 「鼠标单击效果」：单色波纹 / 光标放大 / 无（默认）。单击卡片任意位置直接预览。
	// 单色波纹用色，色块在下拉左侧；光标放大只是把光标短暂放大，没有颜色可调，故不显示色块。
	{
		auto r = SettingUi::optionRow(g, Lang::get(L"setting.fnRecMouseClick"), L"", true,
			SettingTheme::ctrlH, Lang::get(L"setting.fnRecMouseClickTip"));

		auto* swatch = r.ctrlBox->makeChild<Ling::Button>();
		swatch->setSize(20.f, 20.f);
		swatch->setBorderRadius(4.f);
		swatch->setMarginRight(6.f);
		swatch->setText(L"");
		swatch->onClick.add([this](Ling::Button*) { pickMouseFxColor(); });
		clickSwatch_ = swatch;

		auto* ddl = SettingUi::selectDdl(r.ctrlBox,
			{ Lang::get(L"setting.fnRecMouseClickRipple"),
			  Lang::get(L"setting.fnRecMouseClickZoom"),
			  Lang::get(L"setting.fnRecMouseClickCombo"),
			  Lang::get(L"setting.fnRecMouseClickNone") },
			clickIndexFromEffect(s->getMouseClickEffect()),
			[this](int i, const std::wstring&) {
				Setting::get()->setMouseClickEffect(clickEffectFromIndex(i));
				updateSwatchVisibility();
				restorePreviewCursor();   // 换成非「光标放大/组合」时立刻把系统光标放回来
			});
		ddl->setWidth(124.f);
		ddl->setFlexShrink(0.f);
		clickPreviewArea_ = r.row;
	}
	syncColorSwatches();
	updateSwatchVisibility();
	SettingUi::grid2Cells(g);
}

void WinSettingFeatures::buildOutput(Ling::Node* p)
{
	auto* s = Setting::get();
	const std::wstring defDir = Util::defaultOutputDir().wstring();

	// 标题列宽按当前语言的实际文字宽度算：中文标题比英文短，输入框能多占一点；
	// 上限 170，避免英文长标题把输入框挤得太窄。
	{
		float labelW = 110.f;
		float dpi = win->dpi > 0.f ? win->dpi : 1.f;
		auto measure = [&](const wchar_t* key) {
			const std::wstring text = Lang::get(key);
			if (auto layout = Ling::D2D::get()->makeTextLayout(text, SettingTheme::fontBase * dpi)) {
				DWRITE_TEXT_METRICS m{};
				if (SUCCEEDED(layout->GetMetrics(&m)))
					labelW = (std::max)(labelW, m.width / dpi);
			}
		};
		measure(L"setting.manualSaveFmt");
		measure(L"setting.autoSaveFmt");
		measure(L"setting.videoRecordFmt");
		measure(L"setting.fnScreenshotDir");
		measure(L"setting.fnRecordDir");
		measure(L"setting.cachePath");
		gOutputLabelW = (std::min)(labelW + 10.f, 170.f);
	}

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

	// 格式与路径合并成一组
	SettingUi::groupLabel(p, Lang::get(L"setting.formatAndPath"), Lang::get(L"setting.outputSectionTip"));
	outputRow(p, Lang::get(L"setting.manualSaveFmt"),
		s->getManualSaveFormat(), Setting::defaultManualSaveFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setManualSaveFormat(v); });
	outputRow(p, Lang::get(L"setting.autoSaveFmt"),
		s->getAutoSaveFormat(), Setting::defaultAutoSaveFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setAutoSaveFormat(v); });
	outputRow(p, Lang::get(L"setting.videoRecordFmt"),
		s->getVideoRecordFormat(), Setting::defaultVideoRecordFormat(), OutputRowKind::Format,
		[](const std::wstring& v) { Setting::get()->setVideoRecordFormat(v); });
	outputRow(p, Lang::get(L"setting.fnScreenshotDir"), s->getScreenshotDir(), defDir,
		OutputRowKind::Dir,
		[](const std::wstring& v) { Setting::get()->setScreenshotDir(v); }, L"");
	outputRow(p, Lang::get(L"setting.fnRecordDir"), s->getRecordDir(), defDir,
		OutputRowKind::Dir,
		[](const std::wstring& v) { Setting::get()->setRecordDir(v); }, L"");
	// 缓存文件路径：默认 LocalAppData\SnowAir\Cache，可自定义
	outputRow(p, Lang::get(L"setting.cachePath"), s->getCacheDir(), s->getDefaultCacheRoot().wstring(),
		OutputRowKind::Dir,
		[](const std::wstring& v) { Setting::get()->setCacheDir(v); },
		L"", L"setting.cacheDirTip");   // 只保留标题后的「?」提示，输入框不再重复
}

// 识别模型：一页两组 —— 文字识别 / 翻译模型（沿用各自原有的控件与回调）
void WinSettingFeatures::buildRecognize(Ling::Node* p)
{
	// 同页两组统一 12：Tab 条 / OCR 行自带 12，组标题不再叠加。
	auto* ocrLab = SettingUi::groupLabel(p, Lang::get(L"setting.fnTabOcr"));
	if (ocrLab && ocrLab->parent) ocrLab->parent->setMarginTop(0.f);
	buildOcr(p);
	auto* transLab = SettingUi::groupLabel(p, Lang::get(L"setting.fnTabTrans"));
	if (transLab && transLab->parent) transLab->parent->setMarginTop(0.f);
	buildTranslate(p);
}

void WinSettingFeatures::buildOcr(Ling::Node* p)
{
	auto* s = Setting::get();
	// Lite 版不提供云端 / API 识图：只保留本地 PP-OCRv6 与系统 OCR。
	// PP-OCRv6 有轻量/标准两档；只有装了多档时才需要「模型规格」选择。
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
		auto addResultRow = [s](Ling::Node* parent) {
			SettingUi::selectRow(parent, Lang::get(L"setting.fnOcrResultMode"),
				{ Lang::get(L"setting.fnOcrResultOverlay"), Lang::get(L"setting.fnOcrResultReflow") },
				std::clamp(s->getOcrResultMode(), 0, 1),
				[](int i, const std::wstring&) { Setting::get()->setOcrResultMode(i); }, true);
		};

		const bool tierShown = (s->getOcrEngine() == 0 && installed.size() > 1);

		auto* row1 = halfRow(p);
		SettingUi::selectRow(halfCol(row1, false), Lang::get(L"setting.fnOcrModel"), labels, cur,
			[this, engines](int i, const std::wstring&) {
				if (i < 0 || i >= (int)engines.size()) return;
				const bool changed = Setting::get()->getOcrEngine() != engines[i];
				Setting::get()->setOcrEngine(engines[i]);
				// 切到系统 OCR 时「模型规格」要收起来（反之亦然），按需重建本页
				if (changed) this->deferRebuildTab(3);
			}, true);

		// 模型规格：只有选中 PP-OCRv6 且装了多档时才需要选择；只有一档不显示
		if (tierShown) {
			// 识别结果版式放第一行右列（与文本识别模型并排）
			addResultRow(halfCol(row1, true));

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
			auto tierRow = SettingUi::optionRow(halfCol(halfRow(p), false), Lang::get(L"setting.fnOcrTier"), L"", true,
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
			addResultRow(halfCol(row1, true));
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
		{ L"youdao",    L"setting.fnTransYoudao" },
		{ L"baidu",     L"setting.fnTransBaidu" },
		{ L"microsoft", L"setting.fnTransMs" },
		{ L"google",    L"setting.fnTransGoogle" },
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

	// 与文字识别「模型规格」同一套半栏布局：行 = 两个 50% 列，落单也保持半宽

	// 第一行：翻译服务 | 字典模式
	auto* row1 = halfRow(p);
	SettingUi::selectRow(halfCol(row1, false), Lang::get(L"setting.fnTransService"), svcLabs, si,
		[svcIds](int i, const std::wstring&) {
			if (i >= 0 && i < (int)svcIds.size())
				Setting::get()->setTranslateProvider(svcIds[i]);
		}, true);
	SettingUi::toggleRow(halfCol(row1, true), Lang::get(L"setting.fnDictMode"), s->getTranslateDictMode(),
		[](bool on) { Setting::get()->setTranslateDictMode(on); }, true);

	// 第二行左列：每次打开新建会话（右列留空，保持半栏）
	auto* row2 = halfRow(p);
	SettingUi::toggleRow(halfCol(row2, false), Lang::get(L"setting.quickNewSession"), s->getQuickTranslateNewSession(),
		[](bool on) { Setting::get()->setQuickTranslateNewSession(on); }, true);
}

// 其他功能：原「外观设置 > 其他外观」的四项开关搬到这里（2×2 双栏，保持原排版）
void WinSettingFeatures::buildOther(Ling::Node* p)
{
	auto* grid = SettingUi::grid2(p);
	// 开关的点击状态以 Setting 当前值为准（而非构建该行时捕获的值），
	// 避免「恢复默认」程序化改写设置后，下一次点击仍按过期状态取反而失效
	auto mkStateToggle = [&](Ling::Node* parent, const std::wstring& labelKey,
		std::function<bool()> get,
		std::function<void(bool)> set,
		std::function<void(bool)> after,
		const std::wstring& helpKey = L"") -> Ling::Button* {
			auto r = SettingUi::optionRow(parent, Lang::get(labelKey), L"", true, SettingUi::kToggleH,
				helpKey.empty() ? L"" : Lang::get(helpKey));
			r.row->setMarginBottom(0.f);   // 行距交给外层，保持与 grid2Cells 卡片一致
			auto* sw = r.ctrlBox->makeChild<Ling::Button>();
			SettingUi::styleToggle(sw, get());
			sw->onClick.add([this, sw, get, set, after](Ling::Button*) {
				const bool next = !get();
				set(next);
				SettingUi::styleToggle(sw, next);
				if (after) after(next);
				this->syncRestoreState();
				});
			return sw;
		};
	mkStateToggle(grid, L"setting.featureTips",
		[] { return Setting::get()->getFeatureTips(); },
		[](bool on) { Setting::get()->setFeatureTips(on); },
		[](bool) {
			// 立刻把已经创建出来的「?」一起显示/隐藏，不用退出设置页才生效
			SettingUi::applyFeatureTipsVisibility();
		});
	// 「快捷键提示」：开关只影响之后弹出的停悬气泡（已显示的提示仍带旧文字，属预期）
	mkStateToggle(grid, L"setting.hotkeyTip",
		[] { return Setting::get()->getHotkeyTip(); },
		[](bool on) { Setting::get()->setHotkeyTip(on); }, {});
	// 悬停预览：历史记录 / 剪切板卡片停悬时弹出大图或文字卡（默认关闭，避免误触打扰）
	mkStateToggle(grid, L"setting.hoverPreview",
		[] { return Setting::get()->getHoverPreview(); },
		[](bool on) {
			Setting::get()->setHoverPreview(on);
			PluginHost::instance().onSettingChanged(L"hoverPreview");
		}, {},
		L"setting.hoverPreviewTip");
	// 记忆工具样式：记住上次标注工具样式（如大箭头），下次截图自动复用；默认关闭
	mkStateToggle(grid, L"setting.rememberToolStyle",
		[] { return Setting::get()->getRememberToolStyle(); },
		[](bool on) { Setting::get()->setRememberToolStyle(on); }, {});
	SettingUi::grid2Cells(grid);

	// 第三行双栏：左「静默截图」开关，右「全屏时暂停热键」开关
	auto* row = halfRow(p);
	row->setMarginTop(SettingTheme::optionGap);
	mkStateToggle(halfCol(row, false), L"setting.silentCapture",
		[] { return Setting::get()->getSilentCapture(); },
		[](bool on) { Setting::get()->setSilentCapture(on); }, {},
		L"setting.silentCaptureTip");
	mkStateToggle(halfCol(row, true), L"setting.pauseHotkeysFullscreen",
		[] { return Setting::get()->getPauseHotkeysFullscreen(); },
		[](bool on) { Setting::get()->setPauseHotkeysFullscreen(on); }, {},
		L"setting.pauseHotkeysFullscreenTip");

	// 第四行：左「工具栏对齐」下拉（右列留空，保持半栏）
	auto* row2 = halfRow(p);
	row2->setMarginTop(SettingTheme::optionGap);
	{
		const std::vector<std::wstring> labels{
			Lang::get(L"setting.alignLeft"), Lang::get(L"setting.alignRight"), Lang::get(L"setting.alignCenter") };
		auto r = SettingUi::optionRow(halfCol(row2, false), Lang::get(L"setting.toolbarAlign"), L"", true,
			SettingTheme::ctrlH, Lang::get(L"setting.toolbarAlignTip"));
		r.row->setMarginBottom(0.f);
		auto* ddl = SettingUi::selectDdl(r.ctrlBox, labels,
			Setting::get()->getCaptureToolbarAlign(),
			[this](int i, const std::wstring&) {
				Setting::get()->setCaptureToolbarAlign(i);
				this->syncRestoreState();
				// 截图会话正好开着：立刻按新对齐重排（用户手动拖过工具栏时不生效，属预期）
				if (auto* cap = WinCap::get()) cap->layoutTools();
			});
		ddl->setWidth(SettingTheme::dropdownWidth);
		ddl->setFlexShrink(0.f);
	}
}
