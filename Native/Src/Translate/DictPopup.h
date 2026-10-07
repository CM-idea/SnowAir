#pragma once
#include "TranslateTypes.h"
#include <functional>

/** 划词词典弹层（对齐 QT DictPopup；首版简化为回调打开结果） */
namespace DictPopup {
	void lookup(const std::wstring& word, HWND owner,
		std::function<void(const std::wstring& htmlOrText)> done);
}
