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
	void dispatch(const TranslateRequest& req, TranslateCallback cb);
};
