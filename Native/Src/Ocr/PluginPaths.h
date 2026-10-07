#pragma once
#include <filesystem>
#include "OcrPackVariant.h"
#include "../Setting.h"

namespace PluginPaths {
	inline std::filesystem::path root()
	{
		return Setting::get()->getDataPath() / L"plugin";
	}
	inline std::filesystem::path ocrModels() { return root() / L"ocr-models"; }
	inline std::filesystem::path ocrRuntime(OcrPackVariant v)
	{
		return root() / L"ocr-runtime" / OcrPack::folderName(v);
	}
	inline std::filesystem::path ocrBin() { return root() / L"ocr-bin"; }
	inline std::filesystem::path translateRoot() { return root() / L"translate"; }
	inline std::filesystem::path bergamotDll() { return translateRoot() / L"bergamot.dll"; }
	inline std::filesystem::path translateModels() { return translateRoot() / L"models"; }
}
