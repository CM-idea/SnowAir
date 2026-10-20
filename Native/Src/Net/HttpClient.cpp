#include "pch.h"
#include "HttpClient.h"
#include <fstream>
#include <chrono>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>

namespace {
	using namespace winrt;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using namespace winrt::Windows::Web::Http::Headers;
	using namespace winrt::Windows::Storage::Streams;
	using namespace winrt::Windows::System::Threading;

	// 单次读的最长等待：网络卡住时 ReadAsync 会一直挂着，超时就 Cancel 掉，交给上层重试
	constexpr int kReadTimeoutSec = 20;
	// 单次 HTTP 请求（发送 + 读响应体）的最长等待：对端"连上却不回"时防止调用线程永久挂起
	constexpr int kHttpTimeoutSec = 20;

	// 等一个 async op 完成；到点就 Cancel，op.get() 会抛，统一按"失败"返回 false。
	// 想区分"超时"和"真失败"时，调用方用返回 false 直接当作超时处理即可（够用）。
	template <class TOp>
	bool waitOp(TOp const& op, int seconds)
	{
		auto guard = ThreadPoolTimer::CreateTimer(
			[op](const ThreadPoolTimer&) { op.Cancel(); },
			std::chrono::seconds(seconds));
		try {
			op.get();
			guard.Cancel();
			return true;
		}
		catch (...) {
			guard.Cancel();
			return false;
		}
	}

	winrt::Windows::Web::Http::HttpClient makeClient()
	{
		winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
		filter.AllowAutoRedirect(true);
		winrt::Windows::Web::Http::HttpClient client(filter);
		client.DefaultRequestHeaders().UserAgent().TryParseAdd(
			L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131.0.0.0 Safari/537.36");
		return client;
	}

	::SnowHttp::Response fromResponse(winrt::Windows::Web::Http::HttpResponseMessage const& resp,
		int timeoutSec = kHttpTimeoutSec)
	{
		::SnowHttp::Response out;
		out.status = (int)resp.StatusCode();
		try { out.finalUrl = resp.RequestMessage().RequestUri().ToString().c_str(); }
		catch (...) {}
		// 读响应体同样要设限：只认 header 不算完，body 卡住一样会挂死调用线程
		auto op = resp.Content().ReadAsBufferAsync();
		if (!waitOp(op, timeoutSec)) {
			out.error = L"读取响应超时";
			return out;
		}
		auto buf = op.GetResults();
		if (buf && buf.Length() > 0) {
			out.body.resize(buf.Length());
			memcpy(out.body.data(), buf.data(), buf.Length());
		}
		out.ok = resp.IsSuccessStatusCode();
		if (!out.ok) out.error = L"HTTP " + std::to_wstring(out.status);
		return out;
	}
}

namespace SnowHttp {

Response get(const std::wstring& url, const std::map<std::wstring, std::wstring>& headers)
{
	using namespace winrt;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Web::Http;
	Response out;
	try {
		auto client = makeClient();
		HttpRequestMessage req(HttpMethod::Get(), Uri(url));
		for (auto& [k, v] : headers) {
			try { req.Headers().TryAppendWithoutValidation(k, v); }
			catch (...) {}
		}
		auto op = client.SendRequestAsync(req);
		if (!waitOp(op, kHttpTimeoutSec)) {
			out.error = L"请求超时";
			return out;
		}
		out = fromResponse(op.GetResults());
	}
	catch (winrt::hresult_error const& e) {
		out.error = e.message().c_str();
	}
	catch (...) {
		out.error = L"网络请求失败";
	}
	return out;
}

Response post(const std::wstring& url, const std::string& bodyUtf8,
	const std::wstring& contentType,
	const std::map<std::wstring, std::wstring>& headers)
{
	using namespace winrt;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Headers;
	using namespace winrt::Windows::Storage::Streams;
	Response out;
	try {
		auto client = makeClient();
		HttpRequestMessage req(HttpMethod::Post(), Uri(url));
		for (auto& [k, v] : headers) {
			if (_wcsicmp(k.c_str(), L"Content-Type") == 0) continue;
			try { req.Headers().TryAppendWithoutValidation(k, v); }
			catch (...) {}
		}
		DataWriter writer;
		writer.WriteBytes(array_view<const uint8_t>(
			reinterpret_cast<const uint8_t*>(bodyUtf8.data()),
			reinterpret_cast<const uint8_t*>(bodyUtf8.data() + bodyUtf8.size())));
		HttpBufferContent bufContent(writer.DetachBuffer());
		bufContent.Headers().ContentType(HttpMediaTypeHeaderValue::Parse(contentType));
		req.Content(bufContent);
		auto op = client.SendRequestAsync(req);
		if (!waitOp(op, kHttpTimeoutSec)) {
			out.error = L"请求超时";
			return out;
		}
		out = fromResponse(op.GetResults());
	}
	catch (winrt::hresult_error const& e) {
		out.error = e.message().c_str();
	}
	catch (...) {
		out.error = L"网络请求失败";
	}
	return out;
}

// 单次下载尝试（带读超时）
static bool downloadOnce(const std::wstring& url, const std::filesystem::path& dest,
	std::function<void(uint64_t, uint64_t)> onProgress)
{
	using namespace winrt;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Storage::Streams;
	try {
		std::error_code ec;
		std::filesystem::create_directories(dest.parent_path(), ec);
		auto client = makeClient();
		auto headOp = client.GetAsync(Uri(url), HttpCompletionOption::ResponseHeadersRead);
		if (!waitOp(headOp, kHttpTimeoutSec)) return false;
		auto resp = headOp.GetResults();
		if (!resp.IsSuccessStatusCode()) return false;
		uint64_t total = 0;
		try { total = resp.Content().Headers().ContentLength().GetUInt64(); }
		catch (...) {}
		auto streamOp = resp.Content().ReadAsInputStreamAsync();
		if (!waitOp(streamOp, kHttpTimeoutSec)) return false;
		auto stream = streamOp.GetResults();
		std::ofstream file(dest, std::ios::binary | std::ios::trunc);
		if (!file) return false;
		Buffer buf(64 * 1024);
		uint64_t got = 0;
		for (;;) {
			auto readOp = stream.ReadAsync(buf, buf.Capacity(), InputStreamOptions::None);
			// 读超时看门狗（waitOp 内置）：超时/连接被掐断 → 本次尝试失败，交给上面重试
			if (!waitOp(readOp, kReadTimeoutSec)) return false;
			auto read = readOp.GetResults();
			if (!read || read.Length() == 0) break;
			file.write(reinterpret_cast<const char*>(read.data()), read.Length());
			got += read.Length();
			if (onProgress) onProgress(got, total);
		}
		return file.good();
	}
	catch (...) {
		return false;
	}
}

bool downloadFile(const std::wstring& url, const std::filesystem::path& dest,
	std::function<void(uint64_t, uint64_t)> onProgress, int attempts)
{
	if (attempts < 1) attempts = 1;
	for (int i = 0; i < attempts; i++) {
		if (downloadOnce(url, dest, onProgress)) return true;
		// 半截文件删掉，重试时从头下；退避一下再试，别对端刚掐断就又冲上去
		std::error_code ec;
		std::filesystem::remove(dest, ec);
		if (i + 1 < attempts) Sleep(700 * (DWORD)(i + 1));
	}
	return false;
}

} // namespace SnowHttp
