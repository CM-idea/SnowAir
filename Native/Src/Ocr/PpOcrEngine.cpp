#include "pch.h"
#include "PpOcrEngine.h"
#include "PluginPaths.h"
#include "../Util.h"
#include "../Setting.h"
#include <fstream>

namespace PpOcrEngine {
namespace {

// RapidOCR C API 约定（与 QT PpOcrV6Engine DLL 路径对齐的简化版）
using RapidOcrCreate = void* (__cdecl*)();
using RapidOcrFree = void(__cdecl*)(void*);
using RapidOcrRun = const char* (__cdecl*)(void*, const char* imgPath, const char* modelsDir);

HMODULE loadDll(OcrPackVariant v)
{
	auto dir = PluginPaths::ocrRuntime(v);
	auto dll = dir / L"RapidOCR.dll";
	if (!std::filesystem::exists(dll))
		dll = PluginPaths::ocrBin() / L"RapidOCR.dll";
	if (!std::filesystem::exists(dll)) return nullptr;
	return LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

std::wstring utf8ToW(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	if (n <= 0) return {};
	std::wstring out(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
	return out;
}

OcrResult parseJsonPayload(const std::string& payload, OcrEngineKind kind)
{
	OcrResult r;
	r.engineUsed = kind;
	if (payload.empty()) {
		r.error = L"PP-OCR 返回为空";
		return r;
	}
	try {
		auto doc = winrt::Windows::Data::Json::JsonValue::Parse(
			winrt::to_hstring(payload));
		winrt::Windows::Data::Json::JsonArray arr;
		if (doc.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array)
			arr = doc.GetArray();
		else if (doc.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
			auto obj = doc.GetObjectW();
			if (obj.HasKey(L"data")) arr = obj.GetNamedArray(L"data");
			else if (obj.HasKey(L"results")) arr = obj.GetNamedArray(L"results");
		}
		std::wstring plain;
		for (auto const& item : arr) {
			std::wstring text;
			float x = 0, y = 0, w = 200, h = 20;
			if (item.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
				auto o = item.GetObjectW();
				if (o.HasKey(L"text")) text = o.GetNamedString(L"text").c_str();
				else if (o.HasKey(L"txt")) text = o.GetNamedString(L"txt").c_str();
				if (o.HasKey(L"box") && o.GetNamedValue(L"box").ValueType()
					== winrt::Windows::Data::Json::JsonValueType::Array) {
					auto box = o.GetNamedArray(L"box");
					if (box.Size() >= 4 && box.GetAt(0).ValueType()
						== winrt::Windows::Data::Json::JsonValueType::Number) {
						x = (float)box.GetAt(0).GetNumber();
						y = (float)box.GetAt(1).GetNumber();
						w = (float)box.GetAt(2).GetNumber();
						h = (float)box.GetAt(3).GetNumber();
					}
				}
				else {
					if (o.HasKey(L"x")) x = (float)o.GetNamedNumber(L"x");
					if (o.HasKey(L"y")) y = (float)o.GetNamedNumber(L"y");
					if (o.HasKey(L"w")) w = (float)o.GetNamedNumber(L"w");
					if (o.HasKey(L"h")) h = (float)o.GetNamedNumber(L"h");
				}
			}
			else if (item.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
				text = item.GetString().c_str();
			}
			if (text.empty()) continue;
			OcrRegion reg;
			reg.text = text;
			reg.displayText = text;
			reg.x = x; reg.y = y;
			reg.w = std::max(2.f, w);
			reg.h = std::max(2.f, h);
			r.regions.push_back(reg);
			if (!plain.empty()) plain += L'\n';
			plain += text;
		}
		r.text = plain;
		r.ok = !r.regions.empty();
		if (!r.ok) r.error = L"PP-OCR 未识别到文字";
	}
	catch (...) {
		// 纯文本回退
		r.text = utf8ToW(payload);
		r.ok = !r.text.empty();
		if (r.ok) {
			OcrRegion reg;
			reg.text = r.text;
			reg.displayText = r.text;
			reg.w = 200; reg.h = 20;
			r.regions.push_back(reg);
		}
		else r.error = L"PP-OCR 结果解析失败";
	}
	return r;
}

OcrEngineKind kindOf(OcrPackVariant v)
{
	switch (v) {
	case OcrPackVariant::Official: return OcrEngineKind::PpOcrV6;
	case OcrPackVariant::Embedded: return OcrEngineKind::PpOcrV5;
	case OcrPackVariant::StableV4: return OcrEngineKind::PpOcrV4;
	}
	return OcrEngineKind::PpOcrV5;
}

} // namespace

bool isInstalled(OcrPackVariant v)
{
	auto models = PluginPaths::ocrModels() / OcrPack::folderName(v);
	if (!std::filesystem::exists(models)) return false;
	return loadDll(v) != nullptr || std::filesystem::exists(PluginPaths::ocrRuntime(v) / L"python.exe");
}

OcrResult recognize(int w, int h, const BYTE* bgra, OcrPackVariant v)
{
	OcrResult r;
	r.engineUsed = kindOf(v);
	auto models = PluginPaths::ocrModels() / OcrPack::folderName(v);
	if (!std::filesystem::exists(models)) {
		r.error = std::wstring(OcrPack::label(v)) + L" 未安装，请到「插件」页下载";
		return r;
	}

	// Official: python worker
	if (v == OcrPackVariant::Official) {
		auto py = PluginPaths::ocrRuntime(v) / L"python.exe";
		auto worker = PluginPaths::ocrRuntime(v) / L"ocr_worker.py";
		if (!std::filesystem::exists(py) || !std::filesystem::exists(worker)) {
			r.error = L"PP-OCRv6 运行时未安装";
			return r;
		}
		auto tmp = Setting::get()->getDataPath() / L"tmp";
		std::error_code ec;
		std::filesystem::create_directories(tmp, ec);
		auto img = tmp / L"ppocr.png";
		auto out = tmp / L"ppocr_out.json";
		if (!Util::saveToFile(img.wstring(), w, h, const_cast<BYTE*>(bgra))) {
			r.error = L"无法写出临时图片";
			return r;
		}
		std::wstring cmd = L"\"" + py.wstring() + L"\" \"" + worker.wstring()
			+ L"\" --image \"" + img.wstring() + L"\" --models \"" + models.wstring()
			+ L"\" --out \"" + out.wstring() + L"\"";
		STARTUPINFOW si{ .cb = sizeof(si) };
		PROCESS_INFORMATION pi{};
		std::wstring mutableCmd = cmd;
		if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, py.parent_path().c_str(), &si, &pi)) {
			r.error = L"无法启动 PP-OCR worker";
			return r;
		}
		WaitForSingleObject(pi.hProcess, 120000);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		std::ifstream f(out, std::ios::binary);
		std::string payload((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::filesystem::remove(img, ec);
		std::filesystem::remove(out, ec);
		return parseJsonPayload(payload, r.engineUsed);
	}

	// v4/v5: RapidOCR.dll
	HMODULE mod = loadDll(v);
	if (!mod) {
		r.error = std::wstring(OcrPack::label(v)) + L" RapidOCR.dll 未安装";
		return r;
	}
	auto create = (RapidOcrCreate)GetProcAddress(mod, "RapidOcrCreate");
	auto freeFn = (RapidOcrFree)GetProcAddress(mod, "RapidOcrFree");
	auto run = (RapidOcrRun)GetProcAddress(mod, "RapidOcrRun");
	if (!create || !freeFn || !run) {
		// 兼容导出名
		create = (RapidOcrCreate)GetProcAddress(mod, "ocr_create");
		freeFn = (RapidOcrFree)GetProcAddress(mod, "ocr_free");
		run = (RapidOcrRun)GetProcAddress(mod, "ocr_run");
	}
	if (!create || !freeFn || !run) {
		FreeLibrary(mod);
		r.error = L"RapidOCR.dll 导出符号不匹配";
		return r;
	}
	auto tmp = Setting::get()->getDataPath() / L"tmp";
	std::error_code ec;
	std::filesystem::create_directories(tmp, ec);
	auto img = tmp / L"ppocr.png";
	if (!Util::saveToFile(img.wstring(), w, h, const_cast<BYTE*>(bgra))) {
		FreeLibrary(mod);
		r.error = L"无法写出临时图片";
		return r;
	}
	void* handle = create();
	std::string imgA = Ling::Util::convertToStr(img.wstring());
	std::string modelsA = Ling::Util::convertToStr(models.wstring());
	const char* json = run(handle, imgA.c_str(), modelsA.c_str());
	std::string payload = json ? json : "";
	freeFn(handle);
	FreeLibrary(mod);
	std::filesystem::remove(img, ec);
	return parseJsonPayload(payload, r.engineUsed);
}

} // namespace PpOcrEngine
