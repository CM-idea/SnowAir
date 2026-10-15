#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <filesystem>

// 同步 WinRT Http 封装（OCR/翻译/插件下载）；勿命名 HttpClient 以免与 winrt 冲突
namespace SnowHttp {
	struct Response {
		bool ok{ false };
		int status{ 0 };
		std::string body;
		std::wstring error;
	};

	Response get(const std::wstring& url,
		const std::map<std::wstring, std::wstring>& headers = {});
	Response post(const std::wstring& url, const std::string& bodyUtf8,
		const std::wstring& contentType = L"application/x-www-form-urlencoded",
		const std::map<std::wstring, std::wstring>& headers = {});
	// 下载到本地文件；内含读超时（20s 无数据即判失败）与自动重试（默认 3 次，失败之间退避）
	bool downloadFile(const std::wstring& url, const std::filesystem::path& dest,
		std::function<void(uint64_t got, uint64_t total)> onProgress = nullptr,
		int attempts = 3);
}
