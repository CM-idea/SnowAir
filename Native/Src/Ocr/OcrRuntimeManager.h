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
	/** 卸载：删除该方案的运行时（含共享 ocr-bin 里的 RapidOCR/ONNX 依赖） */
	void uninstall(OcrPackVariant v);
private:
	OcrRuntimeManager() = default;
};
