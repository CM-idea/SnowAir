#pragma once
#include <string>
#include <vector>

enum class OcrEngineKind {
	PpOcrV6 = 0,
	// 1 原为 PP-OCRv5、2 原为 PP-OCRv4（RapidOCR.dll）、3 原为 Tesseract：都已移除。
	// 空出来不用，避免旧配置里的数值错位。
	System = 4,
	// 5 原为云识别（API 配置）：Lite 版不提供，保留数值只为兼容旧配置（运行时按 PP-OCRv6 处理）。
	Cloud = 5,
};

struct OcrRegion {
	std::wstring text;
	std::wstring displayText;
	float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 }; // 相对识别图左上角
	// 竖排（日文/中文竖写）：检测框高明显大于宽时置位。叠加层需逐字竖排，
	// 否则横排文字在 54px 宽的窄框内会被压缩成一条线。
	bool vertical{ false };
	// 倾斜文本（书页拍照、屏幕旋转等）：检测框不是轴对齐矩形时置位。
	// quad 是检测框四点（相对识别图左上角，顺序：左上→右上→右下→左下，即沿文字方向）。
	// 横排正放时四点就是轴对齐矩形，rotated 保持 false，走原来的轴对齐排版。
	// 叠加层用它将背景块和文字一起旋转回原角度，实现斜排文字的原位覆盖。
	bool rotated{ false };
	float quadX[4]{}, quadY[4]{};
	float angle{ 0.f }; // 文字方向与水平线的夹角（弧度，屏幕坐标：正值 = 向右下倾斜）
	// 大字号粗体估测：重绘识别结果时用（默认 OCR / 翻译）；小字不猜（置 false）。
	bool bold{ false };
	// 段落号：OCR 给出来的是"视觉行"，同一段的折行会被切成多行（续行还带缩进）。
	// 翻译按**段**下发与排版（见 ocrResultMergedToLines）；OCR 重绘仍按行用，不受影响。
	int para{ 0 };
	// 段首是不是"圆点条目"（• · ▪ …）。翻译时圆点不交给引擎：不同条目被保留/丢掉
	// 不一致，译文左端就会参差；统一在排版时由程序补。
	bool bullet{ false };
};

struct OcrResult {
	bool ok{ false };
	std::wstring text;
	OcrEngineKind engineUsed{ OcrEngineKind::System };
	std::wstring error;
	bool fellBack{ false };
	// 引擎自己起不来（文件损坏 / 初始化失败 / 推理报错），而不是「跑通了但没认出字」。
	// 只有前者才该换下一个引擎；后者是结论，换引擎只会让同一张图的结果时好时坏。
	bool engineUnavailable{ false };
	std::vector<OcrRegion> regions;
};
