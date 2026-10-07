#pragma once
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
	/** 下载「引擎 DLL + 指定语言包」；progress 0~1；回调在下载线程，需自行切回 UI */
	void download(const std::wstring& packId,
		std::function<void(float)> progress,
		std::function<void(bool ok, std::wstring err)> done);
	/** 卸载指定语言包；若卸载后不再有任何语言包，连引擎 DLL 一起清掉 */
	void uninstallPack(const std::wstring& packId);
private:
	TranslateModelManager() = default;
};
