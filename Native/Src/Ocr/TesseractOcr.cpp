#include "pch.h"
#include "TesseractOcr.h"
#include "PluginPaths.h"
#include "../Util.h"
#include "../Setting.h"
#include <fstream>
#include <sstream>

namespace TesseractOcr {
namespace {

std::filesystem::path findTesseract()
{
	wchar_t buf[MAX_PATH]{};
	if (SearchPathW(nullptr, L"tesseract.exe", nullptr, MAX_PATH, buf, nullptr))
		return buf;
	const wchar_t* candidates[] = {
		L"C:\\Program Files\\Tesseract-OCR\\tesseract.exe",
		L"C:\\Program Files (x86)\\Tesseract-OCR\\tesseract.exe",
	};
	for (auto* p : candidates) {
		if (std::filesystem::exists(p)) return p;
	}
	auto bundled = PluginPaths::ocrBin() / L"tesseract.exe";
	if (std::filesystem::exists(bundled)) return bundled;
	return {};
}

std::wstring readAllText(const std::filesystem::path& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return {};
	std::string utf8((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (utf8.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
	if (n <= 0) return {};
	std::wstring out(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
	return out;
}

} // namespace

OcrResult recognize(int w, int h, const BYTE* bgra)
{
	OcrResult r;
	r.engineUsed = OcrEngineKind::Tesseract;
	auto exe = findTesseract();
	if (exe.empty()) {
		r.error = L"未找到 tesseract.exe，请安装 Tesseract 或放到插件目录";
		return r;
	}
	auto tmpDir = Setting::get()->getDataPath() / L"tmp";
	std::error_code ec;
	std::filesystem::create_directories(tmpDir, ec);
	auto img = tmpDir / L"tess_ocr.png";
	auto base = tmpDir / L"tess_out";
	if (!Util::saveToFile(img.wstring(), w, h, const_cast<BYTE*>(bgra))) {
		r.error = L"无法写出临时图片";
		return r;
	}
	std::wstring cmd = L"\"" + exe.wstring() + L"\" \"" + img.wstring()
		+ L"\" \"" + base.wstring() + L"\" -l chi_sim+eng --psm 6 tsv";
	STARTUPINFOW si{ .cb = sizeof(si) };
	PROCESS_INFORMATION pi{};
	std::wstring mutableCmd = cmd;
	if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &si, &pi)) {
		r.error = L"无法启动 Tesseract";
		std::filesystem::remove(img, ec);
		return r;
	}
	WaitForSingleObject(pi.hProcess, 60000);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	auto tsvPath = base.wstring() + L".tsv";
	auto tsv = readAllText(tsvPath);
	std::filesystem::remove(img, ec);
	std::filesystem::remove(tsvPath, ec);

	// 简化：整页文本（从 tsv 拼 word 列）
	std::wstring plain;
	std::wistringstream ss(tsv);
	std::wstring line;
	bool header = true;
	while (std::getline(ss, line)) {
		if (header) { header = false; continue; }
		if (line.empty()) continue;
		// TSV: level page_num ... left top width height conf text
		std::vector<std::wstring> cols;
		size_t start = 0;
		while (start <= line.size()) {
			auto tab = line.find(L'\t', start);
			if (tab == std::wstring::npos) {
				cols.push_back(line.substr(start));
				break;
			}
			cols.push_back(line.substr(start, tab - start));
			start = tab + 1;
		}
		if (cols.size() < 12) continue;
		if (cols[0] != L"5") continue; // word level
		const auto& word = cols.back();
		if (word.empty() || word == L"") continue;
		OcrRegion reg;
		reg.text = word;
		reg.displayText = word;
		reg.x = (float)_wtof(cols[6].c_str());
		reg.y = (float)_wtof(cols[7].c_str());
		reg.w = std::max(2.f, (float)_wtof(cols[8].c_str()));
		reg.h = std::max(2.f, (float)_wtof(cols[9].c_str()));
		r.regions.push_back(reg);
		if (!plain.empty()) plain += L' ';
		plain += word;
	}
	r.text = plain;
	r.ok = !r.text.empty();
	if (!r.ok) r.error = L"Tesseract 未识别到文字";
	return r;
}

} // namespace TesseractOcr
