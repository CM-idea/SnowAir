#include "pch.h"
#include "SystemOcr.h"
#include "../Util.h"
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>

namespace SystemOcr {

bool isAvailable()
{
	try {
		auto langs = winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages();
		return langs.Size() > 0;
	}
	catch (...) {
		return false;
	}
}

static winrt::Windows::Media::Ocr::OcrEngine tryCreateEngine()
{
	using winrt::Windows::Globalization::Language;
	using winrt::Windows::Media::Ocr::OcrEngine;
	static const wchar_t* tags[] = { L"zh-Hans-CN", L"zh-CN", L"en-US", L"en" };
	for (auto* tag : tags) {
		try {
			Language lang(tag);
			if (OcrEngine::IsLanguageSupported(lang)) {
				auto e = OcrEngine::TryCreateFromLanguage(lang);
				if (e) return e;
			}
		}
		catch (...) {}
	}
	return OcrEngine::TryCreateFromUserProfileLanguages();
}

// 双线性放大 BGRA（top-down、行紧凑）。小图/小字放大后再识别可明显提升 Windows OCR 命中率。
// 实现统一放在 Util（PP-OCR 路径同样要用），这里只做转发。
static std::vector<BYTE> upscaleBGRA(const BYTE* src, int w, int h, int factor)
{
	return Util::scaleBGRA(src, w, h, w * factor, h * factor);
}

// Windows OCR 会在 CJK 字符之间插入空格，显示/复制时应去掉；拉丁文本里的空格有意义，需保留。
static std::wstring tidySpaces(const std::wstring& s)
{
	std::wstring o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		const wchar_t c = s[i];
		if (c == L' ' || c == L'\u3000') {
			const bool prevCjk = !o.empty() && o.back() >= 0x2E80;
			const bool nextCjk = (i + 1 < s.size()) && s[i + 1] >= 0x2E80;
			if (prevCjk && nextCjk) continue;
		}
		o.push_back(c);
	}
	return o;
}

OcrResult recognize(int w, int h, const BYTE* bgra)
{
	using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
	using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
	using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
	using winrt::Windows::Storage::Streams::Buffer;

	::OcrResult r;
	r.engineUsed = OcrEngineKind::System;
	if (w <= 0 || h <= 0 || !bgra) {
		r.error = L"图片为空";
		return r;
	}
	try {
		// 小图（长边 < 1000）先放大 2×：Windows OCR 对过小的字命中率很低；
		// 识别完把框坐标除回去（inv）。
		const int maxDim = std::max(w, h);
		const int factor = (maxDim > 0 && maxDim < 1000 && maxDim * 2 <= 6000) ? 2 : 1;
		std::vector<BYTE> up;
		const BYTE* in = bgra;
		int uw = w, uh = h;
		if (factor == 2) {
			up = upscaleBGRA(bgra, w, h, 2);
			if (!up.empty()) { in = up.data(); uw = w * 2; uh = h * 2; }
		}
		const float inv = (factor == 2 && !up.empty()) ? 0.5f : 1.f;

		const uint32_t nbytes = (uint32_t)uw * (uint32_t)uh * 4;
		Buffer buf(nbytes);
		buf.Length(nbytes);
		memcpy(buf.data(), in, nbytes);

		// AlphaMode::Ignore：抓屏 DIB 的第 4 字节常是未定义的 0，用 Premultiplied 会把颜色乘成黑，
		// 直接导致识别不到内容 —— 这里忽略 alpha，只用 BGR。
		SoftwareBitmap bmp(BitmapPixelFormat::Bgra8, uw, uh, BitmapAlphaMode::Ignore);
		bmp.CopyFromBuffer(buf);

		auto engine = tryCreateEngine();
		if (!engine) {
			r.error = L"未安装可用的 OCR 语言包（可在系统设置中添加中文）";
			r.engineUnavailable = true;
			return r;
		}
		auto result = engine.RecognizeAsync(bmp).get();
		if (!result) {
			r.error = L"系统 OCR 识别失败";
			r.engineUnavailable = true;
			return r;
		}
		std::wstring plain;
		for (auto const& line : result.Lines()) {
			auto text = tidySpaces(std::wstring(line.Text()));
			if (text.empty()) continue;
			float minX = 1e9f, minY = 1e9f, maxR = -1e9f, maxB = -1e9f;
			bool any = false;
			for (auto const& word : line.Words()) {
				auto br = word.BoundingRect();
				minX = std::min(minX, (float)br.X);
				minY = std::min(minY, (float)br.Y);
				maxR = std::max(maxR, (float)(br.X + br.Width));
				maxB = std::max(maxB, (float)(br.Y + br.Height));
				any = true;
			}
			::OcrRegion reg;
			reg.text = text;
			reg.displayText = text;
			if (any) {
				// Windows OCR 的 BoundingRect 紧贴字形，比 PP-OCR 的检测框小一圈；直接拿它铺
				// 背景/定字号会"盖不满原文字、字偏小"。按行高外扩一点点，让两个引擎的叠加观感一致。
				const float bw = maxR - minX;
				const float bh = maxB - minY;
				const float pad = bh * 0.15f;
				reg.x = std::max(0.f, (minX - pad) * inv);
				reg.y = std::max(0.f, (minY - pad) * inv);
				reg.w = std::max(2.f, std::min((bw + pad * 2.f) * inv, (float)w - reg.x));
				reg.h = std::max(2.f, std::min((bh + pad * 2.f) * inv, (float)h - reg.y));
			}
			else {
				reg.x = 0;
				reg.y = (float)r.regions.size() * 22.f;
				reg.w = (float)w;
				reg.h = 20.f;
			}
			r.regions.push_back(reg);
			if (!plain.empty()) plain += L'\n';
			plain += text;
		}
		r.text = plain;
		r.ok = !r.regions.empty() || !r.text.empty();
		if (!r.ok) r.error = L"未识别到文字";
	}
	catch (winrt::hresult_error const& e) {
		r.error = e.message().c_str();
		r.engineUnavailable = true;
	}
	catch (...) {
		r.error = L"系统 OCR 识别失败";
		r.engineUnavailable = true;
	}
	return r;
}

} // namespace SystemOcr
