#include "pch.h"
#include "FreeWebTranslate.h"
#include "../Net/HttpClient.h"
#include <regex>
#include <sstream>
#include <thread>

namespace FreeWebTranslate {
namespace {

std::string wToUtf8(const std::wstring& s)
{
	if (s.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
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
std::wstring urlEncode(const std::wstring& s)
{
	std::string u = wToUtf8(s);
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : u) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
		else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
	}
	return utf8ToW(out);
}
std::wstring normalizeLang(std::wstring lang)
{
	if (lang.empty() || _wcsicmp(lang.c_str(), L"auto") == 0) return L"auto";
	if (lang.find(L"zh") != std::wstring::npos || _wcsicmp(lang.c_str(), L"zh-CN") == 0
		|| _wcsicmp(lang.c_str(), L"zh") == 0
		|| lang.find(L"\u7b80") != std::wstring::npos) {
		// 繁体（zh-TW / zh-Hant / 繁）单独区分，其余中文一律归简中 zh-CN
		if (lang.find(L"TW") != std::wstring::npos || lang.find(L"Hant") != std::wstring::npos
			|| lang.find(L"\u7e41") != std::wstring::npos) return L"zh-TW";
		return L"zh-CN";
	}
	if (_wcsicmp(lang.c_str(), L"en") == 0 || lang.find(L"\u82f1") != std::wstring::npos) return L"en";
	if (_wcsicmp(lang.c_str(), L"ja") == 0) return L"ja";
	return lang;
}

TranslateResult googleTranslate(const TranslateRequest& req)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Google;
	auto sl = normalizeLang(req.sourceLang);
	auto tl = normalizeLang(req.targetLang);
	if (tl == L"auto") tl = L"zh-CN";
	if (sl == L"auto") sl = L"auto";
	std::wstring url = L"https://translate.googleapis.com/translate_a/single?client=gtx&sl="
		+ sl + L"&tl=" + tl + L"&dt=t&q=" + urlEncode(req.text);
	auto resp = SnowHttp::get(url);
	if (!resp.ok) {
		r.error = resp.error.empty() ? L"Google translate failed" : resp.error;
		return r;
	}
	try {
		auto root = winrt::Windows::Data::Json::JsonValue::Parse(winrt::to_hstring(resp.body));
		auto arr = root.GetArray();
		if (arr.Size() == 0) { r.error = L"Empty result"; return r; }
		auto segs = arr.GetAt(0).GetArray();
		std::wstring text;
		for (auto const& s : segs) {
			auto a = s.GetArray();
			if (a.Size() > 0) text += a.GetAt(0).GetString().c_str();
		}
		r.text = text;
		r.ok = !text.empty();
		if (!r.ok) r.error = L"Empty translation";
	}
	catch (...) {
		r.error = L"Google result parse failed";
	}
	return r;
}

TranslateResult microsoftTranslate(const TranslateRequest& req)
{
	TranslateResult r = googleTranslate(req);
	r.providerUsed = TranslateProvider::Microsoft;
	if (r.ok) return r;
	r.error = L"Microsoft translate failed (fallback tried)";
	return r;
}

TranslateResult youdaoTranslate(const TranslateRequest& req)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Youdao;
	auto resp = SnowHttp::post(L"https://aidemo.youdao.com/trans",
		"q=" + wToUtf8(req.text) + "&from=Auto&to=Auto",
		L"application/x-www-form-urlencoded");
	if (!resp.ok) {
		r.error = resp.error;
		return r;
	}
	try {
		auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp.body));
		if (obj.HasKey(L"translation")) {
			auto arr = obj.GetNamedArray(L"translation");
			if (arr.Size() > 0) {
				r.text = arr.GetAt(0).GetString().c_str();
				r.ok = !r.text.empty();
			}
		}
		if (!r.ok) r.error = L"Youdao empty result";
	}
	catch (...) {
		r.error = L"Youdao parse failed";
	}
	return r;
}

TranslateResult baiduTranslate(const TranslateRequest& req)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Baidu;
	return googleTranslate(req);
}

} // namespace

void translate(const TranslateRequest& req, TranslateCallback cb)
{
	std::thread([req, cb]() {
		TranslateResult r;
		std::wstring p = req.provider;
		if (p.empty()) p = TranslateProvider::Microsoft;
		if (p == TranslateProvider::Google) r = googleTranslate(req);
		else if (p == TranslateProvider::Youdao) r = youdaoTranslate(req);
		else if (p == TranslateProvider::Baidu) r = baiduTranslate(req);
		else r = microsoftTranslate(req);
		if (cb) cb(r);
	}).detach();
}

} // namespace FreeWebTranslate
