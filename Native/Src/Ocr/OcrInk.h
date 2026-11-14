#pragma once
// 从截图像素里量"一行文字的实际笔画高度"（image 像素）。
// 检测框带留白、框高也不一定跟着字号走，所以字号/字身一律用这个量。
// ToolOcr 与截图侧（段落归并）共用同一套算法，避免两边口径不一致。
#include <algorithm>
#include <cmath>
#include <vector>

namespace ocr {
	// 段首左侧的"条目圆点"检测（image 像素坐标；找到返回圆点左边界，否则 -1）。
	// OCR 对列表条目的圆点时认时不认：认得时并进文本，认不得时整颗丢掉 ——
	// 结果续行会被当成新的一段（逐行翻译、句子拦腰截断），译文条目也没了圆点。
	// 判据：段首那一行左侧有一颗孤立、垂直居中、明显小于文字的小圆点，且与正文隔着明显空白。
	// 截图侧（段落归并）与 ToolOcr（译文排版）共用，避免两边口径不一致。
	inline float leadingBulletDotX(const std::vector<BYTE>& px, int W, int H,
		float fx, float fy, float fh)
	{
		if (px.empty() || W <= 0 || H <= 0 || (size_t)W * (size_t)H * 4 > px.size()) return -1.f;
		const float lh = std::max(1.f, fh);
		if (lh < 6.f) return -1.f;
		const int y0 = std::clamp((int)std::floor(fy), 0, H - 1);
		const int y1 = std::clamp((int)std::ceil(fy + fh), y0 + 1, H);
		const int x0 = std::clamp((int)std::floor(fx - lh * 2.0f), 0, W - 1);
		// 右边界要越过段首字形一点：圆点与文字之间的空当是拿"圆点右沿 → 下一个墨迹簇"量的，
		// 只扫到段首左边会把正文第一字形切在窗口外，空当被算成窗口边缘，圆点反而判不出来。
		const int x1 = std::clamp((int)std::ceil(fx + lh * 0.8f), x0 + 1, W);
		const int bw = x1 - x0, bh = y1 - y0;
		if (bw < 6 || bh < 6) return -1.f;

		auto gray = [&](int x, int y) -> int {
			const BYTE* p = px.data() + ((size_t)y * W + x) * 4;   // BGRA
			return (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
		};
		// 底色取扫描带外圈的中位亮度（与 ocrInkHeight 同口径：浅底深字 / 深底浅字）
		std::vector<int> border;
		border.reserve((size_t)(bw + bh) * 2);
		for (int x = x0; x < x1; ++x) { border.push_back(gray(x, y0)); border.push_back(gray(x, y1 - 1)); }
		for (int y = y0; y < y1; ++y) { border.push_back(gray(x0, y)); border.push_back(gray(x1 - 1, y)); }
		std::sort(border.begin(), border.end());
		const int bgLum = border[border.size() / 2];
		const bool darkText = bgLum > 128;

		// 逐列统计前景像素与纵向范围
		std::vector<int> colCount(bw, 0), colTop(bw, -1), colBottom(bw, -1);
		for (int x = x0; x < x1; ++x) {
			const int k = x - x0;
			for (int y = y0; y < y1; ++y) {
				const int g = gray(x, y);
				const bool fg = darkText ? (g < bgLum - 40) : (g > bgLum + 40);
				if (!fg) continue;
				if (colTop[k] < 0) colTop[k] = y;
				colBottom[k] = y;
				++colCount[k];
			}
		}
		// 按列切成墨迹簇（列间断开 1 列以上算另一簇）
		struct Group { int left, right; };
		std::vector<Group> groups;
		int gl = -1, gr = -1;
		for (int k = 0; k < bw; ++k) {
			if (colCount[k] <= 0) continue;
			if (gl < 0) { gl = x0 + k; gr = x0 + k; }
			else if (x0 + k - gr <= 1) gr = x0 + k;
			else { groups.push_back({ gl, gr }); gl = x0 + k; gr = x0 + k; }
		}
		if (gl >= 0) groups.push_back({ gl, gr });

		const float textStart = fx + 0.35f * lh;   // 段首字形的左沿（留一点容差）
		for (size_t g = 0; g < groups.size(); ++g) {
			const int l = groups[g].left, rr = groups[g].right;
			if ((float)rr >= textStart) break;      // 已经到正文了，它右边不会再有圆点
			int top = H, bottom = -1;
			for (int x = l; x <= rr; ++x) {
				const int k = x - x0;
				if (colCount[k] <= 0) continue;
				top = std::min(top, colTop[k]);
				bottom = std::max(bottom, colBottom[k]);
			}
			if (top >= bottom) continue;
			const float dotW = (float)(rr - l + 1), dotH = (float)(bottom - top + 1);
			const float cy = (top + bottom) * 0.5f - y0;
			if (dotW > lh * 0.5f || dotH > lh * 0.55f) continue;          // 太宽/太高：是字或图标
			if (cy < lh * 0.18f || cy > lh * 0.85f) continue;             // 不在行中部：不是条目圆点
			// 与正文之间要隔着明显空白：圆点与文字之间的空当远大于普通字距
			const int nextLeft = (g + 1 < groups.size()) ? groups[g + 1].left : x1;
			if ((float)(nextLeft - rr - 1) < std::max(2.f, lh * 0.18f)) continue;
			return (float)l;
		}
		return -1.f;
	}

	inline float ocrInkHeight(const std::vector<BYTE>& px, int W, int H,
		float fx, float fy, float fw, float fh)
	{
		if (px.empty() || W <= 0 || H <= 0 || (size_t)W * (size_t)H * 4 > px.size()) return 0.f;
		const int x0 = std::clamp((int)std::floor(fx), 0, W - 1);
		const int y0 = std::clamp((int)std::floor(fy), 0, H - 1);
		const int x1 = std::clamp((int)std::ceil(fx + fw), x0 + 1, W);
		const int y1 = std::clamp((int)std::ceil(fy + fh), y0 + 1, H);
		const int bw = x1 - x0, bh = y1 - y0;
		if (bw < 4 || bh < 4) return 0.f;

		auto gray = [&](int x, int y) -> int {
			const BYTE* p = px.data() + ((size_t)y * W + x) * 4;   // BGRA
			return (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
		};
		std::vector<int> border;
		border.reserve((size_t)(bw + bh) * 2);
		for (int x = x0; x < x1; ++x) { border.push_back(gray(x, y0)); border.push_back(gray(x, y1 - 1)); }
		for (int y = y0; y < y1; ++y) { border.push_back(gray(x0, y)); border.push_back(gray(x1 - 1, y)); }
		if (border.size() < 4) return 0.f;
		std::sort(border.begin(), border.end());
		const int bgLum = border[border.size() / 2];
		const bool darkText = bgLum > 128;   // 浅底深字 / 深底浅字

		std::vector<int> counts(bh, 0);
		int maxCount = 0;
		for (int y = 0; y < bh; ++y) {
			int n = 0;
			for (int x = 0; x < bw; ++x) {
				const int g = gray(x0 + x, y0 + y);
				if (darkText ? (g < bgLum - 40) : (g > bgLum + 40)) ++n;
			}
			counts[y] = n;
			maxCount = std::max(maxCount, n);
		}
		if (maxCount < std::max(2, bw / 40)) return 0.f;   // 几乎无前景：纯色块 / 对比度不足

		const int thr = std::max(1, (int)(maxCount * 0.10f));
		int first = -1, last = -1;
		for (int y = 0; y < bh; ++y) {
			if (counts[y] >= thr) { if (first < 0) first = y; last = y; }
		}
		if (first < 0 || last < first) return 0.f;
		return (float)(last - first + 1);
	}
}
