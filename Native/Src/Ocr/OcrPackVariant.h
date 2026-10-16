#pragma once
#include <string>

// 离线文字识别的方案标识。
// 历史上这里有 PP-OCRv6/v5/v4 三档（v4/v5 走 RapidOCR.dll + onnxruntime），v5/v4 已整体移除：
// 那个 DLL 没有引擎句柄/释放接口，实测「LoadLibrary + 调用 + FreeLibrary」既不回收内存、
// 每次调用还继续涨（单次 400MB 量级），留在进程里就是白占几百 MB。
// 现在统一走 lw.PPOCR.C（纯 C 运行时，见 LwPpOcrEngine）：create 一次常驻复用，结束时
// lw_ocr_free 能把内存真正还回去。
enum class OcrPackVariant {
	Official = 0,   // PP-OCRv6（lw.PPOCR.C）
};

namespace OcrPack {
	inline OcrPackVariant clampVariant(int) { return OcrPackVariant::Official; }

	// 旧配置里选过 1(v5)/2(v4)/5(其它) 的引擎：一律归到 v6。
	// 注意 4 = 系统 OCR、5 = 云识别由 OcrEngineKind 解释，这里只处理"PP-OCR 系"的映射。
	inline OcrPackVariant fromEngineId(int) { return OcrPackVariant::Official; }

	inline const wchar_t* label(OcrPackVariant) { return L"PP-OCRv6"; }
	inline const wchar_t* folderName(OcrPackVariant) { return L"ppocr-v6"; }
}
