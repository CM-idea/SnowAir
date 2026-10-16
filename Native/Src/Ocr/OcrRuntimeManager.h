#pragma once
#include "OcrPackVariant.h"
#include <functional>
#include <string>

class OcrRuntimeManager {
public:
	static OcrRuntimeManager& instance();
	bool isInstalled(OcrPackVariant v) const;
	void download(OcrPackVariant v,
		std::function<void(float)> progress,
		std::function<void(bool ok, std::wstring err)> done);
	/** 卸载：删除 PP-OCRv6 运行时目录（含老版本遗留的 RapidOCR/ONNX 依赖目录） */
	void uninstall(OcrPackVariant v);
private:
	OcrRuntimeManager() = default;
};
