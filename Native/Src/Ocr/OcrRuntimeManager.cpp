#include "pch.h"
#include "OcrRuntimeManager.h"
#include "OcrPackVariant.h"
#include "OcrPackFetch.h"
#include "PluginPaths.h"
#include "LwPpOcrEngine.h"
#include <thread>

namespace {
	// lw.PPOCR.C 原生运行时包（与模型包同一个发布号）
	const std::wstring kNativeUrl =
		L"https://github.com/lxw112190/lw.PPOCR.C/releases/download/v1.2.0/"
		L"lw.PPOCR.C-1.2.0-windows-x64-msvc.zip";

	// 主 DLL 必须装成；其余是它依赖的 VC 运行库（包里自带，取不到就跳过，系统多半也有）。
	// ucrtbase.dll / api-ms-win-* 不在复制之列：Win10 自带，放一份到插件目录反而会和系统那份打架。
	const wchar_t* const kDlls[] = {
		L"lw_ppocr_c.dll",
		L"msvcp140.dll", L"msvcp140_1.dll", L"msvcp140_2.dll",
		L"msvcp140_atomic_wait.dll", L"msvcp140_codecvt_ids.dll",
		L"vcruntime140.dll", L"vcruntime140_1.dll",
	};
}

OcrRuntimeManager& OcrRuntimeManager::instance()
{
	static OcrRuntimeManager inst;
	return inst;
}

void OcrRuntimeManager::uninstall(OcrPackVariant v)
{
	// 先放掉引擎与 DLL，否则文件被占用删不干净
	LwPpOcrEngine::release();
	std::error_code ec;
	std::filesystem::remove_all(PluginPaths::ocrRuntime(v), ec);
	std::filesystem::remove_all(PluginPaths::ocrBin(), ec);   // 老版本遗留的 RapidOCR.dll 目录
}

bool OcrRuntimeManager::isInstalled(OcrPackVariant v) const
{
	std::error_code ec;
	auto dll = PluginPaths::ocrRuntime(v) / L"lw_ppocr_c.dll";
	return std::filesystem::is_regular_file(dll, ec)
		&& std::filesystem::file_size(dll, ec) > 100 * 1024;
}

void OcrRuntimeManager::download(OcrPackVariant v,
	std::function<void(float)> progress,
	std::function<void(bool, std::wstring)> done)
{
	std::thread([v, progress, done]() {
		auto dir = PluginPaths::ocrRuntime(v);
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		if (OcrRuntimeManager::instance().isInstalled(v)) {
			if (progress) progress(1.f);
			if (done) done(true, {});
			return;
		}

		auto tmp = dir / L"_pkg";
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directories(tmp, ec);
		auto zip = tmp / L"lw-ppocr-native.zip";   // Expand-Archive 只认 .zip 扩展名

		if (!OcrPackFetch::tryDownload(OcrPackFetch::githubMirrors(kNativeUrl), zip,
			[&](float p) { if (progress) progress(p * 0.7f); })) {
			std::filesystem::remove_all(tmp, ec);
			if (done) done(false, L"PP-OCRv6 运行时下载失败，请检查网络后重试");
			return;
		}
		auto extract = tmp / L"x";
		if (!OcrPackFetch::extractZip(zip, extract)) {
			std::filesystem::remove_all(tmp, ec);
			if (done) done(false, L"PP-OCRv6 运行时解压失败");
			return;
		}
		if (progress) progress(0.85f);

		bool gotMain = false;
		for (const auto* name : kDlls) {
			auto src = OcrPackFetch::findFile(extract, name, L"");
			if (src.empty()) {
				if (_wcsicmp(name, L"lw_ppocr_c.dll") == 0) {
					std::filesystem::remove_all(tmp, ec);
					if (done) done(false, L"运行时包里没有 lw_ppocr_c.dll");
					return;
				}
				continue;   // 可选运行库：包里没有就算了
			}
			auto dst = dir / name;
			std::filesystem::remove(dst, ec);
			std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
			if (!ec && _wcsicmp(name, L"lw_ppocr_c.dll") == 0) gotMain = true;
		}
		std::filesystem::remove_all(tmp, ec);
		if (!gotMain) {
			if (done) done(false, L"lw_ppocr_c.dll 复制失败");
			return;
		}
		if (progress) progress(1.f);
		if (done) done(true, {});
	}).detach();
}
