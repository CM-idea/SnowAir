#include "pch.h"
#include "DictLookup.h"
#include "../Net/HttpClient.h"
#include "../App.h"
#include "../Lang.h"
#include <thread>
#include <winrt/Windows.Data.Json.h>

using namespace winrt::Windows::Data::Json;

namespace {

	std::wstring toLower(std::wstring s)
	{
		for (auto& c : s) c = (wchar_t)towlower(c);
		return s;
	}
	std::wstring trim(const std::wstring& s)
	{
		auto a = s.find_first_not_of(L" \t\r\n");
		if (a == std::wstring::npos) return {};
		auto b = s.find_last_not_of(L" \t\r\n");
		return s.substr(a, b - a + 1);
	}
	// 查询串百分号编码（单词多是 ASCII，中文也能安全带上）
	std::wstring pctEncode(const std::wstring& s)
	{
		std::string u8;
		int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			u8.resize((size_t)n);
			WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), u8.data(), n, nullptr, nullptr);
		}
		static const wchar_t* hex = L"0123456789ABCDEF";
		std::wstring out;
		for (char ch : u8) {
			const unsigned char c = (unsigned char)ch;
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
				|| c == '-' || c == '_' || c == '.' || c == '~')
				out.push_back((wchar_t)c);
			else {
				out.push_back(L'%');
				out.push_back(hex[c >> 4]);
				out.push_back(hex[c & 15]);
			}
		}
		return out;
	}

	// 取对象子节点；数组时取第一项（有道既有 {k:{...}} 也有 {k:[{...}]} 两种形态）
	JsonObject objOf(const JsonObject& parent, const wchar_t* key)
	{
		if (!parent || !parent.HasKey(key)) return JsonObject{ nullptr };
		auto v = parent.GetNamedValue(key);
		if (v.ValueType() == JsonValueType::Object) return v.GetObjectW();
		if (v.ValueType() == JsonValueType::Array) {
			auto a = v.GetArray();
			if (a.Size() > 0 && a.GetAt(0).ValueType() == JsonValueType::Object)
				return a.GetAt(0).GetObjectW();
		}
		return JsonObject{ nullptr };
	}
	JsonArray arrOf(const JsonObject& parent, const wchar_t* key)
	{
		if (!parent || !parent.HasKey(key)) return JsonArray{ nullptr };
		auto v = parent.GetNamedValue(key);
		if (v.ValueType() == JsonValueType::Array) return v.GetArray();
		return JsonArray{ nullptr };
	}
	std::wstring strAt(const JsonObject& o, const wchar_t* key, const std::wstring& def = L"")
	{
		if (!o || !o.HasKey(key)) return def;
		try { return o.GetNamedString(key, L"").c_str(); }
		catch (...) { return def; }
	}
	// 有道里 "l"."i" 既可能是字符串，也可能是字符串数组
	std::wstring liToString(const JsonValue& v)
	{
		if (v.ValueType() == JsonValueType::String) return v.GetString().c_str();
		if (v.ValueType() == JsonValueType::Array) {
			std::wstring out;
			for (auto&& x : v.GetArray()) {
				if (x.ValueType() != JsonValueType::String) continue;
				if (!out.empty()) out += L"；";
				out += x.GetString().c_str();
			}
			return out;
		}
		return L"";
	}
	void appendJoin(std::wstring& dst, const std::wstring& add)
	{
		if (add.empty()) return;
		if (!dst.empty()) dst += L"；";
		dst += add;
	}

	DictEntry parseYoudao(const std::string& body, const std::wstring& word)
	{
		DictEntry e;
		e.word = word;
		JsonObject root{ nullptr };
		try { root = JsonValue::Parse(winrt::to_hstring(body)).GetObjectW(); }
		catch (...) {}
		if (!root) { e.error = Lang::get(L"setting.dictEmpty"); return e; }

		// 简明释义
		if (auto fy = objOf(root, L"fanyi")) {
			e.brief = strAt(fy, L"tran");
			auto input = strAt(fy, L"input");
			if (!input.empty()) e.word = input;
		}

		// 英汉 / 英英
		if (auto ec = objOf(root, L"ec")) {
			auto words = arrOf(ec, L"word");
			if (words && words.Size() > 0 && words.GetAt(0).ValueType() == JsonValueType::Object) {
				auto w0 = words.GetAt(0).GetObjectW();
				if (e.word.empty())
					e.word = strAt(objOf(objOf(w0, L"return-phrase"), L"l"), L"i", word);
				e.ukPhone = strAt(w0, L"ukphone");
				e.usPhone = strAt(w0, L"usphone");

				auto trs = arrOf(w0, L"trs");
				for (auto&& tv : trs) {
					if (tv.ValueType() != JsonValueType::Object) continue;
					auto trObj = tv.GetObjectW();
					const std::wstring pos = trim(strAt(trObj, L"pos"));
					std::wstring meaning;
					auto trArr = arrOf(trObj, L"tr");
					for (auto&& trv : trArr) {
						if (trv.ValueType() != JsonValueType::Object) continue;
						auto l = objOf(trv.GetObjectW(), L"l");
						if (l && l.HasKey(L"i")) appendJoin(meaning, liToString(l.GetNamedValue(L"i")));
					}
					if (meaning.empty()) meaning = trim(strAt(trObj, L"tran"));
					if (meaning.empty()) continue;
					bool dup = false;
					for (auto& s : e.senses)
						if (s.pos == pos && s.meaning == meaning) { dup = true; break; }
					if (!dup) e.senses.push_back({ pos, meaning });
				}

				auto wfs = arrOf(w0, L"wfs");
				for (auto&& fv : wfs) {
					if (fv.ValueType() != JsonValueType::Object) continue;
					auto wf = objOf(fv.GetObjectW(), L"wf");
					auto name = trim(strAt(wf, L"name"));
					auto value = trim(strAt(wf, L"value"));
					if (!name.empty() && !value.empty()) e.forms.push_back({ name, value });
				}
			}
			// 标签（考试类型）
			auto examArr = arrOf(ec, L"exam_type");
			for (auto&& v : examArr)
				if (v.ValueType() == JsonValueType::String) e.tags.push_back(v.GetString().c_str());
		}

		// 短语
		if (auto phrs = objOf(root, L"phrs")) {
			auto list = arrOf(phrs, L"phrs");
			for (auto&& pv : list) {
				if (pv.ValueType() != JsonValueType::Object) continue;
				auto phr = objOf(pv.GetObjectW(), L"phr");
				const std::wstring en = strAt(objOf(objOf(phr, L"headword"), L"l"), L"i");
				std::wstring zh;
				auto trs = arrOf(phr, L"trs");
				if (trs && trs.Size() > 0 && trs.GetAt(0).ValueType() == JsonValueType::Object)
					zh = strAt(objOf(objOf(trs.GetAt(0).GetObjectW(), L"tr"), L"l"), L"i");
				if (en.empty()) continue;
				e.phrases.push_back({ en, zh });
				if (e.phrases.size() >= 8) break;
			}
		}

		if (e.brief.empty() && !e.senses.empty()) {
			int n = 0;
			for (auto& s : e.senses) {
				appendJoin(e.brief, s.pos.empty() ? s.meaning : (s.pos + L" " + s.meaning));
				if (++n >= 2) break;
			}
		}

		e.ok = !e.brief.empty() || !e.senses.empty() || !e.phrases.empty();
		if (!e.ok) e.error = Lang::get(L"setting.dictNotFound");
		return e;
	}
}

DictLookup& DictLookup::instance()
{
	static DictLookup inst;
	return inst;
}

void DictLookup::lookup(const std::wstring& word, Callback cb)
{
	const std::wstring q = trim(word);
	auto post = [](DictEntry e, Callback c) {
		auto entry = std::make_shared<DictEntry>(std::move(e));
		auto cbp = std::make_shared<Callback>(std::move(c));
		if (auto* app = Ling::App::get())
			app->dq.TryEnqueue([entry, cbp]() { if (*cbp) (*cbp)(*entry); });
	};
	if (q.empty()) {
		DictEntry e;
		e.error = Lang::get(L"setting.dictNotFound");
		post(std::move(e), std::move(cb));
		return;
	}
	const std::wstring key = toLower(q);
	auto it = cache.find(key);
	if (it != cache.end()) {
		post(it->second, std::move(cb));
		return;
	}

	std::thread([this, q, key, cb]() {
		auto fetch = [](const std::wstring& url) -> std::string {
			auto r = SnowHttp::get(url, {
				{ L"User-Agent", L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
					L"(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36" },
				{ L"Referer", L"https://dict.youdao.com/" },
				{ L"Accept", L"application/json,text/plain,*/*" },
				});
			return r.ok ? r.body : std::string{};
		};
		DictEntry e;
		auto b1 = fetch(L"https://dict.youdao.com/jsonapi_s?doctype=json&jsonversion=4&q=" + pctEncode(q));
		if (!b1.empty()) e = parseYoudao(b1, q);
		if (!e.ok) {
			auto b2 = fetch(L"https://dict.youdao.com/jsonapi?q=" + pctEncode(q));
			auto e2 = b2.empty() ? DictEntry{} : parseYoudao(b2, q);
			if (e2.ok) e = e2;
		}
		if (!e.ok && e.error.empty()) e.error = Lang::get(L"setting.dictFailed");

		auto entry = std::make_shared<DictEntry>(std::move(e));
		auto cbp = std::make_shared<Callback>(std::move(cb));
		if (auto* app = Ling::App::get()) {
			app->dq.TryEnqueue([this, key, entry, cbp]() {
				if (entry->ok) cache[key] = *entry;
				if (*cbp) (*cbp)(*entry);
			});
		}
	}).detach();
}
