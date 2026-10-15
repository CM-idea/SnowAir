#include "pch.h"
#include "TranslateService.h"
#include "TranslateLangs.h"
#include "FreeWebTranslate.h"
#include "BergamotEngine.h"
#include "../Setting.h"
#include <thread>
#include <algorithm>
#include <future>

TranslateService& TranslateService::instance()
{
	static TranslateService inst;
	return inst;
}

std::wstring TranslateService::resolveProvider(const std::wstring& preferred)
{
	std::wstring p = preferred;
	if (p.empty()) p = Setting::get()->getTranslateProvider();
	if (p.empty()) p = TranslateProvider::Microsoft;
	std::wstring lower = p;
	for (auto& c : lower) c = (wchar_t)towlower(c);
	// 老配置里可能残留已移除的 AI/第三方引擎名，统一回落到微软（唯一默认免费引擎）
	if (lower == L"ai" || lower == L"custom" || lower == L"qwen-flash" || lower == L"qwen-plus"
		|| lower == L"deepseek-v4-flash" || lower == L"yandex" || lower == L"bing" || lower == L"deepl") {
		return TranslateProvider::Microsoft;
	}
	return p;
}

// 服务链：用户选的服务优先；失败再依次退到其它在线服务，最后退离线（装了才有意义）
std::vector<std::wstring> TranslateService::providerChain(const std::wstring& preferred)
{
	std::vector<std::wstring> chain;
	auto push = [&chain](const std::wstring& p) {
		for (auto& c : chain) if (_wcsicmp(c.c_str(), p.c_str()) == 0) return;
		chain.push_back(p);
	};
	push(resolveProvider(preferred));        // 用户设置的翻译服务
	push(TranslateProvider::Microsoft);      // 在线主力（内部走 Google 接口）
	push(TranslateProvider::Youdao);         // 另一个在线源
	if (BergamotEngine::isAvailable()) push(TranslateProvider::Offline);   // 离线兜底
	return chain;
}

TranslateResult TranslateService::translateOnce(const TranslateRequest& req)
{
	if (resolveProvider(req.provider) == TranslateProvider::Offline)
		return BergamotEngine::translateSync(req);

	// 在线引擎是异步回调，这里同步等它回来（调用方本来就在工作线程上）
	std::promise<TranslateResult> prom;
	auto fut = prom.get_future();
	FreeWebTranslate::translate(req, [&prom](const TranslateResult& tr) { prom.set_value(tr); });
	return fut.get();
}

TranslateResult TranslateService::runChain(const TranslateRequest& base, std::vector<std::wstring>& chain)
{
	if (base.text.empty()) {
		TranslateResult r;
		r.ok = true;
		r.providerUsed = resolveProvider(base.provider);
		return r;
	}
	TranslateResult last;
	for (size_t i = 0; i < chain.size(); i++) {
		TranslateRequest r = base;
		r.provider = chain[i];
		auto res = translateOnce(r);
		if (res.ok) {
			// 命中的服务提到链首：多段翻译时后续分段直接用它，不再从头逐级试
			std::rotate(chain.begin(), chain.begin() + i, chain.begin() + i + 1);
			return res;
		}
		last = res;
	}
	return last;
}

void TranslateService::translate(const TranslateRequest& req, TranslateCallback cb)
{
	if (req.text.empty()) {
		TranslateResult out;
		out.ok = true;
		out.providerUsed = resolveProvider(req.provider);
		if (cb) cb(out);
		return;
	}
	translateSegments(std::vector<std::wstring>{ req.text }, req, cb);
}

void TranslateService::translateSegments(const std::vector<std::wstring>& segments,
	const TranslateRequest& base, TranslateCallback cb)
{
	if (segments.empty()) {
		TranslateResult r;
		r.ok = true;
		r.providerUsed = resolveProvider(base.provider);
		if (cb) cb(r);
		return;
	}

	std::thread([segments, base, cb]() {
		// 先按**整段文本**定"整块"方向（比分段各自判更准）：源是 auto 就检测补上；
		// 整块已经是目标语时自动换向（治"选中的英文配目标 en，等于没翻"的老问题）。
		std::wstring joined;
		for (auto& s : segments) {
			if (!joined.empty()) joined += L'\n';
			joined += s;
		}
		const bool srcAuto = base.sourceLang.empty()
			|| _wcsicmp(base.sourceLang.c_str(), L"auto") == 0;
		TranslateRequest req = base;
		TranslateLangs::resolveDirection(req.sourceLang, req.targetLang, joined);

		auto chain = providerChain(base.provider);

		TranslateResult r;
		r.ok = true;
		r.providerUsed = resolveProvider(base.provider);
		size_t okCount = 0;
		std::wstring lastErr;
		for (size_t i = 0; i < segments.size(); i++) {
			std::wstring line;
			// ① 这一段本身就是目标语（脚本可确认：目标为中日韩时）→ 原样保留：
			//    既不浪费一次请求，也不会被"反向翻译"翻坏。多语混排的选区就靠这条各按各的。
			if (TranslateLangs::isSameLang(req.targetLang, segments[i])) {
				line = segments[i];
				okCount++;
			}
			else {
				TranslateRequest one = req;
				one.text = segments[i];
				// ② 逐段定源语：用户设的是 auto 时按这一段的脚本判（混排时各行方向可以不同）；
				//    用户显式指定了源语就尊重用户，不覆盖。
				if (srcAuto) {
					auto d = TranslateLangs::detect(segments[i]);
					if (!d.empty()) one.sourceLang = d;
				}
				auto tr = runChain(one, chain);
				if (tr.ok) {
					r.providerUsed = tr.providerUsed;
					line = tr.text;
					okCount++;
				}
				else {
					// 这一行失败：保留原文，不拖垮其它已经翻好的行
					line = segments[i];
					if (lastErr.empty()) lastErr = tr.error.empty() ? L"未知错误" : tr.error;
				}
			}
			if (i) r.text += L'\n';
			r.text += line;
		}
		// 全失败 → 报失败（面板退回显示原文 + 红条）；部分失败 → 出结果，另给个提示
		if (okCount == 0) {
			r.ok = false;
			r.error = lastErr;
		}
		else if (!lastErr.empty()) {
			r.partial = true;
			r.error = lastErr;
		}
		if (cb) cb(r);
	}).detach();
}
