#include "pch.h"
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.System.Threading.h>

#include "../App.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Ocr/OcrPackVariant.h"
#include "../Ocr/OcrRuntimeManager.h"
#include "../Ocr/OcrModelManager.h"
#include "../Tool/IconCodes.h"
#include "../Update.h"
#include "../Util.h"
#include "WinSetting.h"
#include "WinSettingAbout.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"

// 关于页（逐项对齐 design/about-page.html 已确认的版本）：
//   ① 配图 520×176，水平居中，顶部被卡片裁掉 14px，下沿压住标题 39px（图在字上面）
//   ② SnowAir 大字 56px / 600
//   ③ 版本号 + Lite beta 胶囊（高 22 / 字号 11.5 / 间距 9）
//   ④ 口号 13.5px 实色
//   ⑤ 按钮组 margin-top:auto → 顶到卡片底部；按钮高 36 / 圆角 8 / 间距 10
//   ⑥ 分隔线 + 导入配置 / 导出配置 / 打开配置目录 + © 城迷 · Liuyiran · 更新日志
// 页面高度恒等于卡片高度（WinSetting::loadPage 里直接挂 cardNode），
// 这样 ⑤ 的 auto 边距才有可分配空间，底部信息才会贴卡片底。
// 配图压字的实现：图片行高只占 176-39，图本身 176 向下溢出；再 bringToFront 提到标题之上。

namespace {

// 配图展示尺寸（逻辑像素）：宽 524，高度按原图 1045:384 的比例算 → ≈192.5。
// 宽高比跟原图一致，ImageBox 就会正好按宽度铺满、不再缩到 479。
constexpr float kArtW{ 524.f };
constexpr float kArtH{ kArtW * 384.f / 1045.f };
// 设计稿里配图的 margin-top 是 -44（内容区上内边距 30），即顶部被卡片裁掉 14px
constexpr float kArtTopClip{ 14.f };
// 标题被配图压住的深度：设计稿里配图下沿 162、标题起点 123 → 39。
// 实机里图片底部带一圈透明留白，所以取值要更大，标题上沿才会真正落到图纸上。
// 每个 +1 = 标题上移 1px（压得更深）。
constexpr float kArtOverlap{ 62.f };

// 彩蛋气泡：左边二维码 88×88，右边两段文字（标题 + 说明、标题 + 群号）。
// 文本列宽/块高按内容实测（见 showEggTip），四周留白由 HoverTip 统一按 12 算，四边才一致。
constexpr float kEggQr{ 88.f };
constexpr float kEggGap{ 12.f };       // 二维码与文字的间距
constexpr float kEggTextWMax{ 240.f }; // 文本列宽兜底（实测失败时用）
constexpr const wchar_t* kEggDesc{ L"欢迎加入频道反馈问题和建议。" };
// 行高按字号估：13px 标题 ≈ 18，12px 正文 ≈ 16（只用来定气泡高度）
constexpr float kEggTitleH{ 18.f };
constexpr float kEggNumberH{ 16.f };
constexpr float kEggTitleGap{ 2.f };    // 标题与它下面那行
constexpr float kEggSectionGap{ 14.f };  // 两段之间
constexpr float kEggBodySize{ 12.f };   // 正文（说明句 / 群号）字号

// 内嵌的图片是 RCDATA，而 Ling::Image 只认文件路径（内部会整块读进内存再解码）。
// 所以先落到 %TEMP%，加载完立刻删掉 —— 不在用户配置目录里留垃圾。
// 返回路径；调用方用完负责删除（loadImg 是同步的，返回后即可删）。
std::wstring writeResToTemp(const wchar_t* resName, const wchar_t* fileName)
{
	auto [data, size] = Ling::Util::getRes(resName);
	if (data == nullptr || size == 0) return {};

	wchar_t tmp[MAX_PATH]{};
	if (GetTempPathW(MAX_PATH, tmp) == 0) return {};

	auto file = std::filesystem::path{ tmp } / fileName;
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) return {};
	out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	out.close();
	if (!out) return {};
	return file.wstring();
}

std::wstring writeArtToTemp()
{
	return writeResToTemp(L"about", L"SnowAir-about-art.png");
}

// 旧版本会把图留在配置目录里，此处一并清理
void removeLegacyArtFile()
{
	const auto legacy = Setting::get()->getDataPath() / L"about-art.png";
	std::error_code ec;
	std::filesystem::remove(legacy, ec);
}

// 选一个配置文件（导入用）。取消返回空串。
std::wstring pickConfigFile(HWND owner)
{
	std::wstring out;
	Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(dlg.GetAddressOf()))) || !dlg)
		return out;
	DWORD flags{ 0 };
	if (SUCCEEDED(dlg->GetOptions(&flags)))
		dlg->SetOptions(flags | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
	COMDLG_FILTERSPEC spec[]{ { L"JSON (*.json)", L"*.json" } };
	dlg->SetFileTypes(_countof(spec), spec);
	dlg->SetFileTypeIndex(1);
	if (FAILED(dlg->Show(owner))) return out;   // 取消
	Microsoft::WRL::ComPtr<IShellItem> item;
	if (FAILED(dlg->GetResult(item.GetAddressOf())) || !item) return out;
	PWSTR p{ nullptr };
	if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
		out = p;
		CoTaskMemFree(p);
	}
	return out;
}

// 整行居中的容器（水平居中 + 交叉轴居中）
Ling::Node* centerRow(Ling::Node* parent)
{
	auto* row = parent->makeChild<Ling::Node>();
	row->setWidthPercent(100.f);
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setJustifyContent(Ling::Justify::Center);
	row->setAlignItems(Ling::Align::Center);
	return row;
}

// 只读胶囊（版本号 / Lite beta）。故意不用 Button：不能点，就不该出现手型光标。
Ling::Node* badge(Ling::Node* parent, const std::wstring& text, bool accent)
{
	auto* box = parent->makeChild<Ling::Node>();
	box->setHeight(22.f);
	box->setBorderRadius(SettingTheme::radiusFull);
	box->setPadding(10.f, 0.f, 10.f, 0.f);
	box->setBorder(1.f, accent ? SettingTheme::successBorder : SettingTheme::border);
	box->setBg(accent ? SettingTheme::successBg : SettingTheme::card);
	box->setAlignItems(Ling::Align::Center);
	box->setJustifyContent(Ling::Justify::Center);

	auto* label = box->makeChild<Ling::Label>();
	label->setText(text);
	label->setFontSize(11.5f);
	label->setColor(accent ? SettingTheme::accentGreen : SettingTheme::textSecondary);
	return box;
}

// 会自转的图标 Label：检查更新时让刷新图标转起来。
// 走 Composition 标量动画（合成线程跑，不占 UI 线程、不用定时器）。
class SpinLabel : public Ling::Label
{
public:
	explicit SpinLabel(Ling::WinBase* w) : Ling::Label(w) {}

	void startSpin()
	{
		auto compositor = visual.Compositor();
		auto anim = compositor.CreateScalarKeyFrameAnimation();
		anim.InsertKeyFrame(1.0f, 360.f);
		anim.Duration(std::chrono::milliseconds(900));
		anim.IterationBehavior(winrt::Windows::UI::Composition::AnimationIterationBehavior::Forever);
		visual.StartAnimation(L"RotationAngleInDegrees", anim);
	}
	void stopSpin()
	{
		visual.StopAnimation(L"RotationAngleInDegrees");
		visual.RotationAngleInDegrees(0.f);
	}
protected:
	void layout() override
	{
		Ling::Label::layout();
		// 绕自身中心转：CenterPoint 是视觉局部坐标，必须等 layout 拿到真实 w/h 再设
		visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.f });
	}
};

// 图标 + 文字的按钮：Button 只负责背景/圆角/命中与悬停，图标和文字用两个 Label 手动摆。
// （Button 内置的 Text 置空；这也是侧栏菜单项一直在用的做法）
Ling::Button* iconButton(Ling::Node* parent, const wchar_t* glyph, const std::wstring& text,
	bool primary, std::function<void()> onClick,
	Ling::Label** outLabel = nullptr, SpinLabel** outIcon = nullptr)
{
	auto* btn = parent->makeChild<Ling::Button>();
	btn->setText(L"");
	btn->setHeight(36.f);
	btn->setBorderRadius(8.f);
	btn->setPadding(16.f, 0.f, 16.f, 0.f);
	btn->setFlexDirection(Ling::FlexDirection::Row);
	btn->setJustifyContent(Ling::Justify::Center);
	btn->setAlignItems(Ling::Align::Center);
	if (primary) {
		btn->setBorder(1.f, SettingTheme::primary);
		btn->setBg(SettingTheme::primary);
		btn->setHoverBg(SettingTheme::primaryHover);
	}
	else {
		btn->setBorder(1.f, SettingTheme::border);
		btn->setBg(SettingTheme::card);
		btn->setHoverBg(SettingTheme::accent);
	}

	const uint32_t fg = primary ? SettingTheme::primaryForeground : SettingTheme::foreground;

	auto* icon = btn->makeChild<SpinLabel>();
	icon->setText(glyph);
	icon->setFontFamily(Icon::Family);
	icon->setFontSize(15.f);
	icon->setColor(fg);
	icon->setMarginRight(7.f);

	auto* label = btn->makeChild<Ling::Label>();
	label->setText(text);
	label->setFontSize(SettingTheme::fontBase);
	label->setColor(fg);

	if (onClick) {
		btn->onClick.add([onClick](Ling::Button*) { onClick(); });
	}
	if (outLabel) *outLabel = label;
	if (outIcon) *outIcon = icon;
	return btn;
}

// "v0.1.12" / "0.1.12" / "v0.1.12-beta" → {0,1,12}；认不出来就全 0（等于「不比本地新」）
std::array<int, 3> parseVersionTag(const std::wstring& tag)
{
	std::array<int, 3> ver{ 0, 0, 0 };
	int idx = 0;
	std::wstring digits;
	auto flush = [&]() {
		if (digits.empty() || idx >= 3) { digits.clear(); return; }
		try { ver[idx++] = std::stoi(digits); }
		catch (...) {}
		digits.clear();
	};
	for (wchar_t ch : tag) {
		if (ch >= L'0' && ch <= L'9') digits.push_back(ch);
		else flush();
	}
	flush();
	return ver;
}

// 拉 GitHub 最新 release 的 tag。网络/解析失败一律 ok=false，不抛出去。
// 8 秒没响应就取消（避免请求挂住导致按钮看起来「卡死」）。
// 「检查中」至少显示 700ms：命中 HTTP 缓存时请求几毫秒就回来了，不兜底的话
// 图标还没转起来就停了，看起来像「再按没反应」。
// 结果经 app 调度队列回到 UI 线程，并用 weak 守卫页面是否还活着；seq 用于丢弃过期请求。
winrt::fire_and_forget fetchLatestTag(std::weak_ptr<bool> weak, WinSettingAbout* page, int seq)
{
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using namespace winrt::Windows::Data::Json;

	constexpr auto kMinSpin = std::chrono::milliseconds(700);
	const auto started = std::chrono::steady_clock::now();

	std::wstring tag;
	bool ok{ false };
	try {
		co_await winrt::resume_background();
		// 每次都真正问一次服务器：版本检查不该吃到上一次的缓存
		HttpBaseProtocolFilter filter;
		filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache);
		HttpClient client{ filter };
		// GitHub API 必须带 User-Agent，否则直接 403
		client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"SnowAir");
		auto op = client.GetStringAsync(
			winrt::Windows::Foundation::Uri{ L"https://github.com/CM-idea/SnowAir/releases.atom" });
		auto guard = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
			[op](const winrt::Windows::System::Threading::ThreadPoolTimer&) { op.Cancel(); },
			std::chrono::seconds(8));
		std::wstring body{ co_await op };
		guard.Cancel();
		// Releases Atom 不受未认证 GitHub API 的 60 次/小时限流影响
		tag = Util::parseGithubReleaseTag(body);
		ok = !tag.empty();
	}
	catch (...) {
		ok = false;
	}

	// 已经在线程池线程上，直接睡够剩余时间，保证动画可见
	const auto spent = std::chrono::steady_clock::now() - started;
	if (spent < kMinSpin) {
		std::this_thread::sleep_for(kMinSpin - spent);
	}

	if (auto app = Ling::App::get()) {
		app->dq.TryEnqueue([weak, page, seq, ok, tag]() {
			if (!weak.lock()) return;   // 页面已经切走/销毁
			page->onCheckFinished(seq, ok, tag);
		});
	}
}

// 晾衣架检查请求失败时不会触发 onRackUpdateChanged：主程序结果回来后兜底收尾，
// 避免状态行永远停在「检查中…」。1.5 秒足够正常请求回来了。
void scheduleRackFallback(std::weak_ptr<bool> weak, WinSettingAbout* page, int seq)
{
	winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
		[weak, page, seq](const winrt::Windows::System::Threading::ThreadPoolTimer&) {
			if (auto app = Ling::App::get()) {
				app->dq.TryEnqueue([weak, page, seq]() {
					if (weak.lock()) page->onRackCheckFallback(seq);
				});
			}
		}, std::chrono::milliseconds(1500));
}

} // namespace

WinSettingAbout::WinSettingAbout(Ling::WinBase* parent) : Ling::Node(parent)
{
	setFlexDirection(Ling::FlexDirection::Column);
	setWidthPercent(100.f);
	YGNodeStyleSetMinWidth(node, 0.f);

	// 彩蛋的进度计时器：悬停时每 tick 点亮一个字符，点满才弹气泡
	if (win) {
		eggTimerTok_ = win->onTimer.add([this](UINT id) {
			if (id != kEggProgressTimerId) return;
			if (eggLit_ < (int)authorChars_.size()) {
				authorChars_[(size_t)eggLit_]->setColor(SettingTheme::textPrimary);
				++eggLit_;
			}
			if (eggLit_ < (int)authorChars_.size()) return;
			win->killTimer(kEggProgressTimerId);
			if (cursorOverAuthor()) this->showEggTipNow();
		});
	}

	// ① 配图不参与排版：绝对定位、最后创建（后加的子节点画在上面），位置在 layout() 里算。
	//    标题用 marginTop 让出图的高度，上沿落进图纸里 → 「图压字」。

	// ② SnowAir 大字
	auto* titleRow = centerRow(this);
	titleRow->setMarginTop(kArtH - kArtTopClip - kArtOverlap - SettingTheme::contentPadY);
	auto* title = titleRow->makeChild<Ling::Label>();
	title->setText(L"SnowAir");
	title->setFontFamily(L"Segoe UI");
	title->setFontSize(56.f);
	title->setFontWeight(600);
	title->setColor(SettingTheme::textPrimary);

	// ③ 版本号 + 版本标记
	auto* metaRow = centerRow(this);
	metaRow->setMarginTop(18.f);
	const auto ver = Util::getAppVersion();
	badge(metaRow, wfmt(L"v%d.%d.%d", ver[0], ver[1], ver[2]), false);
	auto* edition = badge(metaRow, L"Lite beta", true);
	edition->setMarginLeft(9.f);

	// ④ 口号
	auto* sloganRow = centerRow(this);
	sloganRow->setMarginTop(16.f);
	auto* slogan = sloganRow->makeChild<Ling::Label>();
	slogan->setText(Lang::get(L"about.slogan"));
	slogan->setFontSize(13.5f);
	slogan->setColor(SettingTheme::textPrimary);

	// ⑤ 按钮组：图标 + 文字；margin-top:auto 让整组沉到卡片底部
	auto* actions = centerRow(this);
	YGNodeStyleSetMarginAuto(actions->node, YGEdgeTop);

	SpinLabel* checkIcon = nullptr;
	checkBtn_ = iconButton(actions, Icon::Refresh, Lang::get(L"about.checkUpdate"), true,
		[this]() { onCheckButton(); }, &checkLabel_, &checkIcon);
	checkBtn_->setMarginRight(10.f);
	checkIconLabel_ = checkIcon;
	checkBtn_->onEnter.add([this](Ling::Button*) {
		checkHovered_ = true;
		updateCheckTextColor();
	});
	checkBtn_->onLeave.add([this](Ling::Button*) {
		checkHovered_ = false;
		updateCheckTextColor();
	});
	if (checkIcon) {
		spinIcon_ = [checkIcon](bool on) {
			if (on) checkIcon->startSpin();
			else    checkIcon->stopSpin();
		};
	}

	auto* feedback = iconButton(actions, Icon::Bubble, Lang::get(L"about.feedback"), false, []() {
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/issues",
			nullptr, nullptr, SW_SHOWNORMAL);
	});
	feedback->setMarginRight(10.f);

	iconButton(actions, Icon::Github, Lang::get(L"about.repo"), false, []() {
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir",
			nullptr, nullptr, SW_SHOWNORMAL);
	});

	// ⑤-2 检查结果状态行：● 已是最新版本 · 刚刚检查
	auto* statusRow = centerRow(this);
	statusRow->setMarginTop(18.f);

	statusDot_ = statusRow->makeChild<Ling::Node>();
	statusDot_->setSize(6.f, 6.f);
	statusDot_->setBorderRadius(3.f);
	statusDot_->setBg(SettingTheme::textTertiary);
	statusDot_->setMarginRight(8.f);

	statusText_ = statusRow->makeChild<Ling::Label>();
	statusText_->setText(Lang::get(L"about.notChecked"));
	statusText_->setFontSize(12.f);
	statusText_->setColor(SettingTheme::textTertiary);

	statusTime_ = statusRow->makeChild<Ling::Label>();
	statusTime_->setFontSize(12.f);
	statusTime_->setColor(SettingTheme::textTertiary);
	statusTime_->setMarginLeft(4.f);

	// 状态行尾：自动检查频率下拉（每天 / 每周 / 每月 / 从不）
	const std::vector<std::wstring> freqOptions{
		Lang::get(L"about.checkFreqDay"),
		Lang::get(L"about.checkFreqWeek"),
		Lang::get(L"about.checkFreqMonth"),
		Lang::get(L"about.checkFreqNever"),
	};
	auto* freqDdl = SettingUi::selectDdl(statusRow, freqOptions,
		Setting::get()->getUpdateCheckFreq(),
		[](int idx, const std::wstring&) {
			Setting::get()->setUpdateCheckFreq(idx);
			if (idx != 3) Update::checkLater();   // 改频率后若已到期就立刻补查一次
		}, true);
	freqDdl->setWidth(64.f);
	freqDdl->setHeight(20.f);
	freqDdl->setMarginLeft(10.f);
	freqDdl->setFlexShrink(0.f);
	// 常态做成和「尚未检查」同一档文字：12px、三级灰、无边框/无底色；停悬才浮出浅底
	freqDdl->setBorder(0.f, 0);
	freqDdl->setBg(0);
	freqDdl->setHoverBg(SettingTheme::accent);
	freqDdl->setColor(SettingTheme::textTertiary);
	freqDdl->setPadding(4.f, 0.f, 4.f, 0.f);
	freqDdl->setBorderRadius(4.f);
	for (auto& c : freqDdl->children) {
		if (auto* lab = dynamic_cast<Ling::Label*>(c.get())) {
			lab->setFontSize(12.f);
			lab->setColor(SettingTheme::textTertiary);
		}
	}

	// 晾衣架检查结果回来后刷新（只服务本轮手动检查；进页面时保持「尚未检查」）
	rackChangedTok_ = Update::onRackUpdateChanged().add([this]() {
		rackChecked_ = true;
		rackUpdate_ = Update::rackUpdateAvailable();
		if (!appChecked_) return;
		if (checking_) finishCheck();
		else renderCheckResult();
	});

	// ⑥ 分隔线 + 底部信息（导入配置 / 导出配置 / 打开配置目录 / 版权 / 更新日志）
	// 上下各 18：状态行上方 18、下方到分隔线也是 18，视觉上对称
	SettingUi::separator(this, 18.f, 16.f);

	auto* dirRow = makeChild<Ling::Node>();
	dirRow->setWidthPercent(100.f);
	dirRow->setFlexDirection(Ling::FlexDirection::Row);
	dirRow->setAlignItems(Ling::Align::Center);

	// 无边框的轻按钮（文字，可选右侧图标），三个入口共用一套样式；
	// 常态与「更新日志」同色（textTertiary），悬停加浅底、文字转深色。
	auto lightLink = [dirRow](const std::wstring& text, const wchar_t* icon, bool first,
		std::function<void()> onClick) {
		auto* btn = dirRow->makeChild<Ling::Button>();
		btn->setText(L"");                                   // 内置 Text 置空，图标和文字用手动 Label
		btn->setHeight(22.f);
		btn->setBorderRadius(SettingTheme::radiusInner);      // 6：轻量圆角，贴合小字号
		btn->setPadding(6.f, 0.f, 6.f, 0.f);
		// 第一个按钮左缘与下面「© 城迷 · Liuyiran」对齐；其余紧挨着，读起来是「导入配置 导出配置 打开配置目录」一组
		btn->setMarginLeft(first ? -6.f : 2.f);
		btn->setFlexDirection(Ling::FlexDirection::Row);
		btn->setJustifyContent(Ling::Justify::Center);
		btn->setAlignItems(Ling::Align::Center);
		btn->setBorder(0.f, 0);
		btn->setBg(0);
		btn->setHoverBg(SettingTheme::accent);

		auto* label = btn->makeChild<Ling::Label>();
		label->setText(text);
		label->setFontSize(11.5f);
		label->setColor(SettingTheme::textTertiary);

		// 图标可选：导入 / 导出只留文字，打开配置目录保留文件夹图标
		Ling::Label* iconLabel = nullptr;
		if (icon) {
			iconLabel = btn->makeChild<Ling::Label>();
			iconLabel->setText(icon);
			iconLabel->setFontFamily(Icon::Family);
			// 图标字形净高只有 em 的 ~0.75：11.5px 正文配 14px 图标才视觉等高
			iconLabel->setFontSize(14.f);
			iconLabel->setColor(SettingTheme::textTertiary);
			iconLabel->setMarginLeft(5.f);
		}

		// Label 不在 Button 的 hover 变色链上，手动跟随：悬停转深色
		btn->onEnter.add([label, iconLabel](Ling::Button*) {
			label->setColor(SettingTheme::textPrimary);
			if (iconLabel) iconLabel->setColor(SettingTheme::textPrimary);
		});
		btn->onLeave.add([label, iconLabel](Ling::Button*) {
			label->setColor(SettingTheme::textTertiary);
			if (iconLabel) iconLabel->setColor(SettingTheme::textTertiary);
		});
		btn->onClick.add([onClick](Ling::Button*) { onClick(); });
	};

	const HWND owner = win ? win->hwnd : nullptr;

	lightLink(Lang::get(L"about.importConfig"), nullptr, true, [owner]() {
		const auto path = pickConfigFile(owner);
		if (path.empty()) return;
		const auto tip = Lang::get(L"about.sysTip");
		if (!Setting::get()->importConfigFrom(path)) {
			MessageBoxW(owner, Lang::get(L"about.importFailed").c_str(), tip.c_str(),
				MB_OK | MB_ICONWARNING);
			return;
		}
		MessageBoxW(owner, Lang::get(L"about.importOk").c_str(), tip.c_str(), MB_OK | MB_ICONINFORMATION);
		// 提示关掉后自动重启，让新实例重新读一遍配置。
		// 重启投递到调度队列：不在按钮自己的点击回调里同步退出进程
		auto relaunch = [owner]() {
			if (::App::restart()) return;
			MessageBoxW(owner, Lang::get(L"about.importDone").c_str(),
				Lang::get(L"about.sysTip").c_str(), MB_OK | MB_ICONWARNING);
		};
		if (auto* a = Ling::App::get()) a->dq.TryEnqueue(relaunch);
		else relaunch();
	});

	lightLink(Lang::get(L"about.exportConfig"), nullptr, false, [owner]() {
		const auto path = Util::getSaveFilePath(owner, L"json", L"SnowAir-config.json", L"");
		if (path.empty()) return;   // 用户取消
		if (!Setting::get()->exportConfigTo(path)) {
			MessageBoxW(owner, Lang::get(L"about.exportFailed").c_str(),
				Lang::get(L"about.sysTip").c_str(), MB_OK | MB_ICONWARNING);
		}
	});

	lightLink(Lang::get(L"about.openDataDir"), Icon::Folder, false, [owner]() {
		const auto dir = Setting::get()->getDataPath();
		ShellExecuteW(owner, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	});

	auto* crRow = makeChild<Ling::Node>();
	crRow->setWidthPercent(100.f);
	crRow->setFlexDirection(Ling::FlexDirection::Row);
	crRow->setAlignItems(Ling::Align::Center);
	crRow->setMarginTop(10.f);

	// 「© 城迷 · 」是普通文字：彩蛋只在停在 Liuyiran 上时触发
	auto* copyright = crRow->makeChild<Ling::Label>();
	copyright->setText(L"© 城迷 · ");
	copyright->setFontSize(11.5f);
	copyright->setColor(SettingTheme::textTertiary);

	auto* author = crRow->makeChild<Ling::Button>();
	author->setBg(0);
	author->setBorder(0.f, 0);
	author->setHoverBg(0);
	author->setPadding(0.f, 0.f, 0.f, 0.f);
	author->setFlexDirection(Ling::FlexDirection::Row);
	author->setAlignItems(Ling::Align::Center);

	// 彩蛋：Liuyiran 拆成逐个字符，悬停时像进度条一样从左到右点亮，
	// 点满才弹出二维码气泡（直接弹出就不像彩蛋了）
	authorChars_.clear();
	for (const wchar_t* p = L"Liuyiran"; *p; ++p) {
		auto* ch = author->makeChild<Ling::Label>();
		ch->setText(std::wstring(1, *p));
		ch->setFontSize(11.5f);
		ch->setColor(SettingTheme::textTertiary);
		ch->setFlexShrink(0.f);
		authorChars_.push_back(ch);
	}
	authorBtn_ = author;
	author->onEnter.add([this](Ling::Button*) { this->startEggProgress(); });
	author->onLeave.add([this](Ling::Button*) { this->stopEggProgress(); });

	auto* dot = crRow->makeChild<Ling::Label>();
	dot->setText(L"·");
	dot->setFontSize(11.5f);
	dot->setColor(SettingTheme::textTertiary);
	dot->setMarginLeft(8.f);
	dot->setMarginRight(8.f);

	auto* changelog = crRow->makeChild<Ling::Button>();
	changelog->setText(L"");
	changelog->setBg(0);
	changelog->setBorder(0.f, 0);
	changelog->setHoverBg(0);
	changelog->setPadding(0.f, 0.f, 0.f, 0.f);
	changelog->setFlexDirection(Ling::FlexDirection::Row);
	changelog->setAlignItems(Ling::Align::Center);

	auto* changelogText = changelog->makeChild<Ling::Label>();
	changelogText->setText(Lang::get(L"about.changelog"));
	changelogText->setFontSize(11.5f);
	changelogText->setColor(SettingTheme::textTertiary);

	// 外链提示：文字右侧一个小箭头（字库里 e637「箭头」）
	auto* changelogArrow = changelog->makeChild<Ling::Label>();
	changelogArrow->setText(Icon::Arrow);
	changelogArrow->setFontFamily(Icon::Family);
	changelogArrow->setFontSize(11.f);
	changelogArrow->setColor(SettingTheme::textTertiary);
	changelogArrow->setMarginLeft(4.f);

	// 两个 Label 不在 Button 的 hover 变色链上，手动跟随
	changelog->onEnter.add([changelogText, changelogArrow](Ling::Button*) {
		changelogText->setColor(SettingTheme::textPrimary);
		changelogArrow->setColor(SettingTheme::textPrimary);
	});
	changelog->onLeave.add([changelogText, changelogArrow](Ling::Button*) {
		changelogText->setColor(SettingTheme::textTertiary);
		changelogArrow->setColor(SettingTheme::textTertiary);
	});

	changelog->onClick.add([](Ling::Button*) {
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/releases",
			nullptr, nullptr, SW_SHOWNORMAL);
	});

	// ⑦ 配图：最后创建 → z 序最上层（后加的子节点叠在前面的上面），因此压住标题而不是被压。
	//    位置（居中 + 顶到 -14px）在 layout() 里算，绝对定位不占排版空间。
	artHost_ = makeChild<Ling::Node>();
	artHost_->setPositionType(Ling::Position::Absolute);
	artHost_->setSize(kArtW, kArtH);
	auto* art = artHost_->makeChild<Ling::ImageBox>();
	art->setWidth(kArtW);
	art->setHeight(kArtH);
	if (const auto path = writeArtToTemp(); !path.empty()) {
		// maxW/maxH 是物理像素：按当前 DPI 取解码上限，避免把整张原图解码进内存
		art->loadImg(path, kArtW * win->dpi, kArtH * win->dpi);
		// loadImg 是同步的（整块读进内存后解码），加载完就能把临时文件删掉
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}
	removeLegacyArtFile();
}

WinSettingAbout::~WinSettingAbout()
{
	// 页面会被「切页」销毁：把挂在宿主窗口上的进度计时器和订阅一起撤掉，
	// 否则定时器回调会打到已经释放的 this 上（切到别的页就崩）
	if (win) {
		win->killTimer(kEggProgressTimerId);
		if (eggTimerTok_.value) win->onTimer.remove(eggTimerTok_);
	}
	if (rackChangedTok_.value) Update::onRackUpdateChanged().remove(rackChangedTok_);
}

// 彩蛋的第一步：悬停时把 Liuyiran 当进度条，从第一个字符开始逐个点亮。
// 点满（kEggLitStepMs × 字数）才弹气泡；中途移走就整行复位、气泡也不弹。
void WinSettingAbout::startEggProgress()
{
	if (!win) return;
	resetEggChars();
	win->setTimer(kEggLitStepMs, kEggProgressTimerId);
}

// 整行熄灭：只在气泡不显示时才做（气泡还在时文字要保持点亮，不能先熄）
void WinSettingAbout::resetEggChars()
{
	eggLit_ = 0;
	for (auto* ch : authorChars_) ch->setColor(SettingTheme::textTertiary);
}

void WinSettingAbout::showEggTipNow()
{
	if (!authorBtn_ || !win) return;
	if (!eggTip_) {
		eggTip_ = std::make_unique<SettingUi::HoverTip>(win);
		// 气泡不管从哪条路收掉，都把进度条熄灭
		eggTip_->onHidden = [this]() { resetEggChars(); };
	}
	// 文本列宽按说明句实测、块高按文字实测（说明句是两行），气泡就紧贴内容，不留空白
	float textW = kEggTextWMax, textH = 0.f;
	if (auto lay = Ling::D2D::makeTextLayout(kEggDesc, kEggBodySize * win->dpi)) {
		DWRITE_TEXT_METRICS m{};
		if (SUCCEEDED(lay->GetMetrics(&m))) {
			textW = std::ceil(m.width / win->dpi) + 1.f;
			textH = m.height / win->dpi;
		}
	}
	// 块高 = 标题 + 2 + 说明 + 8 + 群名 + 2 + 群号；和二维码取大者
	const float blockH = kEggTitleH + kEggTitleGap + textH + kEggSectionGap
		+ kEggTitleH + kEggTitleGap + kEggNumberH;
	const float contentH = std::max(kEggQr, blockH);
	eggTip_->showCustom(authorBtn_, kEggQr + kEggGap + textW, contentH, [this, textW](Ling::Node* content) {
		// 气泡配色跟着主题反着来（HoverTip 的 tipBg/tipFg）：深色主题给浅底 + 黑码，
		// 浅色主题给深底 + 白码，两种主题下二维码都清晰
		const bool dark = SettingTheme::isDark();
		const uint32_t textMain = dark ? 0x262626FFu : 0xFFFFFFFFu;
		const uint32_t textSub = dark ? 0x6A6A6AFFu : 0xB5B5B5FFu;

		auto* row = content->makeChild<Ling::Node>();
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::FlexStart);   // 文字列与二维码顶部对齐

		auto* qr = row->makeChild<Ling::ImageBox>();
		qr->setSize(kEggQr, kEggQr);
		qr->setFlexShrink(0.f);
		if (const auto path = writeResToTemp(dark ? L"qrcode_black" : L"qrcode_white",
			dark ? L"SnowAir-qrcode-black.png" : L"SnowAir-qrcode-white.png"); !path.empty()) {
			qr->loadImg(path, kEggQr * win->dpi, kEggQr * win->dpi);
			std::error_code ec;
			std::filesystem::remove(path, ec);
		}

		auto* col = row->makeChild<Ling::Node>();
		// 不给固定宽度：让文字自己撑开，行宽由内容决定，气泡里多出来的几像素会被
		// content 的水平居中均分到左右两侧，不会出现"右边比左边宽"
		col->setFlexShrink(0.f);
		col->setMarginLeft(kEggGap);
		col->setFlexDirection(Ling::FlexDirection::Column);
		col->setJustifyContent(Ling::Justify::Center);

		// 文字颜色跟着气泡底色走：深色主题是浅底深字，浅色主题是深底浅字
		auto* title = col->makeChild<Ling::Label>();
		title->setText(L"扫码加入腾讯频道");
		title->setFontSize(13.f);
		title->setFontWeight(600);
		title->setColor(textMain);
		title->setFlexShrink(0.f);

		auto* desc = col->makeChild<Ling::Label>();
		desc->setText(kEggDesc);
		desc->setFontSize(kEggBodySize);
		desc->setColor(textSub);
		desc->setMarginTop(kEggTitleGap);

		// 群名也是标题（与上面同字号同字重），只是隔开一小段
		auto* group = col->makeChild<Ling::Label>();
		group->setText(L"用户交流群");
		group->setFontSize(13.f);
		group->setFontWeight(600);
		group->setColor(textMain);
		group->setMarginTop(kEggSectionGap);
		group->setFlexShrink(0.f);

		// 群号可点：跳到频道 / 群的加入页。字号与颜色跟说明文字一致，只靠手型光标提示可点
		auto* number = col->makeChild<Ling::Button>();
		number->setBg(0);
		number->setBorder(0.f, 0);
		number->setHoverBg(0);
		number->setPadding(0.f, 0.f, 0.f, 0.f);
		number->setMarginTop(kEggTitleGap);   // 群名也是标题：标题跟它下面一行同样只隔 2
		number->setFlexDirection(Ling::FlexDirection::Row);
		number->setAlignItems(Ling::Align::Center);
		number->setJustifyContent(Ling::Justify::Start);   // 与上面两行左对齐（按钮默认居中）

		auto* numberText = number->makeChild<Ling::Label>();
		numberText->setText(L"191023631");
		numberText->setFontSize(kEggBodySize);
		numberText->setColor(textSub);
		// 悬停变主题绿（= SettingTheme::brand），离开复原
		number->onEnter.add([numberText](Ling::Button*) { numberText->setColor(SettingTheme::brand); });
		number->onLeave.add([numberText, textSub](Ling::Button*) { numberText->setColor(textSub); });
		number->onClick.add([this](Ling::Button*) {
			// 点群号：气泡此刻正处于"光标在气泡上"的状态，必须无条件收，否则会压在新开的页面前
			if (eggTip_) eggTip_->hideTip();
			ShellExecuteW(nullptr, L"open", L"https://qm.qq.com/q/XtZoOTO1KU", nullptr, nullptr, SW_SHOWNORMAL);
			});
		});
}

void WinSettingAbout::stopEggProgress()
{
	if (win) win->killTimer(kEggProgressTimerId);
	// 气泡还在显示（进度已点满、光标正往气泡上挪）：文字保持点亮，不能先熄；
	// 气泡真正收掉时由 onHidden 回调复位
	if (eggTip_ && eggTip_->isShown()) {
		if (authorBtn_) eggTip_->hideFor(authorBtn_);
		return;
	}
	resetEggChars();
	// 悬停离开走 hideFor：光标已经在气泡上就不收，让人能移进去点群号
	if (eggTip_ && authorBtn_) eggTip_->hideFor(authorBtn_);
}

bool WinSettingAbout::cursorOverAuthor() const
{
	if (!win || !win->hwnd || !authorBtn_) return false;
	POINT cur{};
	if (!GetCursorPos(&cur)) return false;
	ScreenToClient(win->hwnd, &cur);
	return authorBtn_->isPosIn(cur);
}

void WinSettingAbout::layout()
{
	Ling::Node::layout();
	if (!artHost_) return;
	// 水平居中；顶部再上移 14px 被卡片圆角裁掉（视觉上像图从卡片顶边伸出来）
	const float left = std::round((w - kArtW) * 0.5f);
	artHost_->visual.Offset({ left, -kArtTopClip, 0.f });
}

void WinSettingAbout::startCheck()
{
	// 不限制点击次数：每点一次就重开一轮，旧请求的结果按 seq 丢弃
	const int seq = ++checkSeq_;
	checking_ = true;
	appChecked_ = false;
	appOk_ = false;
	appUpdate_ = false;
	// 上次检查已经查到的插件版本先当作本轮结果；新结果回来后会再刷新
	rackChecked_ = !Update::rackLatestVersion().empty();
	rackUpdate_ = rackChecked_ && Update::rackUpdateAvailable();
	ocrUpdate_ = false;
	for (int i = 0; i < OcrPack::kCount; ++i) {
		const auto v = OcrPack::clampVariant(i);
		if (OcrRuntimeManager::instance().hasOutdated(v) ||
			OcrModelManager::instance().hasOutdated(v)) {
			ocrUpdate_ = true;
			break;
		}
	}
	updateAction_ = false;
	actionIsDownload_ = false;
	// 先恢复成默认的刷新图标/「检查更新」，再切成「检查中…」
	applyCheckButton();
	if (spinIcon_) spinIcon_(true);
	if (checkLabel_) checkLabel_->setText(Lang::get(L"about.checkingBtn"));
	setStatus(Lang::get(L"about.checking"), L"", SettingTheme::textTertiary);
	// 主程序与晾衣架插件同时检查；插件结果由 Update 缓存，插件集成页直接读取
	Update::checkRackUpdate(true);
	// 网络请求在后台线程跑，回来经 dq 落到 onCheckFinished（内部用 weak 守卫页面存活）
	fetchLatestTag(getWeakThis(), this, seq);
}

void WinSettingAbout::onCheckButton()
{
	if (!updateAction_) {
		startCheck();
		return;
	}
	// 有更新：点这一次就执行跳转/下载，然后把按钮恢复成「检查更新」；
	// 再点一次才是重新检查。
	const bool download = actionIsDownload_;
	updateAction_ = false;
	actionIsDownload_ = false;
	applyCheckButton();
	if (download) {
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/releases",
			nullptr, nullptr, SW_SHOWNORMAL);
	}
	else if (auto app = Ling::App::get()) {
		// 切页会析构当前关于页（含这个按钮）：推到下一轮 UI 队列，别在回调里切
		app->dq.TryEnqueue([]() { WinSetting::init(6); });
	}
}

void WinSettingAbout::applyCheckButton()
{
	if (!checkBtn_ || !checkLabel_ || !checkIconLabel_) return;
	if (updateAction_) {
		checkLabel_->setText(Lang::get(L"about.updateNowBtn"));
		checkIconLabel_->setText(Icon::Download);
		// 与版本号旁的「Lite beta」胶囊同一套配色：保留绿色描边；
		// 悬停只轻微加深底色，不改变描边、不做明显的颜色跳变
		checkBtn_->setBorder(1.f, SettingTheme::successBorder);
		// 检查结果回来时鼠标通常还停在按钮上；Button 在 hover 态会忽略 setBorder，
		// 所以必须同步 hover 边框色，否则浅色模式下会残留默认的深色描边
		checkBtn_->setHoverBorderColor(SettingTheme::successBorder);
		checkBtn_->setBg(SettingTheme::successBg);
		checkBtn_->setHoverBg(SettingTheme::successHover);
		checkBtn_->setBorderRadius(8.f);
	}
	else {
		checkLabel_->setText(Lang::get(L"about.checkUpdate"));
		checkIconLabel_->setText(Icon::Refresh);
		checkBtn_->setBorder(1.f, SettingTheme::primary);
		checkBtn_->setHoverBorderColor(SettingTheme::primary);
		checkBtn_->setBg(SettingTheme::primary);
		checkBtn_->setHoverBg(SettingTheme::primaryHover);
		checkBtn_->setBorderRadius(8.f);
	}
	updateCheckTextColor();
}

void WinSettingAbout::updateCheckTextColor()
{
	if (!checkLabel_ || !checkIconLabel_) return;
	const uint32_t fg = updateAction_
		? (checkHovered_ ? SettingTheme::accentGreenHover : SettingTheme::accentGreen)
		: SettingTheme::primaryForeground;
	checkIconLabel_->setColor(fg);
	checkLabel_->setColor(fg);
}

void WinSettingAbout::resetCheckButton()
{
	// 正在检查时别把「检查中…」改回默认
	if (checking_) return;
	updateAction_ = false;
	actionIsDownload_ = false;
	applyCheckButton();
}

void WinSettingAbout::setStatus(const std::wstring& text, const std::wstring& time, uint32_t dotColor)
{
	if (statusText_) statusText_->setText(text);
	if (statusTime_) statusTime_->setText(time);
	if (statusDot_)  statusDot_->setBg(dotColor);
}

void WinSettingAbout::onCheckFinished(int seq, bool ok, const std::wstring& remoteTag)
{
	if (seq != checkSeq_) return;   // 又点了一次：这次结果已过期，丢弃

	appChecked_ = true;
	appOk_ = ok;
	appUpdate_ = false;
	if (ok) {
		const auto remote = parseVersionTag(remoteTag);
		const auto local = Util::getAppVersion();
		appUpdate_ = remote > local;
	}
	if (rackChecked_) finishCheck();
	else scheduleRackFallback(getWeakThis(), this, seq);
}

void WinSettingAbout::onRackCheckFallback(int seq)
{
	// 晾衣架请求超时/失败不会发事件：主程序结果回来后按缓存收尾
	if (seq != checkSeq_ || !checking_ || !appChecked_ || rackChecked_) return;
	rackChecked_ = true;
	rackUpdate_ = Update::rackUpdateAvailable();
	finishCheck();
}

void WinSettingAbout::finishCheck()
{
	if (!checking_ || !appChecked_) return;
	checking_ = false;
	renderCheckResult();
}

void WinSettingAbout::renderCheckResult()
{
	if (spinIcon_) spinIcon_(false);
	updateAction_ = false;
	actionIsDownload_ = false;

	if (rackUpdate_ && appUpdate_) {
		setStatus(Lang::get(L"about.rackAndAppUpdate"), Lang::get(L"about.justNow"), SettingTheme::success);
		updateAction_ = true;
		actionIsDownload_ = true;   // 两者都有：优先打开主程序下载页
	}
	else if (appUpdate_) {
		setStatus(Lang::get(L"about.appUpdate"), Lang::get(L"about.justNow"), SettingTheme::success);
		updateAction_ = true;
		actionIsDownload_ = true;
	}
	else if (rackUpdate_ && ocrUpdate_) {
		setStatus(Lang::get(L"about.componentUpdate"), Lang::get(L"about.justNow"), SettingTheme::success);
		updateAction_ = true;       // 跳插件集成，由用户在插件页更新
	}
	else if (ocrUpdate_) {
		setStatus(Lang::get(L"about.ocrUpdate"), Lang::get(L"about.justNow"), SettingTheme::success);
		updateAction_ = true;
	}
	else if (rackUpdate_) {
		setStatus(Lang::get(L"about.rackUpdate"), Lang::get(L"about.justNow"), SettingTheme::success);
		updateAction_ = true;       // 只跳插件集成，仍由用户在插件页点更新
	}
	else if (appChecked_ && !appOk_) {
		setStatus(Lang::get(L"about.checkFailed"), L"", SettingTheme::destructive);
	}
	else if (appChecked_) {
		setStatus(Lang::get(L"about.upToDate"), Lang::get(L"about.justNow"), SettingTheme::success);
	}
	else {
		setStatus(Lang::get(L"about.notChecked"), L"", SettingTheme::textTertiary);
	}
	applyCheckButton();
}
