#include "pch.h"
#include "TranslateService.h"
#include "TranslateLangs.h"
#include "TranslateModelManager.h"
#include "FreeWebTranslate.h"
#include "BergamotEngine.h"
#include "../Setting.h"
#include <thread>
#include <algorithm>
#include <future>

namespace {
// 这台机器上离线引擎能不能翻这个方向：所需的语言对（可能是 en 枢轴的两段）都装齐了才算
bool offlineCanServe(const std::wstring& sourceLang, const std::wstring& targetLang)
{
	return TranslateModelManager::instance().canServeOffline(sourceLang, targetLang);
}
}

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

// 服务链：用户选的服务优先；失败后按「微软 > 有道 > 百度」依次回退，
// 最后才用离线引擎兜底（装了离线包才有意义）。
// Google **不参与自动兜底**：国内直连几乎必然超时，一次就要等满 20s，
// 会把整条链（以及截图翻译的每一行）拖死；想用 Google 就在设置里明确选它。
std::vector<std::wstring> TranslateService::providerChain(const std::wstring& preferred,
	const std::wstring& sourceLang, const std::wstring& targetLang)
{
	std::vector<std::wstring> chain;
	const std::wstring first = resolveProvider(preferred);
	const bool offlineFirst = _wcsicmp(first.c_str(), TranslateProvider::Offline) == 0;
	// 用户明确选了离线：方向装齐了就只走离线（隐私/断网，也不用为每行等在线超时）。
	// 这个方向的语言包没装时不把离线放在链首 —— 它必然失败（"缺少该方向（ja-en）"），
	// 直接改走在线，避免"OCR 认出了日文却翻不出结果"；离线仍留在链尾，
	// 全挂时由它给出"去插件集成下载"的可操作错误。
	if (offlineFirst && offlineCanServe(sourceLang, targetLang))
		return { first };
	auto push = [&chain](const std::wstring& p) {
		for (auto& c : chain) if (_wcsicmp(c.c_str(), p.c_str()) == 0) return;
		chain.push_back(p);
	};
	if (!offlineFirst) push(first);   // 用户设置的翻译服务（选 Google 时只在这一步进链）
	push(TranslateProvider::Microsoft);
	push(TranslateProvider::Youdao);
	push(TranslateProvider::Baidu);
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

		auto chain = providerChain(base.provider, req.sourceLang, req.targetLang);

		TranslateResult r;
		r.ok = true;
		r.providerUsed = resolveProvider(base.provider);
		size_t okCount = 0;
		std::wstring lastErr;
		enum class LineAction { Keep, Translate };
		struct LinePlan {
			LineAction action{ LineAction::Translate };
			std::wstring sourceLang;
		};
		std::vector<LinePlan> plans(segments.size());
		for (size_t i = 0; i < segments.size(); i++) {
			// 已是目标语的行原样保留，不送翻译。
			if (TranslateLangs::isSameLang(req.targetLang, segments[i])) {
				plans[i].action = LineAction::Keep;
				okCount++;
				continue;
			}
			LinePlan& plan = plans[i];
			plan.sourceLang = req.sourceLang;
			// 自动检测时按行定源语；显式指定源语时尊重用户设置。
			const bool hasHan = TranslateLangs::countScripts(segments[i]).han > 0;
			if (srcAuto) {
				const std::wstring detected = TranslateLangs::detect(segments[i]);
				if (!detected.empty()) plan.sourceLang = detected;
				if (!detected.empty() && !hasHan
					&& _wcsicmp(detected.c_str(), req.targetLang.c_str()) == 0) {
					plan.action = LineAction::Keep;
					okCount++;
					continue;
				}
			}
			// 拉丁字母占多、又夹汉字的行会被判成 en；目标也是 en 时按中文处理。
			if (srcAuto && hasHan && _wcsicmp(plan.sourceLang.c_str(), req.targetLang.c_str()) == 0)
				plan.sourceLang = L"zh-CN";
		}

		std::vector<std::wstring> translated(segments.size());
		for (size_t i = 0; i < segments.size(); i++)
			if (plans[i].action == LineAction::Keep) translated[i] = segments[i];

		for (size_t i = 0; i < segments.size();) {
			if (plans[i].action == LineAction::Keep) {
				i++;
				continue;
			}
			// 连续的、同源语的行合成一批；批量失败或行数对不上时退回逐行。
			size_t end = i;
			size_t chars = 0;
			while (end < segments.size() && plans[end].action == LineAction::Translate
				&& plans[end].sourceLang == plans[i].sourceLang && end - i < 20) {
				if (end > i && chars + segments[end].size() > 4000) break;
				chars += segments[end].size();
				end++;
			}

			bool batched = false;
			if (end - i > 1) {
				std::wstring text;
				for (size_t k = i; k < end; k++) {
					if (!text.empty()) text += L'\n';
					text += segments[k];
				}
				TranslateRequest one = req;
				one.sourceLang = plans[i].sourceLang;
				one.text = text;
				auto tr = runChain(one, chain);
				if (tr.ok) {
					std::vector<std::wstring> lines;
					size_t start = 0;
					for (size_t pos = 0; pos <= tr.text.size(); pos++) {
						if (pos != tr.text.size() && tr.text[pos] != L'\n') continue;
						std::wstring line = tr.text.substr(start, pos - start);
						if (!line.empty() && line.back() == L'\r') line.pop_back();
						lines.push_back(std::move(line));
						start = pos + 1;
					}
					if (lines.size() == end - i) {
						for (size_t k = i; k < end; k++) translated[k] = std::move(lines[k - i]);
						okCount += end - i;
						r.providerUsed = tr.providerUsed;
						batched = true;
					}
				}
			}
			if (!batched) {
				for (size_t k = i; k < end; k++) {
					TranslateRequest one = req;
					one.sourceLang = plans[k].sourceLang;
					one.text = segments[k];
					auto tr = runChain(one, chain);
					if (tr.ok) {
						translated[k] = tr.text;
						r.providerUsed = tr.providerUsed;
						okCount++;
					}
					else {
						translated[k] = segments[k];
						if (lastErr.empty()) lastErr = tr.error.empty() ? L"未知错误" : tr.error;
					}
				}
			}
			i = end;
		}

		for (size_t i = 0; i < segments.size(); i++) {
			std::wstring line = std::move(translated[i]);
			// 每段译文只占一行，调用方按 '\n' 与识别框一一对应。
			for (auto& c : line) {
				if (c == L'\r' || c == L'\n') c = L' ';
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
