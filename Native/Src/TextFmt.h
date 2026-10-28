#pragma once
// 轻量字符串格式化：代替 std::format。
//
// 我们只用到「把几个数字/字符串拼进一句话」这一种用法，却要为此实例化
// std::format 的格式串解析器、类型擦除参数表、浮点 to_chars 和 chrono 支持。
// 这里直接用 swprintf，参数类型在编译期做约束，避免把 std::wstring 直接
// 塞进 %s 这种运行期崩溃（忘了 .c_str() 会编译不过，而不是等到线上）。
#include <cstdio>
#include <string>
#include <type_traits>

namespace fmt {

template <class T>
constexpr bool kWfmtArg = std::is_arithmetic_v<std::decay_t<T>>
	|| std::is_convertible_v<T, const wchar_t*>;

template <class... Args>
std::wstring wfmt(const wchar_t* format, Args... args)
{
	static_assert((kWfmtArg<Args> && ...), "wfmt: 参数只能是数字或 const wchar_t*（std::wstring 请传 .c_str()）");
	wchar_t buffer[512];
	const int n = swprintf(buffer, std::size(buffer), format, args...);
	return n > 0 ? std::wstring(buffer, static_cast<size_t>(n)) : std::wstring();
}

} // namespace fmt

using fmt::wfmt;
