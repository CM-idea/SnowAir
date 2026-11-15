#include "pch.h"
#include <chrono>
#include <array>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.System.Threading.h>
#include "Update.h"
#include "Setting.h"
#include "Util.h"
#include "Plugin/PluginHost.h"

namespace {
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Data::Json;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using winrt::Windows::System::Threading::ThreadPoolTimer;

	//今天是第几天（1970-01-01 以来的天数）。一天最多查一次，就靠这个数记账
	long long today()
	{
		auto day = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
		return day.time_since_epoch().count();
	}

	// ── 每日静默检查（GitHub releases）──────────────────────────────────
	// 只判断有没有新版本，不下载；结果缓存起来给「关于」页显示。
	// 只在 UI 线程读写
	std::wstring g_pendingVersion;

	// 晾衣架插件的最新版本 / 检查状态。只在 UI 线程读写。
	std::wstring g_rackLatestVersion;
	bool g_rackChecking{ false };
	winrt::event<winrt::delegate<>>& rackChangedEvent()
	{
		static winrt::event<winrt::delegate<>> event;
		return event;
	}

	// "v0.1.12" / "0.1.12" → {0,1,12}；认不出来返回全 0（等于「没有新版」）
	std::array<int, 3> parseTag(const std::wstring& tag)
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

	// 拉 SnowAir-Rack 的 Releases Atom，只取最新 tag；失败静默。
	winrt::fire_and_forget doRackCheck()
	{
		std::wstring latest;
		try {
			co_await winrt::resume_background();
			winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
			filter.CacheControl().ReadBehavior(
				winrt::Windows::Web::Http::Filters::HttpCacheReadBehavior::NoCache);
			winrt::Windows::Web::Http::HttpClient client{ filter };
			client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"SnowAir");
			auto op = client.GetStringAsync(Uri{
				L"https://github.com/CM-idea/SnowAir-Rack/releases.atom" });
			auto guard = ThreadPoolTimer::CreateTimer(
				[op](const ThreadPoolTimer&) { op.Cancel(); }, std::chrono::seconds(8));
			std::wstring body{ co_await op };
			guard.Cancel();
			const std::wstring tag = Util::parseGithubReleaseTag(body);
			if (!tag.empty()) {
				const auto remote = parseTag(tag);
				if (remote[0] || remote[1] || remote[2])
					latest = wfmt(L"%d.%d.%d", remote[0], remote[1], remote[2]);
			}
		}
		catch (...) {
			// 网络不通 / 超时 / 解析失败：保留上次结果，下次手动检查再试
		}
		if (auto app = Ling::App::get()) {
			app->dq.TryEnqueue([latest]() {
				g_rackChecking = false;
				if (!latest.empty()) {
					g_rackLatestVersion = latest;
					rackChangedEvent()();
				}
			});
		}
		else {
			g_rackChecking = false;
		}
	}

	winrt::fire_and_forget checkGithubLatest()
	{
		std::wstring ver;
		try {
			co_await winrt::resume_background();
			winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
			filter.CacheControl().ReadBehavior(
				winrt::Windows::Web::Http::Filters::HttpCacheReadBehavior::NoCache);
			winrt::Windows::Web::Http::HttpClient client{ filter };
			client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"SnowAir");
			auto op = client.GetStringAsync(Uri{
				L"https://github.com/CM-idea/SnowAir/releases.atom" });
			auto guard = ThreadPoolTimer::CreateTimer(
				[op](const ThreadPoolTimer&) { op.Cancel(); }, std::chrono::seconds(8));
			std::wstring body{ co_await op };
			guard.Cancel();
			const std::wstring tag = Util::parseGithubReleaseTag(body);
			if (!tag.empty()) {
				const auto remote = parseTag(tag);
				if (remote > Ling::Util::getVerNum()) {
					ver = wfmt(L"%d.%d.%d", remote[0], remote[1], remote[2]);
				}
			}
		}
		catch (...) {
			// 网络不通 / 超时 / 返回不是 JSON：静默放弃，明天再来
		}
		if (!ver.empty()) {
			if (auto app = Ling::App::get()) {
				app->dq.TryEnqueue([ver]() { g_pendingVersion = ver; });
			}
		}
	}
}

std::wstring Update::pendingVersion()
{
	return g_pendingVersion;
}

std::wstring Update::rackLatestVersion()
{
	return g_rackLatestVersion;
}

bool Update::rackUpdateAvailable()
{
	if (g_rackLatestVersion.empty()) return false;
	const std::wstring installed = PluginHost::instance().rackVersion();
	if (installed.empty()) return false;
	return parseTag(g_rackLatestVersion) > parseTag(installed);
}

winrt::event<winrt::delegate<>>& Update::onRackUpdateChanged()
{
	return rackChangedEvent();
}

void Update::checkRackUpdate(bool)
{
	// 频率/记账由 checkLater 控制；这里只防并发。手动检查直接走这里。
	if (g_rackChecking) return;
	g_rackChecking = true;
	doRackCheck();
}

void Update::checkLater()
{
	// 按用户设置的频率查：一天 / 一周 / 一月 / 从不；记账在配置里，跨进程有效。
	// 失败也不反复重试，等下一个周期。
	const int freq = Setting::get()->getUpdateCheckFreq();
	if (freq == 3) return;   // 从不
	const long long day = today();
	const long long last = Setting::get()->getUpdateCheckDay();
	const long long interval = (freq == 0) ? 1 : (freq == 1) ? 7 : 30;
	if (last == 0 || day - last >= interval) {
		Setting::get()->setUpdateCheckDay(day);
		checkGithubLatest();
	}
	// 晾衣架有独立版本号，与主程序共用「一天/一周/一月/从不」频率，但单独记账。
	// 这样主程序今天已经查过，也仍会按周期补查插件。
	const int rackLast = Setting::get()->getRackInt(L"rack.updateCheckDay", 0);
	if (rackLast == 0 || day - rackLast >= interval) {
		Setting::get()->setRackInt(L"rack.updateCheckDay", (int)day);
		checkRackUpdate(false);
	}
}
