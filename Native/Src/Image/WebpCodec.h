#pragma once
#include <cstdint>
#include <vector>

// 内置 WebP 编码（libwebp，encoder-only）。
// 静态图与动图都走这里，不再依赖系统 WIC 的 WebP 编码器。
namespace WebpCodec {
	// BGRA（top-down，stride 字节/行）→ WebP。
	// quality 0~100；lossless=true 时忽略 quality，用无损压缩。
	bool EncodeBGRA(const uint8_t* bgra, int w, int h, int stride,
		float quality, bool lossless, std::vector<uint8_t>& out);

	// RGB（top-down，stride 字节/行）→ WebP（动图逐帧用）。
	// method 0~6：越小越快（动图建议 0~3）。
	bool EncodeRGB(const uint8_t* rgb, int w, int h, int stride,
		float quality, int method, std::vector<uint8_t>& out);
}
