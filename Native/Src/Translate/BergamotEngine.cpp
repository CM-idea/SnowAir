#include "pch.h"
#include "BergamotEngine.h"
#include "TranslateLangs.h"
#include "../Ocr/PluginPaths.h"
#include <mutex>

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

	// 进程内缓存"已加载的引擎"。
	// translator_initialize 会把整个模型读进内存（几百 MB），而翻译是**逐行**调用的 —— 每次重新
	// 加载的话，一段 8 行文字就是 8 次加载，慢到像卡死。这里按语言对复用同一份，换方向才重载。
	struct LoadedEngine {
		HMODULE mod{ nullptr };
		BergTranslate run{ nullptr };
		BergFree freeFn{ nullptr };
		void* handle{ nullptr };
		std::wstring key;   // 语言对串，如 "en-zh" 或 "en-ru,ru-zh"
	};
	std::mutex g_mtx;
	LoadedEngine g_engine;

	// 卸载当前引擎（释放模型内存、解除对 bergamot.dll 的文件占用）
	void unloadEngine()
	{
		if (g_engine.freeFn && g_engine.handle) g_engine.freeFn(g_engine.handle);
		if (g_engine.mod) FreeLibrary(g_engine.mod);
		g_engine = LoadedEngine{};
	}

	// 取指定语言对的引擎句柄：命中缓存直接返回；否则（首次 / 换方向）重新加载。
	// 失败返回 nullptr 并写入 err。调用方需持有 g_mtx。
	void* acquireEngine(const std::wstring& key, const std::vector<std::string>& configs, std::wstring& err)
	{
		if (g_engine.handle && g_engine.key == key) return g_engine.handle;
		unloadEngine();   // 换方向：先卸掉旧的

		auto dll = PluginPaths::bergamotDll();
		HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!mod) { err = L"无法加载 bergamot.dll"; return nullptr; }
		auto run = (BergTranslate)GetProcAddress(mod, "translator_translate");
		auto freeFn = (BergFree)GetProcAddress(mod, "translator_free");
		auto init = (BergInit)GetProcAddress(mod, "translator_initialize");
		if (!init || !run || !freeFn) {
			FreeLibrary(mod);
			err = L"bergamot.dll 导出不匹配（需要 translator_initialize / translator_translate / translator_free）";
			return nullptr;
		}
		std::vector<const char*> cpaths;
		cpaths.reserve(configs.size());
		for (auto& c : configs) cpaths.push_back(c.c_str());
		void* handle = init(cpaths.data(), (int)cpaths.size());
		if (!handle) {
			FreeLibrary(mod);
			err = L"离线翻译引擎初始化失败（模型或配置不完整）";
			return nullptr;
		}
		g_engine.mod = mod;
		g_engine.run = run;
		g_engine.freeFn = freeFn;
		g_engine.handle = handle;
		g_engine.key = key;
		return handle;
	}
}

void release()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	unloadEngine();
}

TranslateResult translateSync(const TranslateRequest& req)
{
	TranslateResult r;
	r.providerUsed = TranslateProvider::Offline;

	if (!std::filesystem::exists(PluginPaths::bergamotDll())) {
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
	std::wstring key;
	configs.reserve(pairs.size());
	for (const auto& pid : pairs) {
		auto cfg = pairConfigPath(pid);
		if (!std::filesystem::is_regular_file(cfg)) {
			r.error = L"离线语言包缺少该方向（" + pid + L"），请在「插件集成」下载对应语言包";
			return r;
		}
		configs.push_back(Ling::Util::convertToStr(cfg.wstring()));
		if (!key.empty()) key += L",";
		key += pid;
	}

	// 加锁：两个截图会话各自的后台线程可能并发翻，缓存与句柄不能同时被改
	std::lock_guard<std::mutex> lock(g_mtx);
	std::wstring err;
	void* handle = acquireEngine(key, configs, err);
	if (!handle) {
		r.error = err;
		return r;
	}

	std::string text = Ling::Util::convertToStr(req.text);
	char* out = g_engine.run(handle, text.c_str(), false);
	if (out) {
		int n = MultiByteToWideChar(CP_UTF8, 0, out, -1, nullptr, 0);
		r.text.assign(n > 0 ? n - 1 : 0, 0);
		if (n > 1) MultiByteToWideChar(CP_UTF8, 0, out, -1, r.text.data(), n);
		r.ok = !r.text.empty();
		CoTaskMemFree(out);
	}
	if (!r.ok && r.error.empty()) r.error = L"离线翻译无结果";
	return r;
}

} // namespace BergamotEngine
