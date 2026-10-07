#pragma once
#include <string>
#include <vector>
#include <functional>

// 仅保留非 AI 的翻译引擎：在线免费接口（微软/谷歌/有道/百度）+ 离线 Bergamot。
namespace TranslateProvider {
	inline constexpr const wchar_t* Offline = L"offline";
	inline constexpr const wchar_t* Microsoft = L"microsoft";
	inline constexpr const wchar_t* Google = L"google";
	inline constexpr const wchar_t* Youdao = L"youdao";
	inline constexpr const wchar_t* Baidu = L"baidu";
}

// 快捷翻译 / 功能设置共用的一组主流语种（按默认展示顺序排列）。
// 代码沿用 Google/Bing 通用码：zh-CN / zh-TW 与既有设置值兼容。
struct TranslateLangItem {
	const wchar_t* code;
	const wchar_t* labelKey;   // Lang 键
};

inline constexpr TranslateLangItem kPopularLangs[] = {
	{ L"zh-CN", L"setting.fnLangZh"     },
	{ L"zh-TW", L"setting.fnLangZhHant" },
	{ L"ru",    L"setting.fnLangRu"     },
	{ L"en",    L"setting.fnLangEn"     },
	{ L"ko",    L"setting.fnLangKo"     },
	{ L"ja",    L"setting.fnLangJa"     },
	{ L"fr",    L"setting.fnLangFr"     },
};

inline constexpr int kPopularLangCount = (int)(sizeof(kPopularLangs) / sizeof(kPopularLangs[0]));

// 是否为中文代码（简/繁）
inline bool isChineseLang(const std::wstring& code)
{
	return code.size() >= 2
		&& (code[0] == L'z' || code[0] == L'Z')
		&& (code[1] == L'h' || code[1] == L'H');
}

struct TranslateRequest {
	std::wstring text;
	std::wstring sourceLang{ L"auto" };
	std::wstring targetLang{ L"zh-CN" };
	std::wstring provider;
};

struct TranslateResult {
	bool ok{ false };
	std::wstring text;
	std::wstring detectedSourceLang;
	std::wstring providerUsed;
	std::wstring error;
};

using TranslateCallback = std::function<void(const TranslateResult&)>;
