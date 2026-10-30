#include "pch.h"
#include "OcrService.h"
#include "SystemOcr.h"
#include "LwPpOcrEngine.h"
#include "../Setting.h"

#include <vector>

OcrService& OcrService::instance()
{
	static OcrService inst;
	return inst;
}

std::wstring OcrService::engineDisplayName(OcrEngineKind kind)
{
	switch (kind) {
	case OcrEngineKind::PpOcrV6: return L"PP-OCRv6";
	case OcrEngineKind::System: return L"系统 OCR";
	}
	return L"OCR";
}

namespace {
	// 设置里存的是「偏好」，实际跑哪个引擎由可用性决定：
	//   · PP-OCRv6 要另下组件（运行时 + 模型），不可用就不进候选；
	//   · 系统 OCR 是 Windows 自带的，始终进候选（真缺语言包时它自己的报错最准）。
	// 所以偏好 PP-OCRv6、但用户没下载离线组件时，候选里只剩系统 OCR —— 直接走系统识别，
	// 而不是先试一遍 PP-OCRv6、再把「运行时未安装」当成识别结果抛给用户。
	std::vector<OcrEngineKind> engineOrder(OcrEngineKind prefer, bool ppOcrReady)
	{
		std::vector<OcrEngineKind> order;
		const bool systemFirst = (prefer == OcrEngineKind::System);
		if (systemFirst) order.push_back(OcrEngineKind::System);
		if (ppOcrReady) order.push_back(OcrEngineKind::PpOcrV6);
		if (!systemFirst) order.push_back(OcrEngineKind::System);
		return order;
	}

	OcrResult runEngine(OcrEngineKind kind, int w, int h, const BYTE* bgra)
	{
		return kind == OcrEngineKind::PpOcrV6
			? LwPpOcrEngine::recognize(w, h, bgra)
			: SystemOcr::recognize(w, h, bgra);
	}
}

// 候选顺序由「偏好 + 可用性」决定（见 engineOrder），按顺序跑：
//   · 成功就用它的结果；
//   · 引擎跑通了但没认出字 —— 这是结论，不换引擎（换过去只会拿另一台的误检/空白当答案，
//     同一张图的结果反而时好时坏）；
//   · 引擎自己起不来（文件损坏 / 初始化失败，见 OcrResult::engineUnavailable）才换下一个。
// 全都失败时报**首选**引擎的原因：那才是用户真正需要知道的那条。
//
// 旧配置里残留的 1/2/3（v5 / v4 / Tesseract，均已下架）和 5（云识别，Lite 版已移除）
// 以及任何非法值都落进 else 分支，按 PP-OCRv6 偏好处理。
OcrResult OcrService::recognize(int w, int h, const BYTE* bgra)
{
	const auto prefer = static_cast<OcrEngineKind>(Setting::get()->getOcrEngine());
	const std::vector<OcrEngineKind> order = engineOrder(prefer, LwPpOcrEngine::isInstalled());

	OcrResult firstFailure;
	for (size_t i = 0; i < order.size(); ++i) {
		OcrResult got = runEngine(order[i], w, h, bgra);
		if (got.ok) {
			got.fellBack = (i > 0);   // 不是首选引擎给的结果
			return got;
		}
		if (!got.engineUnavailable) return got;
		if (i == 0) firstFailure = got;
	}
	return firstFailure;
}
