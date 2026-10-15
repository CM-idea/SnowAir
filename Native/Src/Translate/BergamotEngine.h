#pragma once
#include "TranslateTypes.h"

namespace BergamotEngine {
	bool isAvailable();
	TranslateResult translateSync(const TranslateRequest& req);
	// 卸载常驻的离线引擎（释放模型内存、解除对 bergamot.dll 的占用）。
	// 装/卸语言包前必须调用，否则覆盖或删除 DLL 会被"文件占用"挡住。
	void release();
}
