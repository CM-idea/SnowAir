#include "pch.h"
#include "WebpCodec.h"
#include <algorithm>
#include <webp/encode.h>

namespace {
	bool Encode(const uint8_t* data, int w, int h, int stride, bool bgra,
		float quality, bool lossless, int method, std::vector<uint8_t>& out)
	{
		out.clear();
		if (!data || w <= 0 || h <= 0 || stride <= 0) return false;

		WebPConfig config;
		if (!WebPConfigInit(&config)) return false;
		config.quality = std::clamp(quality, 0.f, 100.f);
		config.lossless = lossless ? 1 : 0;
		config.method = std::clamp(method, 0, 6);
		config.thread_level = 0;   // 编译时已关线程，这里保持一致
		if (!WebPValidateConfig(&config)) return false;

		WebPPicture pic;
		if (!WebPPictureInit(&pic)) return false;
		// 无损编码需要 ARGB；有损直接导入到 YUVA，省一次 ARGB 分配和转换
		pic.use_argb = lossless ? 1 : 0;
		pic.width = w;
		pic.height = h;
		const bool imported = bgra
			? WebPPictureImportBGRA(&pic, data, stride) != 0
			: WebPPictureImportRGB(&pic, data, stride) != 0;
		if (!imported) {
			WebPPictureFree(&pic);
			return false;
		}

		WebPMemoryWriter writer;
		WebPMemoryWriterInit(&writer);
		pic.writer = WebPMemoryWrite;
		pic.custom_ptr = &writer;

		const bool ok = WebPEncode(&config, &pic) != 0 && writer.size > 0;
		if (ok) out.assign(writer.mem, writer.mem + writer.size);

		WebPMemoryWriterClear(&writer);
		WebPPictureFree(&pic);
		return ok;
	}
}

bool WebpCodec::EncodeBGRA(const uint8_t* bgra, int w, int h, int stride,
	float quality, bool lossless, std::vector<uint8_t>& out)
{
	return Encode(bgra, w, h, stride, true, quality, lossless, 4, out);
}

bool WebpCodec::EncodeRGB(const uint8_t* rgb, int w, int h, int stride,
	float quality, int method, std::vector<uint8_t>& out)
{
	return Encode(rgb, w, h, stride, false, quality, false, method, out);
}
