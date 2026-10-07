#include "pch.h"
#include "OcrRuntimeManager.h"
#include "PluginPaths.h"
#include "../Net/HttpClient.h"
#include <thread>
#include <vector>

namespace {
	// RapidOCR.dll 只有 lwtw123456/RapidOCR-dll 这个第三方仓库有（RapidAI 官方 release 里没有）
	const std::wstring kRapidDllUrl =
		L"https://github.com/lwtw123456/RapidOCR-dll/releases/download/v1.3.0/RapidOCR.dll";

	// GitHub 直链 + 加速镜像（国内节点常整片挂掉，多挂几个，最后回落到直链）
	std::vector<std::wstring> githubMirrors(const std::wstring& url)
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
	bool tryDownload(const std::vector<std::wstring>& urls, const std::filesystem::path& dest,
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
	bool runHidden(const std::wstring& cmdLine)
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

	// 递归找文件名匹配、且路径含 need 的第一个文件（need 为空则不筛选目录）
	std::filesystem::path findFile(const std::filesystem::path& root,
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

	// 下载并安装 ONNX Runtime（RapidOCR.dll 依赖同目录的 onnxruntime.dll）
	bool installOnnxRuntime(const std::filesystem::path& dir, std::function<void(float)> progress)
	{
		std::error_code ec;
		auto tmp = dir / L"_ort";
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directories(tmp, ec);

		// 优先 NuGet 包（体积小），失败退回 GitHub 官方 zip
		const std::wstring nuget = L"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.24.4/microsoft.ml.onnxruntime.1.24.4.nupkg";
		const std::wstring ghZip = L"https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-win-x64-1.24.4.zip";
		std::filesystem::path archive = tmp / L"ort.nupkg";
		bool ok = tryDownload({ nuget, std::wstring(L"https://ghfast.top/") + nuget }, archive,
			[&](float p) { if (progress) progress(p * 0.6f); });
		if (!ok || !std::filesystem::is_regular_file(archive, ec)) {
			archive = tmp / L"ort.zip";
			if (!tryDownload(githubMirrors(ghZip), archive,
				[&](float p) { if (progress) progress(p * 0.6f); }))
				return false;
		}

		auto extract = tmp / L"x";
		std::filesystem::create_directories(extract, ec);
		std::wstring cmd = L"powershell -NoProfile -Command \"Expand-Archive -LiteralPath '"
			+ archive.wstring() + L"' -DestinationPath '" + extract.wstring() + L"' -Force\"";
		if (!runHidden(cmd)) return false;
		if (progress) progress(0.8f);

		bool gotMain = false;
		for (const wchar_t* name : { L"onnxruntime.dll", L"onnxruntime_providers_shared.dll" }) {
			auto src = findFile(extract, name, L"win-x64");
			if (src.empty()) src = findFile(extract, name, L"");   // 兜底：不限目录
			if (src.empty()) continue;
			auto dst = dir / name;
			std::filesystem::remove(dst, ec);
			std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
			if (!ec && _wcsicmp(name, L"onnxruntime.dll") == 0) gotMain = true;
		}
		std::filesystem::remove_all(tmp, ec);
		return gotMain;
	}
}

OcrRuntimeManager& OcrRuntimeManager::instance()
{
	static OcrRuntimeManager inst;
	return inst;
}

void OcrRuntimeManager::uninstall(OcrPackVariant v)
{
	std::error_code ec;
	std::filesystem::remove_all(PluginPaths::ocrRuntime(v), ec);
	if (v != OcrPackVariant::Official) {
		// Embedded 的 isInstalled 还认共享 ocr-bin 里的 RapidOCR.dll，一并清掉
		std::filesystem::remove_all(PluginPaths::ocrBin(), ec);
	}
}

bool OcrRuntimeManager::isInstalled(OcrPackVariant v) const
{
	auto dir = PluginPaths::ocrRuntime(v);
	if (v == OcrPackVariant::Official)
		return std::filesystem::exists(dir / L"python.exe");
	std::error_code ec;
	auto dll = dir / L"RapidOCR.dll";
	if (std::filesystem::is_regular_file(dll, ec) && std::filesystem::file_size(dll, ec) > 1024 * 1024)
		return true;
	return std::filesystem::exists(PluginPaths::ocrBin() / L"RapidOCR.dll");
}

void OcrRuntimeManager::download(OcrPackVariant v,
	std::function<void(float)> progress,
	std::function<void(bool, std::wstring)> done)
{
	std::thread([v, progress, done]() {
		auto dir = PluginPaths::ocrRuntime(v);
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (v == OcrPackVariant::Official) {
			// PP-OCRv6 走便携 Python + pip 安装，原生版暂未实装
			if (done) done(false, L"PP-OCRv6 运行时暂未实装，请在「功能设置」改选 PP-OCRv5 后重试");
			return;
		}

		// 1) RapidOCR.dll（进度前一半）
		auto dll = dir / L"RapidOCR.dll";
		const bool needDll = !std::filesystem::is_regular_file(dll, ec)
			|| std::filesystem::file_size(dll, ec) < 1024 * 1024;
		if (needDll) {
			if (!tryDownload(githubMirrors(kRapidDllUrl), dll,
				[&](float p) { if (progress) progress(p * 0.5f); })) {
				std::filesystem::remove(dll, ec);
				if (done) done(false, L"RapidOCR.dll 下载失败，请检查网络后重试");
				return;
			}
			if (progress) progress(0.5f);
		}

		// 2) ONNX Runtime 1.24.4（进度后一半）
		if (!std::filesystem::is_regular_file(dir / L"onnxruntime.dll", ec)) {
			if (!installOnnxRuntime(dir, [&](float p) { if (progress) progress(0.5f + p * 0.5f); })) {
				if (done) done(false, L"ONNX Runtime 下载或解压失败，请检查网络后重试");
				return;
			}
		}
		if (progress) progress(1.f);
		if (done) done(true, {});
	}).detach();
}
