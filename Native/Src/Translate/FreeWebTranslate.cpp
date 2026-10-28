#include "pch.h"
#include "FreeWebTranslate.h"
#include "../Net/HttpClient.h"
#include <cstring>
#include <thread>
#include <mutex>
#include <wincrypt.h>

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
// 按 UTF-8 字节百分号编码（查询串与表单体都用它）
std::string urlEncodeUtf8(const std::wstring& s)
{
	std::string u = wToUtf8(s);
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(u.size() * 3);
	for (unsigned char c : u) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
		else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
	}
	return out;
}
std::wstring urlEncode(const std::wstring& s)
{
	return utf8ToW(urlEncodeUtf8(s));
}

// ---- 从网页里取凭证 ------------------------------------------------------
// 原来用 4 条 std::regex，但都是"找到某个标记，跳过空白，遇到分隔符和引号，
// 读一段值"这一种形状。手写等价扫描：同一个标记在页面里可能出现多次（IG、
// IID 这类短词尤其容易），所以一次匹配失败要继续往后找，而不是直接放弃。
bool findQuotedAfter(const std::string& web, const char* marker, char sep, std::string& out)
{
	const size_t mLen = std::strlen(marker);
	for (size_t from = 0;;) {
		size_t p = web.find(marker, from);
		if (p == std::string::npos) return false;
		p += mLen;
		auto skipWs = [&web, &p]() {
			while (p < web.size() && (web[p] == ' ' || web[p] == '\t' || web[p] == '\r' || web[p] == '\n')) ++p;
		};
		skipWs();
		if (p < web.size() && web[p] == sep) {
			++p;
			skipWs();
			if (p < web.size() && web[p] == '"') {
				const size_t end = web.find('"', p + 1);
				if (end != std::string::npos && end > p + 1) {
					out.assign(web, p + 1, end - p - 1);
					return true;
				}
			}
		}
		from = p + 1;
	}
}

// params_AbusePreventionHelper = [ <毫秒时间戳> , "<token>" ]
bool readAbuseHelper(const std::string& web, std::string& key, std::string& token)
{
	static const char* kMarker = "params_AbusePreventionHelper";
	const size_t mLen = std::strlen(kMarker);
	for (size_t from = 0;;) {
		size_t p = web.find(kMarker, from);
		if (p == std::string::npos) return false;
		p += mLen;
		auto skipWs = [&web, &p]() {
			while (p < web.size() && (web[p] == ' ' || web[p] == '\t' || web[p] == '\r' || web[p] == '\n')) ++p;
		};
		skipWs();
		if (p < web.size() && web[p] == '=') { ++p; skipWs(); }
		if (p < web.size() && web[p] == '[') {
			++p;
			skipWs();
			const size_t digitStart = p;
			while (p < web.size() && web[p] >= '0' && web[p] <= '9') ++p;
			if (p > digitStart) {
				key.assign(web, digitStart, p - digitStart);
				skipWs();
				if (p < web.size() && web[p] == ',') {
					++p;
					skipWs();
					if (p < web.size() && web[p] == '"') {
						const size_t end = web.find('"', p + 1);
						if (end != std::string::npos && end > p + 1) {
							token.assign(web, p + 1, end - p - 1);
							return true;
						}
					}
				}
			}
		}
		from = p + 1;
	}
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

// MD5 十六进制小写（百度签名要求）
std::string md5Hex(const std::string& s)
{
	HCRYPTPROV prov = 0;
	HCRYPTHASH hash = 0;
	std::string out;
	if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
		return out;
	if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
		CryptReleaseContext(prov, 0);
		return out;
	}
	CryptHashData(hash, reinterpret_cast<const BYTE*>(s.data()), (DWORD)s.size(), 0);
	BYTE digest[16]{};
	DWORD len = sizeof(digest);
	if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0) && len == sizeof(digest)) {
		static const char* hex = "0123456789abcdef";
		out.reserve(32);
		for (BYTE b : digest) { out += hex[b >> 4]; out += hex[b & 15]; }
	}
	CryptDestroyHash(hash);
	CryptReleaseContext(prov, 0);
	return out;
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

// ============ 微软：Bing 翻译网页接口（免费，无需密钥） ============
// 先取 /translator 页面里的 params_AbusePreventionHelper(key/token)、IG、data-iid，
// 再 POST /ttranslatev3?IG=..&IID=..。
// 注意：凭证与站点绑定 —— 页面被重定向到 cn.bing.com 时，POST 也必须发到 cn.bing.com，
// 否则接口会回 200 + 空体。host 由响应里的最终地址决定。

std::wstring bingLang(const std::wstring& lang)
{
	if (lang.empty() || _wcsicmp(lang.c_str(), L"auto") == 0) return L"auto-detect";
	const std::wstring k = normalizeLang(lang);
	if (_wcsicmp(k.c_str(), L"zh-CN") == 0) return L"zh-Hans";
	if (_wcsicmp(k.c_str(), L"zh-TW") == 0) return L"zh-Hant";
	return k;
}

struct BingCreds {
	std::wstring host;    // 如 https://cn.bing.com
	std::wstring key;     // params_AbusePreventionHelper 的毫秒时间戳
	std::wstring token;
	std::wstring ig;
	std::wstring iid;
};

std::mutex g_bingMtx;
BingCreds g_bing;
bool g_bingValid{ false };

std::wstring originOf(const std::wstring& url, const std::wstring& fallback)
{
	auto p = url.find(L"://");
	if (p == std::wstring::npos) return fallback;
	auto slash = url.find(L'/', p + 3);
	return slash == std::wstring::npos ? url : url.substr(0, slash);
}

bool fetchBingCreds(const std::wstring& base, BingCreds& out, std::wstring& err)
{
	std::map<std::wstring, std::wstring> headers;
	headers[L"Accept"] = L"application/json,text/plain,*/*;q=0.8";
	headers[L"Accept-Language"] = L"zh-CN,zh;q=0.9,en-US;q=0.8,en;q=0.7";
	const std::wstring pageUrl = base + L"/translator";
	auto resp = SnowHttp::get(pageUrl, headers);
	if (!resp.ok) {
		err = resp.error.empty() ? L"Bing 页面请求失败" : resp.error;
		return false;
	}
	const std::string& web = resp.body;
	BingCreds c;
	std::string key, token;
	if (!readAbuseHelper(web, key, token)) {
		err = L"无法解析 Bing 页面凭证（页面结构可能已变更）";
		return false;
	}
	c.key = utf8ToW(key);
	c.token = utf8ToW(token);
	std::string ig;
	if (!findQuotedAfter(web, "IG", ':', ig)) {
		err = L"无法解析 Bing 页面 IG";
		return false;
	}
	c.ig = utf8ToW(ig);
	std::string iid;
	if (!findQuotedAfter(web, "data-iid", '=', iid) && !findQuotedAfter(web, "IID", ':', iid)) {
		err = L"无法解析 Bing 页面 IID";
		return false;
	}
	c.iid = utf8ToW(iid);
	c.host = originOf(resp.finalUrl, base);   // 跟随重定向：凭证属于真正的站点
	out = c;
	return true;
}

void invalidateBingCreds()
{
	std::lock_guard<std::mutex> lock(g_bingMtx);
	g_bingValid = false;
}

// 用一份凭证发一次 /ttranslatev3。返回 ok=false 时调用方会作废凭证并换站点重试。
TranslateResult bingTranslateOnce(const TranslateRequest& req, const BingCreds& c)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Microsoft;

	std::map<std::wstring, std::wstring> headers;
	headers[L"Accept"] = L"application/json,text/plain,*/*;q=0.8";
	headers[L"Referer"] = c.host + L"/translator";
	headers[L"Origin"] = c.host;

	const std::string body = "text=" + urlEncodeUtf8(req.text)
		+ "&fromLang=" + wToUtf8(bingLang(req.sourceLang))
		+ "&to=" + wToUtf8(bingLang(req.targetLang))
		+ "&token=" + wToUtf8(c.token)
		+ "&key=" + wToUtf8(c.key);
	const std::wstring url = c.host + L"/ttranslatev3?IG=" + c.ig + L"&IID=" + c.iid;
	auto resp = SnowHttp::post(url, body, L"application/x-www-form-urlencoded", headers);
	if (!resp.ok) {
		r.error = resp.error.empty() ? L"Bing 翻译请求失败" : resp.error;
		return r;
	}
	if (resp.body.empty()) {
		// 站点与凭证不匹配（或凭证过期）时 Bing 会回 200 + 空体
		r.error = L"Bing 返回空响应";
		return r;
	}
	try {
		auto arr = winrt::Windows::Data::Json::JsonArray::Parse(winrt::to_hstring(resp.body));
		if (arr.Size() == 0) {
			r.error = L"Bing 返回空结果";
			return r;
		}
		auto obj = arr.GetObjectAt(0);
		if (obj.HasKey(L"statusCode")) {
			const int code = (int)obj.GetNamedNumber(L"statusCode", 0);
			r.error = obj.GetNamedString(L"errorMessage", L"").c_str();
			if (r.error.empty())
				r.error = L"Bing 翻译失败（" + std::to_wstring(code) + L"）";
			return r;
		}
		if (obj.HasKey(L"detectedLanguage")) {
			auto det = obj.GetNamedObject(L"detectedLanguage");
			if (det.HasKey(L"language"))
				r.detectedSourceLang = det.GetNamedString(L"language").c_str();
		}
		auto translations = obj.GetNamedArray(L"translations");
		if (translations.Size() > 0)
			r.text = translations.GetObjectAt(0).GetNamedString(L"text").c_str();
		r.ok = !r.text.empty();
		if (!r.ok) r.error = L"Bing 返回空结果";
	}
	catch (...) {
		r.error = L"Bing 返回格式异常";
	}
	return r;
}

TranslateResult microsoftTranslate(const TranslateRequest& req)
{
	// 凭证与站点绑定：页面在哪，POST 就得发到哪。这里按 cn → www 依次试；
	// 空响应/异常直接作废凭证，换下一个站点重新取页面。
	const wchar_t* bases[] = { L"https://cn.bing.com", L"https://www.bing.com" };
	TranslateResult last;
	last.providerUsed = TranslateProvider::Microsoft;
	for (const wchar_t* base : bases) {
		BingCreds c;
		{
			std::lock_guard<std::mutex> lock(g_bingMtx);
			if (g_bingValid) c = g_bing;
		}
		if (c.host.empty()) {
			std::wstring err;
			if (!fetchBingCreds(base, c, err)) {
				last.error = err.empty() ? L"Bing 翻译凭证获取失败" : err;
				continue;
			}
			std::lock_guard<std::mutex> lock(g_bingMtx);
			g_bing = c;
			g_bingValid = true;
		}
		auto tr = bingTranslateOnce(req, c);
		last = tr;              // 失败时把最后一次的错误留给调用方
		if (tr.ok) return tr;
		invalidateBingCreds();   // 失败：凭证/站点作废，接下来重新取
	}
	return last;
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

// ============ 百度：官方通用翻译 API（AppId + MD5 Sign） ============
std::wstring baiduLang(const std::wstring& lang)
{
	if (lang.empty() || _wcsicmp(lang.c_str(), L"auto") == 0) return L"auto";
	const std::wstring k = normalizeLang(lang);
	if (_wcsicmp(k.c_str(), L"zh-CN") == 0) return L"zh";
	if (_wcsicmp(k.c_str(), L"zh-TW") == 0) return L"cht";
	if (_wcsicmp(k.c_str(), L"ja") == 0) return L"jp";
	if (_wcsicmp(k.c_str(), L"ko") == 0) return L"kor";
	if (_wcsicmp(k.c_str(), L"fr") == 0) return L"fra";
	if (_wcsicmp(k.c_str(), L"es") == 0) return L"spa";
	return k;
}

std::wstring newSalt()
{
	GUID g{};
	CoCreateGuid(&g);
	wchar_t buf[40]{};
	swprintf_s(buf, L"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
		g.Data1, g.Data2, g.Data3,
		g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
		g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	return buf;
}

TranslateResult baiduTranslate(const TranslateRequest& req)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Baidu;

	// 百度通用翻译 API 的 AppId / 密钥（旧的内置凭证已失效，会返回 54001 Invalid Sign）
	static const std::wstring appId = L"20240601002067575";
	static const std::wstring secret = L"FR7xu9EngQpmqAcvcC27";

	const std::wstring q = req.text.size() > 5000 ? req.text.substr(0, 5000) : req.text;
	const std::wstring salt = newSalt();
	const std::string sign = md5Hex(wToUtf8(appId + q + salt + secret));
	if (sign.empty()) {
		r.error = L"百度签名计算失败";
		return r;
	}

	const std::string body = "q=" + urlEncodeUtf8(q)
		+ "&from=" + wToUtf8(baiduLang(req.sourceLang))
		+ "&to=" + wToUtf8(baiduLang(req.targetLang))
		+ "&appid=" + wToUtf8(appId)
		+ "&salt=" + wToUtf8(salt)
		+ "&sign=" + sign;
	auto resp = SnowHttp::post(L"https://fanyi-api.baidu.com/api/trans/vip/translate",
		body, L"application/x-www-form-urlencoded");
	if (!resp.ok) {
		r.error = resp.error.empty() ? L"百度翻译请求失败" : resp.error;
		return r;
	}
	try {
		auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp.body));
		if (obj.HasKey(L"error_code")) {
			r.error = obj.GetNamedString(L"error_msg", L"百度翻译失败").c_str();
			return r;
		}
		auto arr = obj.GetNamedArray(L"trans_result");
		std::wstring text;
		for (uint32_t i = 0; i < arr.Size(); i++) {
			auto item = arr.GetObjectAt(i);
			if (i) text += L'\n';
			text += item.GetNamedString(L"dst", L"").c_str();
		}
		r.text = text;
		if (obj.HasKey(L"from")) r.detectedSourceLang = obj.GetNamedString(L"from").c_str();
		r.ok = !r.text.empty();
		if (!r.ok) r.error = L"百度返回空结果";
	}
	catch (...) {
		r.error = L"百度返回格式异常";
	}
	return r;
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
