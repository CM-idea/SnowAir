#include "pch.h"
#include <filesystem>
#include "Lang.h"
#include "Util.h"
#include "Setting.h"

std::unique_ptr<Lang> lang;

namespace {
	// 语言文件优先从 exe 同目录的 Lang 子目录加载（绿色部署 / 调试方便），
	// 那个目录不存在才退回 %appdata%\SnowAir\Lang。
	// directory_iterator 碰到不存在的目录会抛 filesystem_error，
	// 所以用带 error_code 的重载，目录不在就当没有额外语言。
	std::vector<std::filesystem::path> getLangFiles()
	{
		std::vector<std::filesystem::path> result;
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileName(nullptr, buffer, MAX_PATH);
		auto langPath = std::filesystem::path{ buffer }.parent_path().append(L"Lang");
		if (!std::filesystem::is_directory(langPath)) {
			langPath = Setting::get()->getDataPath().append(L"Lang");
		}
		std::error_code ec;
		std::filesystem::directory_iterator it{ langPath, ec }, end{};
		for (; !ec && it != end; it.increment(ec)) {
			result.push_back(it->path());
		}
		return result;
	}
}

Lang::Lang()
{
}

Lang::~Lang()
{
}

void Lang::init()
{
	auto ptr = new Lang();
	lang.reset(ptr);
	lang->initLang(Setting::get()->getLang());
}

void Lang::dispose()
{
	lang.reset();
}

Lang* Lang::get()
{
	return lang.get();
}

std::wstring Lang::get(const std::wstring& keyPath)
{
	// 全程使用带默认值的重载：第三方语言文件缺键属正常情况（程序新增文案后，其文件仍是旧版），
	// 而带默认值的 GetNamedObject/GetNamedString 不会抛异常。缺失时先回退内置 en-US，
	// 仍无则直接显示键名 —— 界面观感较差，但优于异常，且可直接看出缺哪个键。
	// 支持任意层级的点路径（如 imageEdit.tool.split）：中间段必须是对象，叶子必须是字符串，
	// 类型不符按缺键处理——否则 GetNamedString 拿到对象会抛 hresult_error，异常穿透
	// DispatcherQueue 回调直接杀进程。
	auto arr = Ling::Util::splitStr(keyPath, L'.');
	if (arr.size() < 2) return keyPath;

	auto readPath = [](JsonObject const& root, const std::vector<std::wstring>& path) -> std::wstring {
		try {
			JsonObject obj = root;
			if (!obj) return {};
			for (size_t i = 0; i + 1 < path.size(); ++i) {
				obj = obj.GetNamedObject(path[i], nullptr);
				if (!obj) return {};
			}
			auto value = obj.GetNamedValue(path.back(), nullptr);
			if (!value || value.ValueType() != JsonValueType::String) return {};
			return std::wstring{ value.GetString() };
		}
		catch (...) {
			// 手改坏的语言文件：类型不符等一律按缺键处理，绝不让异常穿透出去
			return {};
		}
	};

	auto& self = *lang;
	auto text = readPath(self.langObj, arr);
	if (!text.empty()) return text;
	if (!self.fallbackObj) return keyPath;
	text = readPath(self.fallbackObj, arr);
	if (text.empty()) return keyPath;
	return text;
}

void Lang::initLang(const std::wstring& langCode)
{
	auto [pData, size] = Ling::Util::getRes(langCode);
	std::wstring builtinJson;
	if (pData && size > 0)
		builtinJson = Ling::Util::readTextFromBytes(pData, size);
	JsonObject builtinObj{ nullptr };
	if (!builtinJson.empty())
		builtinObj = JsonObject::Parse(builtinJson);

	if (langCode == L"zh-CN" || langCode == L"en-US") {
		langObj = builtinObj;
		return;
	}
	// 第三方语言文件：用户自己放进来的，可能是老版本的（缺新加的键）、也可能手工改坏了。
	// 所以一是用 TryParse（Parse 解析失败直接抛，一路抛出去就是进程没了），
	// 二是把内置的 en-US 留在 fallbackObj 里给 Lang::get 兜底
	fallbackObj = builtinObj;
	bool loaded{ false };
	for (const auto& entry : getLangFiles()) {
		std::wstring filename = entry.filename().wstring();
		if (filename.find(langCode) == std::wstring::npos) continue;
		auto pathStr = entry.wstring();
		std::wstring content = Ling::Util::readFileText(entry);
		JsonObject obj{ nullptr };
		if (JsonObject::TryParse(content, obj)) {
			langObj = obj;
			loaded = true;
		}
		else {
			auto msg = L"lang pare error：" + pathStr + L"\n use English";
			MessageBox(nullptr, msg.data(), L"SnowAir", MB_OK | MB_ICONWARNING);
		}
		break;
	}
	//文件没找到、或者内容不是合法 JSON：整份都用内置的 en-US
	if (!loaded) langObj = fallbackObj;
}

std::vector<std::pair<std::wstring, std::wstring>> Lang::getSupportedLang()
{
	std::vector<std::pair<std::wstring, std::wstring>> result = { {L"简体中文",L"zh-CN"},{L"English",L"en-US"} };
	for (const auto& entry : getLangFiles()) {
		std::wstring filename = entry.filename().wstring();
		auto arr = Ling::Util::splitStr(filename, L'.');
		if (arr.size() == 3 && arr[2] == L"json") {
			result.push_back({ arr[0] ,arr[1] });
		}
	}
	return result;
}
