#pragma once
#include "TranslateTypes.h"
#include <vector>

class TranslateService {
public:
	static TranslateService& instance();
	static std::wstring resolveProvider(const std::wstring& preferred);
	void translate(const TranslateRequest& req, TranslateCallback cb);
	void translateSegments(const std::vector<std::wstring>& segments,
		const TranslateRequest& base, TranslateCallback cb);
private:
	TranslateService() = default;
	// 按优先级排出的服务链：用户选的服务 → 其它在线服务 → 离线（装了才加）
	static std::vector<std::wstring> providerChain(const std::wstring& preferred);
	// 用指定服务同步翻一次（在线引擎是异步回调，这里等它回来）
	static TranslateResult translateOnce(const TranslateRequest& req);
	// 依次尝试整条链，第一个成功即返回；命中的服务会被提到链首（多段时后续分段不再从头试）
	static TranslateResult runChain(const TranslateRequest& base, std::vector<std::wstring>& chain);
};
