#include "pch.h"
#include "TranslateService.h"
#include "FreeWebTranslate.h"
#include "BergamotEngine.h"
#include "../Setting.h"
#include <thread>
#include <sstream>
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

void TranslateService::dispatch(const TranslateRequest& req, TranslateCallback cb)
{
	TranslateRequest r = req;
	r.provider = resolveProvider(req.provider);
	if (r.text.empty()) {
		TranslateResult out;
		out.ok = true;
		out.providerUsed = r.provider;
		if (cb) cb(out);
		return;
	}
	if (r.provider == TranslateProvider::Offline) {
		std::thread([r, cb]() {
			if (cb) cb(BergamotEngine::translateSync(r));
		}).detach();
		return;
	}
	FreeWebTranslate::translate(r, cb);
}

void TranslateService::translate(const TranslateRequest& req, TranslateCallback cb)
{
	dispatch(req, cb);
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
	auto provider = resolveProvider(base.provider);

	// 在线/离线引擎都不支持批量分隔符协议：在工作线程里逐条同步翻译后拼回多行。
	std::thread([this, segments, base, provider, cb]() {
		TranslateResult r;
		r.ok = true;
		r.providerUsed = provider;
		for (size_t i = 0; i < segments.size(); i++) {
			TranslateRequest req = base;
			req.provider = provider;
			req.text = segments[i];
			std::promise<TranslateResult> prom;
			auto fut = prom.get_future();
			dispatch(req, [&prom](const TranslateResult& tr) { prom.set_value(tr); });
			auto tr = fut.get();
			if (!tr.ok) {
				if (cb) cb(tr);
				return;
			}
			if (i) r.text += L'\n';
			r.text += tr.text;
		}
		if (cb) cb(r);
	}).detach();
}
