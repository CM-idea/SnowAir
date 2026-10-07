#pragma once

// 序号 Emoji 列表
namespace SerialEmoji
{
	inline constexpr const wchar_t* Default = L"🤪";

	inline constexpr const wchar_t* const kList[] = {
		L"🙂", L"🙄", L"😅", L"🤪",
		L"😏", L"😘", L"😍", L"😩",
		L"😭", L"😱", L"🤬", L"💩",
		L"🌚", L"🌝", L"🔴", L"🟢",
		L"㊙️", L"❤️", L"💀", L"⭕",
		L"❌", L"❄", L"🔗", L"📌",
		L"💥", L"✨", L"❗", L"❓",
	};
	inline constexpr int kCount = (int)(sizeof(kList) / sizeof(kList[0]));
	inline constexpr int kCols = 7;
}
