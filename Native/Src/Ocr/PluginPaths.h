#pragma once
#include <filesystem>
#include "OcrPackVariant.h"
#include "../Setting.h"

namespace PluginPaths {
	// 解析插件根目录的优先级：
	//   1) config.json 的 common.pluginDir 显式指定（想放哪就放哪）
	//   2) exe 同目录下已有 plugin 文件夹 → 用它（绿色包 / 不希望占用 C 盘）
	//   3) 退回 %AppData%\SnowAir\plugin（旧行为，兼容已在此下载过插件的用户）
	// 第 2 条与 config.json / ImageReader 的定位口径一致（见 Setting::initConfigPath）。
	inline std::filesystem::path root()
	{
		auto* s = Setting::get();
		if (!s) return std::filesystem::path{ L"plugin" };
		auto custom = s->getPluginDir();
		if (!custom.empty()) return std::filesystem::path{ custom };
		wchar_t buf[MAX_PATH]{};
		if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
			auto portable = std::filesystem::path{ buf }.parent_path() / L"plugin";
			std::error_code ec;
			if (std::filesystem::is_directory(portable, ec)) return portable;
		}
		return s->getDataPath() / L"plugin";
	}
	inline std::filesystem::path ocrModels() { return root() / L"ocr-models"; }
	// 运行时（lw_ppocr_c.dll + VC 运行库）与模型档位无关，各档共用一份
	inline std::filesystem::path ocrRuntime(OcrPackVariant) { return root() / L"ocr-runtime" / L"ppocr-v6"; }
	inline std::filesystem::path ocrBin() { return root() / L"ocr-bin"; }
	inline std::filesystem::path translateRoot() { return root() / L"translate"; }
	inline std::filesystem::path bergamotDll() { return translateRoot() / L"bergamot.dll"; }
	inline std::filesystem::path translateModels() { return translateRoot() / L"models"; }
}
