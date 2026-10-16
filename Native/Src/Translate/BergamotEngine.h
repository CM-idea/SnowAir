#pragma once
#include "TranslateTypes.h"

namespace BergamotEngine {
	bool isAvailable();
	TranslateResult translateSync(const TranslateRequest& req);
	// 卸载常驻的离线引擎（释放模型内存、解除对 bergamot.dll 的占用）。
	// 调用时机：装/卸语言包前必须调用（否则覆盖或删除 DLL 会被"文件占用"挡住）；
	// 另在每次截图会话结束时调用（模型几百 MB，会话一关就没必要继续常驻）。
	void release();
}
