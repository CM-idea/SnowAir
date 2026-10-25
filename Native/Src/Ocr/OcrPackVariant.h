#pragma once
#include <string>

// 离线文字识别的模型档位（同一套引擎 lw.PPOCR.C，只是模型体积不同）。
// 历史上这里还有 PP-OCRv5/v4 三档（RapidOCR.dll + onnxruntime），已整体移除：
// 那个 DLL 没有引擎句柄/释放接口，实测内存既不回收、每次调用还继续涨。
// 现在统一走 lw.PPOCR.C：create 一次常驻复用，lw_ocr_free 能把内存真正还回去。
//
// 档位只影响模型目录，运行时 DLL 三档共用一份（见 PluginPaths::ocrRuntime）。
enum class OcrPackVariant {
	Tiny = 0,    // 轻量：det 1.8MB + rec 4.5MB，最快；字典 6901 条、不含假名（日文只能认汉字）
	Small = 1,   // 标准：det 9.9MB + rec 21.1MB，含 180 个假名，中英日都能认
};

namespace OcrPack {
	inline constexpr int kCount = 2;

	inline OcrPackVariant clampVariant(int v)
	{
		return v == 1 ? OcrPackVariant::Small : OcrPackVariant::Tiny;
	}

	// 旧配置里的 engine id → 档位。老用户装的是 small 包，映射到 Small。
	inline OcrPackVariant fromEngineId(int) { return OcrPackVariant::Small; }

	// 目录名：Small 沿用老目录 ppocr-v6（已装用户不用重下），Tiny 单独一个目录
	inline const wchar_t* folderName(OcrPackVariant v)
	{
		return v == OcrPackVariant::Small ? L"ppocr-v6" : L"ppocr-v6-tiny";
	}

	// 设置里显示的名字（带档位）
	inline std::wstring label(OcrPackVariant v)
	{
		return v == OcrPackVariant::Small ? L"PP-OCRv6 标准" : L"PP-OCRv6 轻量";
	}

	// 只显示档位（已有"PP-OCRv6"前缀的地方用）
	inline std::wstring tierLabel(OcrPackVariant v)
	{
		return v == OcrPackVariant::Small ? L"标准" : L"轻量";
	}

	// 安装标记：档位 + 发布号。手册要求"同一发布号的整套模型一起用"，混档会初始化失败。
	inline const char* markerValue(OcrPackVariant v)
	{
		return v == OcrPackVariant::Small ? "ppocrv6-small-1.2.0" : "ppocrv6-tiny-1.2.0";
	}

	inline const wchar_t* packFile(OcrPackVariant v)
	{
		return v == OcrPackVariant::Small
			? L"lw.PPOCR.C-1.2.0-ppocrv6-small-runtime.zip"
			: L"lw.PPOCR.C-1.2.0-ppocrv6-tiny-runtime.zip";
	}

	// 介绍用：大致下载体积（MB，实测 zip：轻量 6.9MB / 标准 30.6MB）
	inline int approxMB(OcrPackVariant v) { return v == OcrPackVariant::Small ? 31 : 7; }

	// 只有标准档的字典含假名，能识别日文
	inline bool supportsJapanese(OcrPackVariant v) { return v == OcrPackVariant::Small; }
}
