#pragma once
// 窗口黑名单匹配规则（纯字符串逻辑，不碰 Win32，便于 SelfTest 直接覆盖）。
//
// 为什么不能只比窗口标题：不少软件（AI 对话、编辑器、浏览器标签页）的窗口标题
// 会随当前会话 / 模式 / 文件名实时改写，用户按「应用名」加进黑名单后，
// 标题一变就再也匹配不上，复制内容仍然被记进历史。
// 所以词条要同时和下面三个信号比对：
//   1. 窗口标题       —— 标题里通常仍带着应用名，且用户原来的词条就是按标题加的
//   2. 进程名         —— 任务管理器「详细信息」里的名字（不含 .exe），标题变了它不变
//   3. 程序文件说明   —— exe 版本信息里的 FileDescription/ProductName，
//                        开始菜单里显示的往往是这个名字，能和进程名互补
//
// 匹配一律不区分大小写、忽略多余空白、忽略结尾的 .exe。
#include <string>
#include <cwctype>

namespace BlacklistMatch {

	// 非 ASCII（中文等）当成普通词字符：C locale 下 iswalnum 不认汉字，
	// 不补这一条，中文词条的边界判断会整段失效。
	inline bool wordChar(wchar_t c)
	{
		return iswalnum(static_cast<wint_t>(c)) != 0 || static_cast<unsigned>(c) > 0x7F;
	}

	// 小写化 + 折叠空白 + 去掉结尾的 .exe（任务管理器/设置页里复制的名字常带扩展名）
	inline std::wstring normalize(const std::wstring& raw)
	{
		std::wstring out;
		out.reserve(raw.size());
		bool pendingSpace = false;
		for (wchar_t c : raw) {
			if (iswspace(static_cast<wint_t>(c))) {
				pendingSpace = !out.empty();
				continue;
			}
			if (pendingSpace) {
				out.push_back(L' ');
				pendingSpace = false;
			}
			out.push_back(static_cast<wchar_t>(towlower(static_cast<wint_t>(c))));
		}
		constexpr size_t kExtLen = 4;   // L".exe"
		if (out.size() > kExtLen && out.compare(out.size() - kExtLen, kExtLen, L".exe") == 0)
			out.resize(out.size() - kExtLen);
		while (!out.empty() && out.back() == L' ') out.pop_back();
		return out;
	}

	// 只留字母数字/汉字：让 "DeepSeek Harness"、"DeepSeek-Harness"、"DeepSeekHarness"
	// 在最后一步兜底时能算同一个名字。
	inline std::wstring compact(const std::wstring& text)
	{
		std::wstring out;
		out.reserve(text.size());
		for (wchar_t c : text)
			if (wordChar(c)) out.push_back(c);
		return out;
	}

	inline bool asciiOnly(const std::wstring& text)
	{
		for (wchar_t c : text)
			if (static_cast<unsigned>(c) > 0x7F) return false;
		return true;
	}

	inline bool contains(const std::wstring& haystack, const std::wstring& needle)
	{
		return !needle.empty() && haystack.find(needle) != std::wstring::npos;
	}

	// needle 在 haystack 里按「整词」出现。仅对纯 ASCII 词条启用：
	// 中文词条走普通包含（中文没有空格分词，硬要边界反而匹配不上）。
	inline bool containsWord(const std::wstring& haystack, const std::wstring& needle)
	{
		if (needle.empty()) return false;
		if (!asciiOnly(needle)) return contains(haystack, needle);
		for (size_t pos = haystack.find(needle); pos != std::wstring::npos;
			pos = haystack.find(needle, pos + 1)) {
			const bool leftOk = pos == 0 || !wordChar(haystack[pos - 1]);
			const bool rightOk = pos + needle.size() >= haystack.size()
				|| !wordChar(haystack[pos + needle.size()]);
			if (leftOk && rightOk) return true;
		}
		return false;
	}

	// entry：黑名单词条；title / process / description：来源窗口的标题、进程名、程序文件说明
	inline bool matches(const std::wstring& entryRaw, const std::wstring& titleRaw,
		const std::wstring& processRaw, const std::wstring& descriptionRaw)
	{
		const std::wstring entry = normalize(entryRaw);
		if (entry.size() < 2) return false;   // 单字符词条范围太大，直接忽略

		const std::wstring title = normalize(titleRaw);
		const std::wstring process = normalize(processRaw);
		const std::wstring description = normalize(descriptionRaw);
		// 一两个字母的词条（AI、QQ…）在标题里必须整词出现，否则 "email" 会命中 "AI"
		const bool shortAscii = asciiOnly(entry) && entry.size() < 3;

		if (!title.empty()) {
			if (shortAscii ? containsWord(title, entry) : contains(title, entry)) return true;
		}
		if (!process.empty()) {
			if (process == entry) return true;
			// 词条是进程名的一部分（"Photoshop" ← "PhotoshopPortable.exe"）
			if (entry.size() >= 3 && contains(process, entry)) return true;
			// 反之：词条是开始菜单里的显示名，进程名是其中一个词
			//（"Visual Studio Code" 的进程名是 Code.exe，"Adobe Illustrator 2026" 是 Illustrator.exe）
			if (process.size() >= 4 && containsWord(entry, process)) return true;
		}
		if (!description.empty()) {
			if (description == entry) return true;
			// 中文/长显示名靠它补：词条在文件说明里，或文件说明是词条里的一个词
			if (entry.size() >= 3 && contains(description, entry)) return true;
			if (description.size() >= 4 && containsWord(entry, description)) return true;
		}
		// 空白/连字符/大小写写法不一致时兜底（至少 4 个字符，避免过短词条误伤）
		const std::wstring ce = compact(entry);
		if (ce.size() >= 4) {
			if (!process.empty() && compact(process) == ce) return true;
			if (!description.empty() && compact(description) == ce) return true;
			if (!title.empty() && contains(compact(title), ce)) return true;
		}
		return false;
	}
}
