#include "pch.h"
#include "HttpClient.h"
#include <fstream>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Storage.Streams.h>

namespace {
	using namespace winrt;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using namespace winrt::Windows::Web::Http::Headers;
	using namespace winrt::Windows::Storage::Streams;

	winrt::Windows::Web::Http::HttpClient makeClient()
	{
		winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
		filter.AllowAutoRedirect(true);
		winrt::Windows::Web::Http::HttpClient client(filter);
		client.DefaultRequestHeaders().UserAgent().TryParseAdd(
			L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131.0.0.0 Safari/537.36");
		return client;
	}

	::SnowHttp::Response fromResponse(winrt::Windows::Web::Http::HttpResponseMessage const& resp)
	{
		::SnowHttp::Response out;
		out.status = (int)resp.StatusCode();
		auto buf = resp.Content().ReadAsBufferAsync().get();
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
		out = fromResponse(client.SendRequestAsync(req).get());
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
		out = fromResponse(client.SendRequestAsync(req).get());
	}
	catch (winrt::hresult_error const& e) {
		out.error = e.message().c_str();
	}
	catch (...) {
		out.error = L"网络请求失败";
	}
	return out;
}

bool downloadFile(const std::wstring& url, const std::filesystem::path& dest,
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
		auto resp = client.GetAsync(Uri(url), HttpCompletionOption::ResponseHeadersRead).get();
		if (!resp.IsSuccessStatusCode()) return false;
		uint64_t total = 0;
		try { total = resp.Content().Headers().ContentLength().GetUInt64(); }
		catch (...) {}
		auto stream = resp.Content().ReadAsInputStreamAsync().get();
		std::ofstream file(dest, std::ios::binary | std::ios::trunc);
		if (!file) return false;
		Buffer buf(64 * 1024);
		uint64_t got = 0;
		for (;;) {
			auto read = stream.ReadAsync(buf, buf.Capacity(), InputStreamOptions::None).get();
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

} // namespace SnowHttp
