#pragma once
#include <string>
#include <vector>
#include <cwctype>
#include "TranslateTypes.h"

// 语种归一 / 原文粗检测 / 自动换向 / 语言对解析。
// 供 TranslateService（检测与换向）与 BergamotEngine（选离线语言对）共用。
namespace TranslateLangs {

	// app 语种码 → 离线模型用的基码：zh-CN→zh、zh-TW→zh_hant，其余（en/ru/ko/ja/fr/es/de）原样小写
	inline std::wstring baseCode(const std::wstring& lang)
	{
		if (_wcsicmp(lang.c_str(), L"zh-CN") == 0 || _wcsicmp(lang.c_str(), L"zh") == 0) return L"zh";
		if (_wcsicmp(lang.c_str(), L"zh-TW") == 0 || _wcsicmp(lang.c_str(), L"zh-Hant") == 0) return L"zh_hant";
		std::wstring s = lang;
		for (auto& c : s) c = (wchar_t)towlower(c);
		return s;
	}

	// 脚本计数（只数"字母类"字符：假名 / 谚文 / 汉字 / 西里尔 / 拉丁）
	struct Scripts {
		int kana{ 0 }, hangul{ 0 }, han{ 0 }, cyr{ 0 }, lat{ 0 };
		int letters() const { return kana + hangul + han + cyr + lat; }
	};

	inline Scripts countScripts(const std::wstring& text)
	{
		Scripts s;
		for (size_t i = 0; i < text.size(); i++) {
			wchar_t c = text[i];
			if (c >= 0xD800 && c <= 0xDBFF) { ++i; continue; }   // 非 BMP（emoji 等）不参与判断
			if (c >= 0x3040 && c <= 0x30FF) ++s.kana;
			else if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF)) ++s.hangul;
			else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF)) ++s.han;
			else if (c >= 0x0400 && c <= 0x04FF) ++s.cyr;
			else if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) ++s.lat;
		}
		return s;
	}

	// 粗粒度原文检测（只看字符脚本，零依赖、够用）：
	//   假名→ja；谚文→ko；西里尔多于拉丁→ru；**汉字过半**→zh-CN；其余拉丁→en；认不出→空
	// 注意"按比例"判：早先是"见一个汉字就算中文"，中英混排里夹一个汉字就会把整段判成中文。
	inline std::wstring detect(const std::wstring& text)
	{
		const auto s = countScripts(text);
		if (s.letters() <= 0) return L"";
		if (s.kana > 0) return L"ja";                    // 假名是日文强特征（中文文本不会出现）
		if (s.hangul > 0) return L"ko";                  // 谚文同理
		if (s.cyr > 0 && s.cyr > s.lat) return L"ru";
		if (s.han * 2 >= s.letters()) return L"zh-CN";   // 汉字过半才算中文
		if (s.lat > 0) return L"en";
		return L"zh-CN";
	}

	// 该语种脚本占全文"字母"的比例（0~1）：用来判"整段基本就是某种语言"（换向门限 85%）。
	// 拉丁语系（en/fr/de/es）脚本相同、分不出来，只能按"拉丁占比"算。
	inline float langShare(const std::wstring& lang, const std::wstring& text)
	{
		const auto s = countScripts(text);
		if (s.letters() <= 0) return 0.f;
		const float n = (float)s.letters();
		if (isChineseLang(lang)) return (float)s.han / n;
		if (_wcsicmp(lang.c_str(), L"ja") == 0) return (float)(s.kana + s.han) / n;
		if (_wcsicmp(lang.c_str(), L"ko") == 0) return (float)s.hangul / n;
		if (_wcsicmp(lang.c_str(), L"ru") == 0) return (float)s.cyr / n;
		return (float)s.lat / n;
	}

	// 这段文字是否"已经是目标语"。只在脚本能可靠区分时才敢答"是"：
	// 目标为中日韩（看汉字/假名/谚文）时，混排里这些脚本一眼可辨；
	// 目标若是拉丁语系（en/fr/de 脚本相同，无法区分），一律返回 false —— 照常送翻。
	inline bool isSameLang(const std::wstring& lang, const std::wstring& text)
	{
		if (lang.empty()) return false;
		const bool cjk = isChineseLang(lang) || _wcsicmp(lang.c_str(), L"ja") == 0
			|| _wcsicmp(lang.c_str(), L"ko") == 0;
		if (!cjk) return false;
		auto d = detect(text);
		if (d.empty()) return false;
		if (isChineseLang(lang) && isChineseLang(d)) return true;   // 简繁同族都算
		return _wcsicmp(d.c_str(), lang.c_str()) == 0;
	}

	// 自动换向：源为空/auto 时用检测结果补上；之后若源与目标同语种，把目标换到对侧（中↔英）。
	// 默认配置是 auto→en，选中英文文本时正好会撞上"en→en"，这里换成 en→zh-CN。
	inline void resolveDirection(std::wstring& src, std::wstring& tgt, const std::wstring& text)
	{
		if (src.empty() || _wcsicmp(src.c_str(), L"auto") == 0) {
			auto d = detect(text);
			if (!d.empty()) src = d;
		}
		// 还是 auto（认不出语种）：别乱换目标，交给在线引擎自动识别
		if (src.empty() || _wcsicmp(src.c_str(), L"auto") == 0) return;
		// 源与目标撞车（默认 auto→en 遇到英文即 en→en）：只有当整段"确实基本就是目标语"时才换向，
		// 否则保持用户设的目标不动 —— 中英混排里夹几个英文字母不该把 zh→en 翻成 en→zh。
		if (_wcsicmp(src.c_str(), tgt.c_str()) == 0 && langShare(tgt, text) >= 0.85f)
			tgt = isChineseLang(tgt) ? L"en" : L"zh-CN";
	}

	// 需要的语言对（pairId）：1 个 = 直译；2 个 = 以英语为枢轴的接力翻译
	inline std::vector<std::wstring> pairIds(const std::wstring& src, const std::wstring& tgt)
	{
		auto s = baseCode(src), t = baseCode(tgt);
		if (s.empty() || t.empty() || s == t) return {};
		if (t == L"en") return { s + L"-en" };
		if (s == L"en") return { L"en-" + t };
		return { s + L"-en", L"en-" + t };
	}

} // namespace TranslateLangs
