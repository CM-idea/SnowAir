#pragma once
#include "OcrTypes.h"
#include <string>

struct OcrCloudConfig {
	std::wstring id;
	std::wstring provider; // baidu / youdao / custom
	std::wstring name;
	std::wstring appId;
	std::wstring apiKey;
	std::wstring secret;
	std::wstring endpoint;

	std::wstring displayName() const
	{
		if (!name.empty()) return name;
		if (provider == L"baidu") return L"百度识图";
		if (provider == L"youdao") return L"有道识图";
		return L"自定义识图";
	}
	bool isConfigured() const
	{
		if (provider == L"youdao") return !appId.empty() && !secret.empty();
		if (provider == L"custom") return !apiKey.empty() || !endpoint.empty();
		return !apiKey.empty() && !secret.empty();
	}
};

namespace CloudOcr {
	OcrResult recognize(int w, int h, const BYTE* bgra, const OcrCloudConfig& cfg);
}
