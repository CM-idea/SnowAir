#include "pch.h"
#include "DictPopup.h"
#include "FreeWebTranslate.h"

namespace DictPopup {

void lookup(const std::wstring& word, HWND /*owner*/,
	std::function<void(const std::wstring&)> done)
{
	TranslateRequest req;
	req.text = word;
	req.provider = TranslateProvider::Youdao;
	req.targetLang = L"zh-CN";
	FreeWebTranslate::translate(req, [done](const TranslateResult& tr) {
		if (done) done(tr.ok ? tr.text : tr.error);
	});
}

} // namespace DictPopup
