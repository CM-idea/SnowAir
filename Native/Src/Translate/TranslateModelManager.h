#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 插件集成 · 离线翻译：按语言包下载 Firefox Translations 模型 + bergamot.dll。
class TranslateModelManager {
public:
	static TranslateModelManager& instance();
	/** 引擎 DLL 是否就绪（bergamot.dll 存在且体积像样） */
	bool isDllInstalled() const;
	/** 单个语言对目录是否完整（模型 + 词表 + 配置文件） */
	bool isBranchInstalled(const std::wstring& pairId) const;
	/** 一个语言包（含正向/反向若干语言对）是否完整 */
	bool isPackInstalled(const std::wstring& packId) const;
	/** 已装好的语言包 id（DLL 未装好时返回空） */
	std::vector<std::wstring> installedPacks() const;
	// 下载进度：overall(0~1) 给"整体进度条/按钮"，其余给"当前在干什么"的文案
	struct DownloadProgress {
		float overall{ 0.f };
		int stage{ 0 };          // 0=下载引擎 1=解压引擎 2=下载语言包 3=解压语言包
		std::wstring item;       // 语言对 id（stage 2/3 时用）
		uint64_t got{ 0 };       // 当前项已下/总字节（total=0 表示长度未知）
		uint64_t total{ 0 };
	};
	/** 下载「引擎 DLL + 指定语言包」；progress 在下载线程回调，需自行切回 UI */
	void download(const std::wstring& packId,
		std::function<void(const DownloadProgress&)> progress,
		std::function<void(bool ok, std::wstring err)> done);
	/** 卸载指定语言包；若卸载后不再有任何语言包，连引擎 DLL 一起清掉 */
	void uninstallPack(const std::wstring& packId);
private:
	TranslateModelManager() = default;
};
