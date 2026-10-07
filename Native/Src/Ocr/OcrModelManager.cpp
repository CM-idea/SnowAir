#include "pch.h"
#include "OcrModelManager.h"
#include "PluginPaths.h"
#include "../Net/HttpClient.h"
#include <thread>
#include <vector>

namespace {
	// 单个模型文件：落盘名 + 下载地址
	struct OcrFile {
		const wchar_t* name;
		const wchar_t* url;
	};

	// PP-OCRv5 mobile（RapidOCR.dll v1.3.0 只注册了 mobile 那套算子/版本，server 权重会加载失败）
	const std::vector<OcrFile> kV5Files = {
		{ L"ch_PP-OCRv5_det_mobile.onnx",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_mobile.onnx" },
		{ L"ch_PP-OCRv5_rec_mobile.onnx",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile.onnx" },
		{ L"ch_PP-LCNet_x0_25_textline_ori_cls_mobile.onnx",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv5/cls/ch_PP-LCNet_x0_25_textline_ori_cls_mobile.onnx" },
	};

	// PP-OCRv6 small（官方便携 Python 方案使用；无 cls，方向分类不跑）
	const std::vector<OcrFile> kV6Files = {
		{ L"PP-OCRv6_det_small.onnx",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx" },
		{ L"PP-OCRv6_rec_small.onnx",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx" },
		{ L"ppocrv6_dict.txt",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/paddle/PP-OCRv6/rec/PP-OCRv6_rec_small/ppocrv6_dict.txt" },
	};

	// 每个方案需要的文件清单。PP-OCRv4 已下架（2 号方案被删除并归一到 v5），
	// 这里沿用 v5 的 mobile 权重，保证老设置里选到 v4 也能装出一份可用的模型。
	const std::vector<OcrFile>& filesFor(OcrPackVariant v)
	{
		return (v == OcrPackVariant::Official) ? kV6Files : kV5Files;
	}
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
	std::error_code ec;
	std::filesystem::remove_all(PluginPaths::ocrModels() / OcrPack::folderName(v), ec);
}

bool OcrModelManager::isInstalled(OcrPackVariant v) const
{
	auto dir = PluginPaths::ocrModels() / OcrPack::folderName(v);
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) return false;
	// 清单里的文件齐了才算装好（半截下载不该显示「已安装」）
	for (const auto& f : filesFor(v)) {
		auto p = dir / f.name;
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
		const auto& files = filesFor(v);
		auto destRoot = PluginPaths::ocrModels() / OcrPack::folderName(v);
		std::error_code ec;
		std::filesystem::create_directories(destRoot, ec);
		if (files.empty()) {
			if (done) done(false, L"没有可用的模型清单");
			return;
		}
		const size_t n = files.size();
		for (size_t i = 0; i < n; i++) {
			auto dest = destRoot / files[i].name;
			// 已存在且非空：跳过（重复点下载不必重来）
			if (std::filesystem::is_regular_file(dest, ec) && std::filesystem::file_size(dest, ec) > 0) {
				if (progress) progress((float)(i + 1) / (float)n);
				continue;
			}
			bool ok = SnowHttp::downloadFile(files[i].url, dest, [&](uint64_t got, uint64_t total) {
				if (!progress) return;
				float base = (float)i / (float)n;
				float part = total ? (float)got / (float)total : 0.f;
				progress(base + part / (float)n);
			});
			if (!ok) {
				std::filesystem::remove(dest, ec);
				if (done) done(false, std::wstring(L"模型下载失败：") + files[i].name);
				return;
			}
		}
		if (progress) progress(1.f);
		if (done) done(true, {});
	}).detach();
}
