#pragma once
#include "OcrTypes.h"

// PP-OCRv6 离线文字识别，引擎是 lw.PPOCR.C（纯 C 运行时，MIT，见 Ocr/lw_infer.h 与 NOTICE）。
//
// 为什么换掉旧的 RapidOCR.dll：那个 DLL 只有两个一次性函数、没有引擎句柄，实测每次调用
// 都会新建一份引擎（单次约 400MB）且既无释放接口、FreeLibrary 也不归还内存 —— 进程里一旦
// 用过就永久占着几百 MB。这里换成有 create/free 的 C ABI：首次 recognitize 时建一次、
// 之后复用，release() 时 lw_ocr_free + FreeLibrary，实测内存从 103MB 回到 8MB。
//
// 调用时机：截图/贴图会话结束时释放（与离线翻译引擎 BergamotEngine 一样），
// 以及卸载或重装模型之前。
namespace LwPpOcrEngine {
	// 运行时 DLL 与三个模型 + 字典是否齐全（纯文件探测，不加载、不建引擎）
	bool isInstalled();
	// 识别 BGRA（top-down、行紧凑）→ 文本 + 每行框（相对识别图左上角）
	OcrResult recognize(int w, int h, const BYTE* bgra);
	// 释放引擎并卸载 DLL，把识别占的内存还回去（未创建时是安全空操作）
	void release();
}
