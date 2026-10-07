#pragma once
#include "OcrPackVariant.h"
#include <functional>
#include <string>

class OcrModelManager {
public:
	static OcrModelManager& instance();
	bool isInstalled(OcrPackVariant v) const;
	/** 后台下载；progress 0~1；完成回调在调用线程需自行切回 UI */
	void download(OcrPackVariant v,
		std::function<void(float)> progress,
		std::function<void(bool ok, std::wstring err)> done);
	/** 卸载：删除该方案的模型目录（已装/半截都清掉） */
	void uninstall(OcrPackVariant v);
	std::wstring modelsDir(OcrPackVariant v) const;
private:
	OcrModelManager() = default;
};
