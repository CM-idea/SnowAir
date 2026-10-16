#pragma once
// OCR 插件安装流程共用的小工具：多镜像下载、隐藏解压、递归找文件。
// 模型包（OcrModelManager）与运行时（OcrRuntimeManager）都从 lw.PPOCR.C 的 GitHub Release
// 取 zip，流程一样，所以抽到一处。只被 .cpp 包含，且调用方先包含 pch.h。
#include "../Net/HttpClient.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace OcrPackFetch {

	// GitHub 直链 + 加速镜像（国内节点常整片挂掉，多挂几个，最后回落到直链）
	inline std::vector<std::wstring> githubMirrors(const std::wstring& url)
	{
		const wchar_t* prefixes[] = {
			L"https://gh-proxy.com/",
			L"https://gh.llkk.cc/",
			L"https://ghfile.geekertao.top/",
			L"https://gh.jasonzeng.dev/",
			L"https://ghfast.top/",
			L"https://ghproxy.net/",
		};
		std::vector<std::wstring> out;
		for (auto p : prefixes) out.push_back(std::wstring(p) + url);
		out.push_back(url);
		return out;
	}

	// 依次尝试各地址，任一成功即返回
	inline bool tryDownload(const std::vector<std::wstring>& urls, const std::filesystem::path& dest,
		std::function<void(float)> progress)
	{
		for (const auto& u : urls) {
			if (SnowHttp::downloadFile(u, dest, [&](uint64_t got, uint64_t total) {
				if (progress && total) progress((float)got / (float)total);
			}))
				return true;
		}
		return false;
	}

	// 隐藏窗口跑一个命令（PowerShell 解压用）
	inline bool runHidden(const std::wstring& cmdLine)
	{
		STARTUPINFOW si{ .cb = sizeof(si) };
		PROCESS_INFORMATION pi{};
		std::wstring cmd = cmdLine;
		if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &si, &pi))
			return false;
		WaitForSingleObject(pi.hProcess, 600000);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return code == 0;
	}

	// 解压 zip 到 dir。注意：Expand-Archive 只认 .zip 扩展名，调用方落盘时别改名
	inline bool extractZip(const std::filesystem::path& zip, const std::filesystem::path& dir)
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		std::wstring cmd = L"powershell -NoProfile -Command \"Expand-Archive -LiteralPath '"
			+ zip.wstring() + L"' -DestinationPath '" + dir.wstring() + L"' -Force\"";
		return runHidden(cmd);
	}

	// 递归找文件名匹配、且路径含 need 的第一个文件（need 为空则不筛选目录）
	inline std::filesystem::path findFile(const std::filesystem::path& root,
		const std::wstring& name, const std::wstring& need)
	{
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
			it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) break;
			if (!it->is_regular_file(ec)) continue;
			if (_wcsicmp(it->path().filename().c_str(), name.c_str()) != 0) continue;
			if (!need.empty() && it->path().wstring().find(need) == std::wstring::npos) continue;
			return it->path();
		}
		return {};
	}

} // namespace OcrPackFetch
