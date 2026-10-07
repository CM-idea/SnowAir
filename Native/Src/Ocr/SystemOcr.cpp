#include "pch.h"
#include "SystemOcr.h"
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
		const uint32_t nbytes = (uint32_t)w * (uint32_t)h * 4;
		Buffer buf(nbytes);
		buf.Length(nbytes);
		memcpy(buf.data(), bgra, nbytes);

		SoftwareBitmap bmp(BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Premultiplied);
		bmp.CopyFromBuffer(buf);

		auto engine = tryCreateEngine();
		if (!engine) {
			r.error = L"未安装可用的 OCR 语言包（可在系统设置中添加中文）";
			return r;
		}
		auto result = engine.RecognizeAsync(bmp).get();
		if (!result) {
			r.error = L"系统 OCR 识别失败";
			return r;
		}
		std::wstring plain;
		for (auto const& line : result.Lines()) {
			auto text = std::wstring(line.Text());
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
				reg.x = minX;
				reg.y = minY;
				reg.w = std::max(2.f, maxR - minX);
				reg.h = std::max(2.f, maxB - minY);
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
	}
	catch (...) {
		r.error = L"系统 OCR 识别失败";
	}
	return r;
}

} // namespace SystemOcr
