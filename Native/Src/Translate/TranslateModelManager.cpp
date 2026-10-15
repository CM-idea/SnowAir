#include "pch.h"
#include "TranslateModelManager.h"
#include "../Ocr/PluginPaths.h"
#include "../Net/HttpClient.h"
#include <thread>
#include <vector>
#include <fstream>

// 离线翻译资源清单与下载。
namespace {

	// Firefox Translations 模型都放在这个 GCS 桶下（路径取自 Mozilla models.json Release）
	const std::wstring kGcsBase =
		L"https://storage.googleapis.com/moz-fx-translations-data--303e-prod-translations-data/";
	// bergamot 引擎来自 BergamotTranslatorsSharp —— nupkg 本质是个 zip
	const std::wstring kBergamotNupkg =
		L"https://api.nuget.org/v3-flatcontainer/bergamottranslatorsharp/0.5.1/bergamottranslatorsharp.0.5.1.nupkg";

	struct PairSpec {
		std::wstring pairId;
		std::wstring modelGz;    // 相对 kGcsBase 的路径
		std::wstring lexGz;
		std::wstring vocabGz;    // 共用词表（与 src/trg 二选一）
		std::wstring srcVocabGz;
		std::wstring trgVocabGz;
	};
	struct PackSpec {
		std::wstring id;
		std::vector<PairSpec> pairs;
	};

	PairSpec pairShared(const std::wstring& pairId, const std::wstring& dir, const std::wstring& stem)
	{
		PairSpec p;
		p.pairId = pairId;
		p.modelGz = dir + L"/model." + stem + L".intgemm.alphas.bin.gz";
		p.lexGz = dir + L"/lex.50.50." + stem + L".s2t.bin.gz";
		p.vocabGz = dir + L"/vocab." + stem + L".spm.gz";
		return p;
	}
	PairSpec pairSplit(const std::wstring& pairId, const std::wstring& dir, const std::wstring& stem)
	{
		PairSpec p;
		p.pairId = pairId;
		p.modelGz = dir + L"/model." + stem + L".intgemm.alphas.bin.gz";
		p.lexGz = dir + L"/lex.50.50." + stem + L".s2t.bin.gz";
		p.srcVocabGz = dir + L"/srcvocab." + stem + L".spm.gz";
		p.trgVocabGz = dir + L"/trgvocab." + stem + L".spm.gz";
		return p;
	}

	// 语言包顺序与 WinSettingPlugins 的 kLangPacks 一致
	const std::vector<PackSpec>& allPacks()
	{
		static const std::vector<PackSpec> all = [] {
			std::vector<PackSpec> out;
			{
				PackSpec p; p.id = L"zh-en";
				p.pairs = {
					pairShared(L"zh-en", L"models/zh-en/cjk_icu_base_LQeOIbF7Sbq3XA8lsRPotw/exported", L"zhen"),
					pairSplit(L"en-zh", L"models/en-zh/llmaat_finetune10M_qe8_f2_ByQcSxGXQRqGi-UTxYE43g/exported", L"enzh"),
				};
				out.push_back(p);
			}
			{
				PackSpec p; p.id = L"zh-Hant";
				p.pairs = {
					pairSplit(L"zh_hant-en", L"models/zh_hant-en/zh_hant_openlid_zh_tw_lr0002_WJi5Ozi7SZWC6hgfD5GhTA/exported", L"zh_hanten"),
					pairSplit(L"en-zh_hant", L"models/en-zh_hant/zh_hant_llmaat_finetune10M_qe8_f2_aQ8azdOMQOSBVjBDOVDIZQ/exported", L"enzh_hant"),
				};
				out.push_back(p);
			}
			{
				PackSpec p; p.id = L"ru";
				p.pairs = {
					pairShared(L"ru-en", L"models/ru-en/spring-2024_QrcdYgbwS7e7xbhtOSdoNQ/exported", L"ruen"),
					pairShared(L"en-ru", L"models/en-ru/student_base_AYqN3ysXRp2EGkEqeaA5Rg/exported", L"enru"),
				};
				out.push_back(p);
			}
			{
				PackSpec p; p.id = L"ko";
				p.pairs = {
					pairShared(L"ko-en", L"models/ko-en/cjk_icu_base_BnKgBdd0Rzq87oUYN3L9-A/exported", L"koen"),
					pairShared(L"en-ko", L"models/en-ko/cjk_hplt2_Fzrv_XPwTs6KVNkktUeuOA/exported", L"enko"),
				};
				out.push_back(p);
			}
			{
				PackSpec p; p.id = L"ja";
				p.pairs = {
					pairShared(L"ja-en", L"models/ja-en/cjk_icu_base_U4VUAW3STh-bF0Sr-dX69g/exported", L"jaen"),
					pairSplit(L"en-ja", L"models/en-ja/llmaat_finetune10M_qe8_f2_ApiGGQIwTKuF9i_k3n9Q2Q/exported", L"enja"),
				};
				out.push_back(p);
			}
			{
				PackSpec p; p.id = L"fr";
				p.pairs = {
					pairShared(L"fr-en", L"models/fr-en/retrain_hr_EFgIftH_RrCyzl5gjemVNg/exported", L"fren"),
					pairShared(L"en-fr", L"models/en-fr/retrain_hr_NLIxDbE1TBGyOTI-zwZagw/exported", L"enfr"),
				};
				out.push_back(p);
			}
			{
				PackSpec p; p.id = L"es";
				p.pairs = {
					pairShared(L"es-en", L"models/es-en/retrain_hr_HbNjJ60BTwmVTbhfFxuduA/exported", L"esen"),
					pairShared(L"en-es", L"models/en-es/retrain_hr_fix_names_CUAEXUHoQum_cFqh-ZAryw/exported", L"enes"),
				};
				out.push_back(p);
			}
			{
				PackSpec p; p.id = L"de";
				p.pairs = {
					pairShared(L"de-en", L"models/de-en/retrain_hr2_WZOW24i4QJmIib61LeHfwg/exported", L"deen"),
					pairShared(L"en-de", L"models/en-de/retrain_hr_fix_names_SCgGhxUPQ2WAECHLRtzrMg/exported", L"ende"),
				};
				out.push_back(p);
			}
			return out;
		}();
		return all;
	}

	const PackSpec* packById(const std::wstring& id)
	{
		const auto& all = allPacks();
		for (const auto& p : all) if (p.id == id) return &p;
		return all.empty() ? nullptr : &all.front();
	}

	bool endsWith(const std::wstring& s, const std::wstring& suf)
	{
		return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
	}
	std::wstring stripGz(std::wstring p)
	{
		if (endsWith(p, L".gz")) p.resize(p.size() - 3);
		return p;
	}
	std::wstring fileNameOf(const std::wstring& p)
	{
		auto pos = p.find_last_of(L"/\\");
		return pos == std::wstring::npos ? p : p.substr(pos + 1);
	}
	std::string narrow(const std::wstring& s)
	{
		std::string out;
		out.reserve(s.size());
		for (wchar_t c : s) out.push_back(c < 128 ? (char)c : '?');   // 资源名都是 ASCII
		return out;
	}

	// 隐藏窗口跑一个命令（PowerShell 解压 / 解 gz 用）
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

	// PowerShell GzipStream 解 gz
	bool gunzip(const std::filesystem::path& gz, const std::filesystem::path& out)
	{
		std::error_code ec;
		std::filesystem::create_directories(out.parent_path(), ec);
		std::filesystem::remove(out, ec);
		std::wstring cmd =
			L"powershell -NoProfile -Command \"$i='" + gz.wstring() + L"';$o='" + out.wstring()
			+ L"';$fs=[IO.File]::OpenRead($i);"
			L"$gz=New-Object IO.Compression.GzipStream($fs,[IO.Compression.CompressionMode]::Decompress);"
			L"$os=[IO.File]::Create($o);$gz.CopyTo($os);$os.Close();$gz.Close();$fs.Close()\"";
		if (!runHidden(cmd)) return false;
		return std::filesystem::is_regular_file(out, ec) && std::filesystem::file_size(out, ec) > 64;
	}

	// 单个语言对目录写出 bergamot 需要的配置
	bool writeConfigForPairDir(const std::filesystem::path& pairDir)
	{
		std::error_code ec;
		std::wstring model, lex, vocab0, vocab1, sharedVocab;
		for (auto& e : std::filesystem::directory_iterator(pairDir, ec)) {
			if (ec || !e.is_regular_file(ec)) continue;
			auto n = e.path().filename().wstring();
			if (n.rfind(L"model.", 0) == 0 && endsWith(n, L".bin")) model = n;
			else if (n.rfind(L"lex.", 0) == 0 && endsWith(n, L".bin")) lex = n;
			else if (n.rfind(L"vocab.", 0) == 0 && endsWith(n, L".spm")) sharedVocab = n;
			else if (n.rfind(L"srcvocab.", 0) == 0 && endsWith(n, L".spm")) vocab0 = n;
			else if (n.rfind(L"trgvocab.", 0) == 0 && endsWith(n, L".spm")) vocab1 = n;
		}
		if (model.empty() || lex.empty()) return false;
		if (!sharedVocab.empty()) vocab0 = vocab1 = sharedVocab;
		if (vocab0.empty() || vocab1.empty()) return false;

		std::string yml;
		yml += "relative-paths: true\nmodels:\n- "; yml += narrow(model);
		yml += "\nvocabs:\n- "; yml += narrow(vocab0);
		yml += "\n- "; yml += narrow(vocab1);
		yml += "\nshortlist:\n- "; yml += narrow(lex);
		yml += R"(
- false
beam-size: 1
normalize: 1.0
word-penalty: 0
max-length-break: 128
mini-batch-words: 1024
workspace: 128
max-length-factor: 2.0
skip-cost: true
cpu-threads: 0
quiet: true
quiet-translation: true
gemm-precision: int8shiftAlphaAll
alignment: soft
)";
		std::ofstream f(pairDir / L"config.intgemm8bitalpha.yml", std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f.write(yml.data(), (std::streamsize)yml.size());
		return f.good();
	}

	// 只解出某个 RID 的 native 目录（整包解压太慢：包里还有 x86/arm64/其它平台的一大堆东西）。
	// nupkg 本质是 zip，直接用 .NET ZipFile 按前缀挑条目，比 Expand-Archive 快得多。
	bool unzipRidNative(const std::filesystem::path& nupkg, const std::wstring& rid,
		const std::filesystem::path& dest)
	{
		std::error_code ec;
		std::filesystem::create_directories(dest, ec);
		std::wstring cmd = L"powershell -NoProfile -Command \"Add-Type -AssemblyName System.IO.Compression.FileSystem;";
		cmd += L"$z=[System.IO.Compression.ZipFile]::OpenRead('" + nupkg.wstring() + L"');";
		cmd += L"foreach($e in $z.Entries){ if($e.FullName -like 'runtimes/" + rid
			+ L"/native/*'){ [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e,(Join-Path '"
			+ dest.wstring() + L"' $e.Name),$true) } };$z.Dispose()\"";
		return runHidden(cmd);
	}

	// 取出引擎 DLL：依次试 win-x64 → win-arm64 → win-x86，挑到 bergamot.dll 连同同目录依赖一起拷进 runtime 目录。
	// onProgress：0~1 反映"取 DLL"这一步内部的进度（含逐 RID 尝试）。
	bool extractBergamotDll(const std::filesystem::path& nupkg, const std::filesystem::path& runtimeDir,
		const std::function<void(float)>& onProgress)
	{
		static const wchar_t* kRids[] = { L"win-x64", L"win-arm64", L"win-x86" };
		auto tmp = runtimeDir / L"_bergamot_tmp";
		std::error_code ec;
		for (int i = 0; i < 3; i++) {
			std::filesystem::remove_all(tmp, ec);
			std::filesystem::create_directories(tmp, ec);
			if (onProgress) onProgress((float)i / 3.f);
			if (!unzipRidNative(nupkg, kRids[i], tmp)) continue;
			auto dll = tmp / L"bergamot.dll";
			if (!std::filesystem::is_regular_file(dll, ec) || std::filesystem::file_size(dll, ec) < 8ull * 1024 * 1024)
				continue;
			std::filesystem::create_directories(runtimeDir, ec);
			std::filesystem::remove(runtimeDir / L"bergamot.dll", ec);
			// 同目录依赖（onnxruntime 等）一并拷过去
			for (auto& e : std::filesystem::directory_iterator(tmp, ec)) {
				if (!e.is_regular_file(ec)) continue;
				std::filesystem::copy_file(e.path(), runtimeDir / e.path().filename(),
					std::filesystem::copy_options::overwrite_existing, ec);
			}
			std::filesystem::remove_all(tmp, ec);
			if (onProgress) onProgress(1.f);
			return std::filesystem::file_size(runtimeDir / L"bergamot.dll", ec) > 8ull * 1024 * 1024;
		}
		std::filesystem::remove_all(tmp, ec);
		return false;
	}
}

TranslateModelManager& TranslateModelManager::instance()
{
	static TranslateModelManager inst;
	return inst;
}

bool TranslateModelManager::isDllInstalled() const
{
	std::error_code ec;
	auto dll = PluginPaths::bergamotDll();
	return std::filesystem::is_regular_file(dll, ec) && std::filesystem::file_size(dll, ec) > 8ull * 1024 * 1024;
}

bool TranslateModelManager::isBranchInstalled(const std::wstring& pairId) const
{
	auto dir = PluginPaths::translateModels() / pairId;
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) return false;
	bool hasModel = false, hasLex = false, hasShared = false, hasSrc = false, hasTrg = false;
	for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
		if (ec || !e.is_regular_file(ec)) continue;
		auto n = e.path().filename().wstring();
		if (n.rfind(L"model.", 0) == 0 && endsWith(n, L".bin")) hasModel = true;
		else if (n.rfind(L"lex.", 0) == 0 && endsWith(n, L".bin")) hasLex = true;
		else if (n.rfind(L"vocab.", 0) == 0 && endsWith(n, L".spm")) hasShared = true;
		else if (n.rfind(L"srcvocab.", 0) == 0 && endsWith(n, L".spm")) hasSrc = true;
		else if (n.rfind(L"trgvocab.", 0) == 0 && endsWith(n, L".spm")) hasTrg = true;
	}
	if (!hasModel || !hasLex) return false;
	if (!hasShared && !(hasSrc && hasTrg)) return false;
	return std::filesystem::exists(dir / L"config.intgemm8bitalpha.yml", ec);
}

bool TranslateModelManager::isPackInstalled(const std::wstring& packId) const
{
	const PackSpec* pack = packById(packId);
	if (!pack || pack->pairs.empty()) return false;
	for (const auto& pr : pack->pairs)
		if (!isBranchInstalled(pr.pairId)) return false;
	return true;
}

std::vector<std::wstring> TranslateModelManager::installedPacks() const
{
	std::vector<std::wstring> out;
	if (!isDllInstalled()) return out;
	for (const auto& p : allPacks())
		if (isPackInstalled(p.id)) out.push_back(p.id);
	return out;
}

void TranslateModelManager::uninstallPack(const std::wstring& packId)
{
	const PackSpec* pack = packById(packId);
	if (!pack) return;
	std::error_code ec;
	auto models = PluginPaths::translateModels();
	for (const auto& pr : pack->pairs)
		std::filesystem::remove_all(models / pr.pairId, ec);
	// 没有任何语言包了 → 引擎 DLL 也一并清掉（无包即无引擎）
	bool anyLeft = false;
	for (const auto& p : allPacks()) {
		if (p.id == packId) continue;
		if (isPackInstalled(p.id)) { anyLeft = true; break; }
	}
	if (!anyLeft) std::filesystem::remove_all(PluginPaths::translateRoot(), ec);
}

void TranslateModelManager::download(const std::wstring& packId,
	std::function<void(const DownloadProgress&)> progress,
	std::function<void(bool, std::wstring)> done)
{
	std::thread([packId, progress, done]() {
		std::error_code ec;
		const PackSpec* pack = packById(packId);
		if (!pack || pack->pairs.empty()) {
			if (done) done(false, L"未知的语言包");
			return;
		}
		auto& self = TranslateModelManager::instance();
		auto root = PluginPaths::translateRoot();
		auto models = PluginPaths::translateModels();
		std::filesystem::create_directories(models, ec);

		// 进度权重：引擎 DLL 那一步体积最大（35MB 的 nupkg），单独给 6(下载)+2(解压) 的权重，
		// 每个模型文件算 1 —— 早先按"文件个数"分摊，DLL 只占 1/N，进度会长时间钉在 12% 那一档
		// （用户反馈"下载卡到 13%"），实际是下载完在整包解压、还不上报进度。
		const size_t wDllDown = self.isDllInstalled() ? 0 : 6;
		const size_t wDllExtract = self.isDllInstalled() ? 0 : 2;
		size_t totalFiles = wDllDown + wDllExtract;
		for (const auto& pr : pack->pairs)
			totalFiles += 2 + (!pr.vocabGz.empty() ? 1 : 0)
				+ (!pr.srcVocabGz.empty() ? 1 : 0) + (!pr.trgVocabGz.empty() ? 1 : 0);

		size_t step = 0;
		DownloadProgress st;
		// part = 当前这一步内部进度(0~1)；每次上报都带上 overall 与阶段信息
		auto report = [&](float part) {
			st.overall = ((float)step + part) / (float)totalFiles;
			if (progress) progress(st);
		};

		// 1) 引擎 DLL：下载 → 解压出 win 版 bergamot.dll
		if (!self.isDllInstalled()) {
			auto nupkg = root / L"bergamot.nupkg";
			std::filesystem::create_directories(root, ec);
			st.stage = 0;
			st.item.clear();
			st.got = st.total = 0;
			report(0.f);
			if (!SnowHttp::downloadFile(kBergamotNupkg, nupkg, [&](uint64_t g, uint64_t t) {
					st.got = g;
					st.total = t;
					report(t ? (float)g / (float)t * (float)wDllDown : 0.f);
				})) {
				std::filesystem::remove(nupkg, ec);
				if (done) done(false, L"bergamot 引擎下载失败，请检查网络后重试");
				return;
			}
			step += wDllDown;
			st.stage = 1;
			st.got = st.total = 0;
			report(0.f);
			if (!extractBergamotDll(nupkg, root, [&](float part) {
					report(part * (float)wDllExtract);
				})) {
				std::filesystem::remove(nupkg, ec);
				if (done) done(false, L"解压 bergamot.dll 失败");
				return;
			}
			std::filesystem::remove(nupkg, ec);
			step += wDllExtract;
			report(0.f);
		}

		// 2) 每个语言对的模型（.gz → 解压成 .bin/.spm）
		for (const auto& pr : pack->pairs) {
			auto pairDir = models / pr.pairId;
			std::filesystem::create_directories(pairDir, ec);
			auto fetch = [&](const std::wstring& rel) -> bool {
				if (rel.empty()) return true;
				auto gzName = fileNameOf(rel);
				auto gzPath = pairDir / gzName;
				auto outPath = pairDir / stripGz(gzName);
				bool ok = false;
				if (std::filesystem::is_regular_file(outPath, ec) && std::filesystem::file_size(outPath, ec) > 64) {
					ok = true;                       // 已解压：跳过
				}
				else {
					st.stage = 2;
					st.item = pr.pairId;
					st.got = st.total = 0;
					report(0.f);
					ok = SnowHttp::downloadFile(kGcsBase + rel, gzPath, [&](uint64_t g, uint64_t t) {
						st.got = g;
						st.total = t;
						report(t ? (float)g / (float)t : 0.f);
					});
					if (ok) {
						st.stage = 3;
						st.got = st.total = 0;
						report(0.f);
						ok = gunzip(gzPath, outPath);
					}
					std::filesystem::remove(gzPath, ec);
				}
				++step;
				report(0.f);
				return ok;
			};
			if (!fetch(pr.modelGz) || !fetch(pr.lexGz) || !fetch(pr.vocabGz)
				|| !fetch(pr.srcVocabGz) || !fetch(pr.trgVocabGz)) {
				if (done) done(false, std::wstring(L"语言包下载失败：") + pr.pairId);
				return;
			}
			writeConfigForPairDir(pairDir);
		}

		st.stage = 2;
		st.item.clear();
		st.got = st.total = 0;
		st.overall = 1.f;
		if (progress) progress(st);
		if (done) done(true, {});
	}).detach();
}
