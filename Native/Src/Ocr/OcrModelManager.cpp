#include "pch.h"
#include "OcrModelManager.h"
#include "OcrPackVariant.h"
#include "OcrPackFetch.h"
#include "PluginPaths.h"
#include "LwPpOcrEngine.h"
#include <thread>

namespace {
	// lw.PPOCR.C 官方模型包（PP-OCRv6 Tiny：det + cls + rec + 字典）。
	// 固定到发布号，避免与运行时版本串味（其文档明确要求"一个发布号的整套一起用"）。
	const std::wstring kTinyPackUrl =
		L"https://github.com/lxw112190/lw.PPOCR.C/releases/download/v1.2.0/"
		L"lw.PPOCR.C-1.2.0-ppocrv6-tiny-runtime.zip";

	// 装好后模型目录里应有的文件（从包内摊平到 ocr-models/ppocr-v6/）
	const wchar_t* const kFiles[] = { L"det.lwm", L"cls.lwm", L"rec.lwm", L"ppocr_keys.txt" };
}

OcrModelManager& OcrModelManager::instance()
{
	static OcrModelManager inst;
	return inst;
}

std::wstring OcrModelManager::modelsDir(OcrPackVariant v) const
{
	return (PluginPaths::ocrModels() / OcrPack::folderName(v)).wstring();
}

void OcrModelManager::uninstall(OcrPackVariant v)
{
	// 引擎读着模型文件，先把它（连同 DLL）放掉再删，否则删除会失败/半截
	LwPpOcrEngine::release();
	std::error_code ec;
	std::filesystem::remove_all(PluginPaths::ocrModels() / OcrPack::folderName(v), ec);
}

bool OcrModelManager::isInstalled(OcrPackVariant v) const
{
	auto dir = PluginPaths::ocrModels() / OcrPack::folderName(v);
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) return false;
	// 清单里的文件齐了才算装好（半截下载不该显示「已安装」）
	for (const auto* name : kFiles) {
		auto p = dir / name;
		if (!std::filesystem::is_regular_file(p, ec)) return false;
		if (std::filesystem::file_size(p, ec) == 0) return false;
	}
	return true;
}

void OcrModelManager::download(OcrPackVariant v,
	std::function<void(float)> progress,
	std::function<void(bool, std::wstring)> done)
{
	std::thread([v, progress, done]() {
		auto destRoot = PluginPaths::ocrModels() / OcrPack::folderName(v);
		std::error_code ec;
		std::filesystem::create_directories(destRoot, ec);

		if (OcrModelManager::instance().isInstalled(v)) {
			if (progress) progress(1.f);
			if (done) done(true, {});
			return;
		}

		// 下载官方模型包 → 解压 → 把 4 个文件摊到模型目录（不重下已存在的单个文件）
		auto tmp = destRoot / L"_pkg";
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directories(tmp, ec);
		auto zip = tmp / L"ppocrv6-tiny-runtime.zip";   // Expand-Archive 只认 .zip 扩展名

		if (!OcrPackFetch::tryDownload(OcrPackFetch::githubMirrors(kTinyPackUrl), zip,
			[&](float p) { if (progress) progress(p * 0.7f); })) {
			std::filesystem::remove_all(tmp, ec);
			if (done) done(false, L"PP-OCRv6 模型包下载失败，请检查网络后重试");
			return;
		}
		auto extract = tmp / L"x";
		if (!OcrPackFetch::extractZip(zip, extract)) {
			std::filesystem::remove_all(tmp, ec);
			if (done) done(false, L"PP-OCRv6 模型包解压失败");
			return;
		}
		if (progress) progress(0.85f);

		for (const auto* name : kFiles) {
			auto dst = destRoot / name;
			if (std::filesystem::is_regular_file(dst, ec) && std::filesystem::file_size(dst, ec) > 0)
				continue;   // 已有且非空：跳过
			auto src = OcrPackFetch::findFile(extract, name, L"");
			if (src.empty()) {
				std::filesystem::remove_all(tmp, ec);
				if (done) done(false, std::wstring(L"模型包里缺少 ") + name);
				return;
			}
			std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				std::filesystem::remove_all(tmp, ec);
				if (done) done(false, std::wstring(L"模型文件写入失败：") + name);
				return;
			}
		}
		std::filesystem::remove_all(tmp, ec);
		if (progress) progress(1.f);
		if (done) done(true, {});
	}).detach();
}
