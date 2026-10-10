#include "pch.h"
#include "PpOcrEngine.h"
#include "PluginPaths.h"
#include "../Util.h"
#include "../Setting.h"
#include <fstream>

namespace PpOcrEngine {
namespace {

// RapidOCR.dll 的 C 接口（该 DLL 只导出这两个函数）：
//   const char* RapidOcrFromPathW(const wchar_t* imagePath, const char* optionsJson);
//   const char* RapidOcrFromBytes(const unsigned char* bytes, size_t len, const char* optionsJson);
// 返回 UTF-8 JSON：{"code":100,"message":"ok","data":[{"box":[[x,y]...],"text":".."}]}，
// 失败也返回 JSON（code != 100）。引擎由 DLL 内部自管，没有 create/free。
using RapidOcrFromPathW = const char* (__cdecl*)(const wchar_t* imagePath, const char* optionsJson);

HMODULE loadDll(OcrPackVariant v)
{
	auto dir = PluginPaths::ocrRuntime(v);
	auto dll = dir / L"RapidOCR.dll";
	if (!std::filesystem::exists(dll))
		dll = PluginPaths::ocrBin() / L"RapidOCR.dll";
	if (!std::filesystem::exists(dll)) return nullptr;
	return LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// 在模型目录里按文件名找 *_det / *_cls / *_rec 三个 onnx（大小写不敏感）
std::filesystem::path findModel(const std::filesystem::path& dir, const std::wstring& tag)
{
	std::error_code ec;
	for (auto it = std::filesystem::directory_iterator(dir, ec);
		!ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
		if (!it->is_regular_file(ec)) continue;
		std::wstring name = it->path().filename().wstring();
		std::wstring lower = name;
		for (auto& c : lower) c = (wchar_t)towlower(c);
		if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, L".onnx") == 0
			&& lower.find(tag) != std::wstring::npos)
			return it->path();
	}
	return {};
}

// 把已下载的模型（ocr-models/<variant>/）放到 RapidOCR.dll 同级的 models/ 下。
// 实测该 DLL 只认自己目录的 models/（按 *_det*.onnx / *_cls*.onnx / *_rec*.onnx 匹配），
// optionsJson 里给的路径不生效；而模型是另一个下载器管着的，未必在 DLL 目录里。
bool ensureModelsNextToDll(HMODULE mod, OcrPackVariant v)
{
	wchar_t buf[MAX_PATH]{};
	if (!GetModuleFileNameW(mod, buf, MAX_PATH)) return false;
	auto dst = std::filesystem::path(buf).parent_path() / L"models";
	auto src = PluginPaths::ocrModels() / OcrPack::folderName(v);
	std::error_code ec;
	if (!std::filesystem::exists(src, ec)) return false;

	std::vector<std::filesystem::path> need;
	for (const wchar_t* tag : { L"_det", L"_cls", L"_rec" }) {
		auto f = findModel(src, tag);
		if (!f.empty()) need.push_back(f);
	}
	if (need.empty()) return false;

	std::filesystem::create_directories(dst, ec);
	for (const auto& f : need) {
		auto target = dst / f.filename();
		if (std::filesystem::exists(target, ec)) continue;
		// 同盘用硬链接（零拷贝），跨盘才复制
		if (CreateHardLinkW(target.c_str(), f.c_str(), nullptr)) continue;
		std::filesystem::copy_file(f, target, std::filesystem::copy_options::overwrite_existing, ec);
	}
	return true;
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
			// 该 DLL 失败时也返回 JSON：{"code":404,"message":"..."}
			if (obj.HasKey(L"code") && (int)obj.GetNamedNumber(L"code") != 100) {
				r.error = obj.HasKey(L"message")
					? std::wstring(obj.GetNamedString(L"message").c_str())
					: L"PP-OCR 识别失败";
				return r;
			}
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
					if (box.Size() >= 4) {
						if (box.GetAt(0).ValueType() == winrt::Windows::Data::Json::JsonValueType::Number) {
							// [x, y, w, h]
							x = (float)box.GetAt(0).GetNumber();
							y = (float)box.GetAt(1).GetNumber();
							w = (float)box.GetAt(2).GetNumber();
							h = (float)box.GetAt(3).GetNumber();
						}
						else if (box.GetAt(0).ValueType() == winrt::Windows::Data::Json::JsonValueType::Array) {
							// [[x1,y1],[x2,y2],[x3,y3],[x4,y4]] → 取包围盒
							float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
							for (auto const& pt : box) {
								if (pt.ValueType() != winrt::Windows::Data::Json::JsonValueType::Array) continue;
								auto pa = pt.GetArray();
								if (pa.Size() < 2) continue;
								const float px = (float)pa.GetAt(0).GetNumber();
								const float py = (float)pa.GetAt(1).GetNumber();
								minX = std::min(minX, px); minY = std::min(minY, py);
								maxX = std::max(maxX, px); maxY = std::max(maxY, py);
							}
							if (maxX >= minX && maxY >= minY) {
								x = minX; y = minY; w = maxX - minX; h = maxY - minY;
							}
						}
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

	// v4/v5: RapidOCR.dll（导出 RapidOcrFromPathW，引擎由 DLL 内部自管）
	HMODULE mod = loadDll(v);
	if (!mod) {
		r.error = std::wstring(OcrPack::label(v)) + L" RapidOCR.dll 未安装";
		return r;
	}
	auto fromPath = (RapidOcrFromPathW)GetProcAddress(mod, "RapidOcrFromPathW");
	if (!fromPath) {
		FreeLibrary(mod);
		r.error = L"RapidOCR.dll 导出符号不匹配（缺少 RapidOcrFromPathW）";
		return r;
	}
	if (!ensureModelsNextToDll(mod, v)) {
		FreeLibrary(mod);
		r.error = std::wstring(OcrPack::label(v)) + L" 模型文件缺失，请在「插件」页重新下载";
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
	const char* json = fromPath(img.wstring().c_str(), nullptr);   // 模型路径由 DLL 自治，options 传 NULL
	std::string payload = json ? json : "";
	FreeLibrary(mod);
	std::filesystem::remove(img, ec);
	return parseJsonPayload(payload, r.engineUsed);
}

} // namespace PpOcrEngine
