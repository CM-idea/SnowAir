#include "pch.h"
#include "BergamotEngine.h"
#include "../Ocr/PluginPaths.h"

namespace BergamotEngine {

using BergCreate = void* (__cdecl*)(const char* modelsDir);
using BergFree = void(__cdecl*)(void*);
using BergTranslate = const char* (__cdecl*)(void*, const char* src, const char* from, const char* to);

bool isAvailable()
{
	return std::filesystem::exists(PluginPaths::bergamotDll());
}

TranslateResult translateSync(const TranslateRequest& req)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Offline;
	auto dll = PluginPaths::bergamotDll();
	if (!std::filesystem::exists(dll)) {
		r.error = L"离线翻译未安装，请到「插件」页下载 Bergamot";
		return r;
	}
	HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!mod) {
		r.error = L"无法加载 bergamot.dll";
		return r;
	}
	auto create = (BergCreate)GetProcAddress(mod, "bergamot_create");
	auto freeFn = (BergFree)GetProcAddress(mod, "bergamot_free");
	auto run = (BergTranslate)GetProcAddress(mod, "bergamot_translate");
	if (!create || !freeFn || !run) {
		FreeLibrary(mod);
		r.error = L"bergamot.dll 导出不匹配";
		return r;
	}
	std::string models = Ling::Util::convertToStr(PluginPaths::translateModels().wstring());
	std::string text = Ling::Util::convertToStr(req.text);
	std::string from = Ling::Util::convertToStr(req.sourceLang);
	std::string to = Ling::Util::convertToStr(req.targetLang);
	void* h = create(models.c_str());
	const char* out = run(h, text.c_str(), from.c_str(), to.c_str());
	if (out) {
		int n = MultiByteToWideChar(CP_UTF8, 0, out, -1, nullptr, 0);
		r.text.assign(n > 0 ? n - 1 : 0, 0);
		if (n > 1) MultiByteToWideChar(CP_UTF8, 0, out, -1, r.text.data(), n);
		r.ok = !r.text.empty();
	}
	freeFn(h);
	FreeLibrary(mod);
	if (!r.ok && r.error.empty()) r.error = L"离线翻译无结果";
	return r;
}

} // namespace BergamotEngine
