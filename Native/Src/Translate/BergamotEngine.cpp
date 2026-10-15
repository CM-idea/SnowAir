#include "pch.h"
#include "BergamotEngine.h"
#include "TranslateLangs.h"
#include "../Ocr/PluginPaths.h"

// 离线引擎：BergamotTranslatorSharp 的动态库（Freeesia/BergamotTranslatorSharp）。
// 它的 C 接口（见其 bergamot-translator-dynamic/include/bergamot.h）是：
//   void* translator_initialize(const char** configPaths, int numPaths);   // 1 个 config 直译；2 个 config 以英语为枢轴接力
//   char* translator_translate(void* translator, const char* text, bool html);
//   void  translator_free(void* translator);
// 注意：translate 返回的缓冲区由引擎用 CoTaskMemAlloc 分配，调用方必须 CoTaskMemFree。
// 语言方向由所选模型（config 里指向的 model/vocab）决定，接口本身不带 from/to。

namespace BergamotEngine {

using BergInit = void* (__cdecl*)(const char** configPaths, int numPaths);
using BergTranslate = char* (__cdecl*)(void* translator, const char* text, bool html);
using BergFree = void (__cdecl*)(void* translator);

bool isAvailable()
{
	return std::filesystem::exists(PluginPaths::bergamotDll());
}

namespace {
	// 语言对目录：models/<pairId>/config.intgemm8bitalpha.yml（由 TranslateModelManager 写出）
	std::filesystem::path pairConfigPath(const std::wstring& pairId)
	{
		return PluginPaths::translateModels() / pairId / L"config.intgemm8bitalpha.yml";
	}
}

TranslateResult translateSync(const TranslateRequest& req)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Offline;

	auto dll = PluginPaths::bergamotDll();
	if (!std::filesystem::exists(dll)) {
		r.error = L"离线翻译未安装，请到「插件集成」下载离线翻译语言包";
		return r;
	}

	// 源/目标 → 语言对（TranslateService 已做过原文检测与自动换向，这里拿到的应是具体语种）
	auto pairs = TranslateLangs::pairIds(req.sourceLang, req.targetLang);
	if (pairs.empty()) {
		r.error = L"离线翻译不支持该语言方向（源与目标相同或无法识别）";
		return r;
	}
	std::vector<std::string> configs;
	configs.reserve(pairs.size());
	for (const auto& pid : pairs) {
		auto cfg = pairConfigPath(pid);
		if (!std::filesystem::is_regular_file(cfg)) {
			r.error = L"离线语言包缺少该方向（" + pid + L"），请在「插件集成」下载对应语言包";
			return r;
		}
		configs.push_back(Ling::Util::convertToStr(cfg.wstring()));
	}

	HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!mod) {
		r.error = L"无法加载 bergamot.dll";
		return r;
	}
	auto init = (BergInit)GetProcAddress(mod, "translator_initialize");
	auto run = (BergTranslate)GetProcAddress(mod, "translator_translate");
	auto freeFn = (BergFree)GetProcAddress(mod, "translator_free");
	if (!init || !run || !freeFn) {
		FreeLibrary(mod);
		r.error = L"bergamot.dll 导出不匹配（需要 translator_initialize / translator_translate / translator_free）";
		return r;
	}

	std::vector<const char*> cpaths;
	cpaths.reserve(configs.size());
	for (auto& c : configs) cpaths.push_back(c.c_str());

	void* handle = init(cpaths.data(), (int)cpaths.size());
	if (!handle) {
		FreeLibrary(mod);
		r.error = L"离线翻译引擎初始化失败（模型或配置不完整）";
		return r;
	}

	std::string text = Ling::Util::convertToStr(req.text);
	char* out = run(handle, text.c_str(), false);
	if (out) {
		int n = MultiByteToWideChar(CP_UTF8, 0, out, -1, nullptr, 0);
		r.text.assign(n > 0 ? n - 1 : 0, 0);
		if (n > 1) MultiByteToWideChar(CP_UTF8, 0, out, -1, r.text.data(), n);
		r.ok = !r.text.empty();
		CoTaskMemFree(out);
	}
	freeFn(handle);
	FreeLibrary(mod);
	if (!r.ok && r.error.empty()) r.error = L"离线翻译无结果";
	return r;
}

} // namespace BergamotEngine
