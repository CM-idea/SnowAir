#include "pch.h"
#include "CloudOcr.h"
#include "../Net/HttpClient.h"
#include "../Util.h"
#include "../Setting.h"
#include <fstream>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

namespace CloudOcr {
namespace {

std::string base64Encode(const BYTE* data, size_t len)
{
	DWORD outLen = 0;
	CryptBinaryToStringA(data, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outLen);
	std::string out(outLen, '\0');
	CryptBinaryToStringA(data, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &outLen);
	if (!out.empty() && out.back() == '\0') out.pop_back();
	return out;
}

std::wstring utf8ToW(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring out(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
	return out;
}

std::string wToUtf8(const std::wstring& s)
{
	if (s.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::vector<BYTE> encodePng(int w, int h, const BYTE* bgra)
{
	auto tmp = Setting::get()->getDataPath() / L"tmp";
	std::error_code ec;
	std::filesystem::create_directories(tmp, ec);
	auto path = tmp / L"cloud_ocr.png";
	if (!Util::saveToFile(path.wstring(), w, h, const_cast<BYTE*>(bgra))) return {};
	std::ifstream f(path, std::ios::binary);
	std::vector<BYTE> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::filesystem::remove(path, ec);
	return data;
}

} // namespace

OcrResult recognize(int w, int h, const BYTE* bgra, const OcrCloudConfig& cfg)
{
	OcrResult r;
	r.engineUsed = OcrEngineKind::Cloud;
	auto png = encodePng(w, h, bgra);
	if (png.empty()) {
		r.error = L"Unable to encode image";
		return r;
	}
	auto b64 = base64Encode(png.data(), png.size());
	std::string body;
	std::wstring url;
	std::wstring contentType = L"application/x-www-form-urlencoded";

	if (cfg.provider == L"baidu" || cfg.provider.empty()) {
		// Simplified: general OCR API (needs valid token; apiKey used as access_token placeholder)
		url = L"https://aip.baidubce.com/rest/2.0/ocr/v1/general_basic?access_token=" + cfg.apiKey;
		body = "image=" + b64;
	}
	else if (cfg.provider == L"youdao") {
		url = L"https://openapi.youdao.com/ocrapi";
		body = "img=" + b64 + "&appKey=" + wToUtf8(cfg.apiKey);
	}
	else if (!cfg.endpoint.empty()) {
		url = cfg.endpoint;
		body = b64;
		contentType = L"application/octet-stream";
	}
	else {
		r.error = L"Cloud OCR not configured";
		return r;
	}

	auto resp = SnowHttp::post(url, body, contentType);
	if (!resp.ok) {
		r.error = resp.error.empty() ? L"Cloud OCR failed" : resp.error;
		return r;
	}
	try {
		auto doc = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp.body));
		std::wstring plain;
		if (doc.HasKey(L"words_result")) {
			for (auto const& item : doc.GetNamedArray(L"words_result")) {
				auto o = item.GetObjectW();
				auto t = std::wstring(o.GetNamedString(L"words"));
				if (t.empty()) continue;
				OcrRegion reg;
				reg.text = t;
				reg.displayText = t;
				reg.y = (float)r.regions.size() * 22.f;
				reg.w = 200; reg.h = 20;
				r.regions.push_back(reg);
				if (!plain.empty()) plain += L'\n';
				plain += t;
			}
		}
		r.text = plain;
		r.ok = !r.text.empty();
		if (!r.ok) r.error = L"Cloud OCR empty result";
	}
	catch (...) {
		r.text = utf8ToW(resp.body);
		r.ok = !r.text.empty();
		if (!r.ok) r.error = L"Cloud OCR parse failed";
	}
	return r;
}

} // namespace CloudOcr
