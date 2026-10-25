#include "pch.h"
#include "OcrModelManager.h"
#include "OcrPackVariant.h"
#include "OcrPackFetch.h"
#include "PluginPaths.h"
#include "LwPpOcrEngine.h"
#include <fstream>
#include <thread>

namespace {
	// lw.PPOCR.C 官方模型包（det + cls + rec + 字典），按档位取不同 zip。
	// 固定到发布号，避免与运行时版本串味（其文档明确要求"一个发布号的整套一起用"）。
	const std::wstring kPackBase =
		L"https://github.com/lxw112190/lw.PPOCR.C/releases/download/v1.2.0/";

	// 装好后模型目录里应有的文件（从包内摊平到 ocr-models/<档位目录>/）
	const wchar_t* const kFiles[] = { L"det.lwm", L"cls.lwm", L"rec.lwm", L"ppocr_keys.txt" };

	// 模型档位标记。换了档位后老安装必须重新下载：
	// 只补缺文件会留下 A 档的 det 配 B 档的 rec/字典，类别数对不上，引擎直接初始化失败。
	const wchar_t* const kPackMarkerName = L"pack.id";
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
	// 档位标记：别的档位装过（或旧版本没这个文件）→ 视为未安装，引导重新下载对应档位
	std::ifstream marker(dir / kPackMarkerName, std::ios::binary);
	std::string value;
	std::getline(marker, value);
	while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
		value.pop_back();
	return value == OcrPack::markerValue(v);
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

		// 覆盖安装前先放掉引擎：老模型正被读着时删/换文件会失败
		LwPpOcrEngine::release();

		// 下载官方模型包 → 解压 → 把 4 个文件摊到模型目录
		auto tmp = destRoot / L"_pkg";
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directories(tmp, ec);
		auto zip = tmp / OcrPack::packFile(v);   // Expand-Archive 只认 .zip 扩展名

		if (!OcrPackFetch::tryDownload(OcrPackFetch::githubMirrors(kPackBase + OcrPack::packFile(v)), zip,
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

		// 四个文件必须整体覆盖：混档（tiny 的 det + small 的 rec/字典）会让类别数对不上
		for (const auto* name : kFiles) {
			auto dst = destRoot / name;
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

		// 标记最后写：中途失败时保持"未安装"，下次还能重试
		{
			std::ofstream marker(destRoot / kPackMarkerName, std::ios::binary | std::ios::trunc);
			marker << OcrPack::markerValue(v);
			if (!marker) {
				if (done) done(false, L"模型包标记写入失败");
				return;
			}
		}

		if (progress) progress(1.f);
		if (done) done(true, {});
	}).detach();
}
