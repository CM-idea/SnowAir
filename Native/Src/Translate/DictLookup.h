#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

/** 有道风格词典条目（单击译文里的英文单词展开）。 */
struct DictSense { std::wstring pos; std::wstring meaning; };
struct DictForm { std::wstring label; std::wstring value; };
struct DictPhrase { std::wstring en; std::wstring zh; };
struct DictEntry {
	std::wstring word;
	std::wstring brief;         // 简明释义
	std::wstring usPhone;       // 美音
	std::wstring ukPhone;       // 英音
	std::vector<std::wstring> tags;
	std::vector<DictSense> senses;
	std::vector<DictForm> forms;
	std::vector<DictPhrase> phrases;
	std::wstring error;
	bool ok{ false };
};

class DictLookup {
public:
	using Callback = std::function<void(const DictEntry&)>;
	static DictLookup& instance();
	/** 异步查询；命中缓存立即回调。回调统一投递回 UI 线程（Ling::App 调度队列）。 */
	void lookup(const std::wstring& word, Callback cb);
private:
	DictLookup() = default;
	std::map<std::wstring, DictEntry> cache;   // key = 小写单词
};
