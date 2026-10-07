#pragma once
#include <vector>
#include <string>
#include <cstdint>

// 电子包浆：戳记 + 绿偏 + JPEG 轮次
namespace PatinaEffect
{
	struct Style {
		int strength{ 15 };     // 0–100 → rounds ≈ strength*0.45
		bool green{ true };
		bool watermark{ true };
		int plan{ 0 };          // 0 右下 / 1 下右 / 2 中下（再经随机微调）
		int wmSize{ 20 };
		std::wstring seed;
	};

	// 输入/输出 BGRA 预乘无关的不透明像素（pitch = width*4）
	bool apply(std::vector<BYTE>& bgra, UINT32 width, UINT32 height, UINT32 pitch, const Style& s);
}
