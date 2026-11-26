#pragma once
// 长截图拼接内核（纯逻辑，不依赖 Windows）。
//
// 两个阶段：
//   1) 位移估计：相邻两帧之间内容整体移动了多少行，用来“预测”下一帧在长图里的位置；
//   2) 落点判定：把这一帧放到已拼好的画布上的哪个位置——判据只有一条：
//      和画布吻合，且这个位置是唯一的（或等分候选都挤在预测附近，由连续性决定）。
//
// 把画布当基准是这里最关键的一点：画布里有几屏的上下文，重复的行、重复的图案
// 不可能在整段上都吻合；只靠相邻两帧去猜（尤其是背景固定的页面）迟早会错位。
//
// 所有比较都只统计“同一位置上确实变了”的像素：固定背景、吸顶栏、空白区域在两帧
// 里一模一样，不参与打分，因此纯文字、照片、渐变、固定背景都能定位。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace LongStitch {

using Byte = unsigned char;

// 相邻两帧的位移估计结果
struct Step
{
	int shift{ 0 };          // 内容位移：负数=内容上移（页面向下滚）
	int staticTop{ 0 };      // 与上一帧逐像素相同的顶部行数
	int staticBottom{ 0 };
	bool topIsBar{ false };  // 顶部静态带像固定栏（有纹理且内侧内容确实动过），不像整片空白
	bool bottomIsBar{ false };
	bool confident{ false };// shift 可信
	bool outOfRange{ false };// 位移超出可信范围（只当预测不可用，不影响落点判定）
};

// 一次 push 的结果
struct Stitched
{
	bool accepted{ false }; // 找到了落点（画布可能因此增长）
	bool grew{ false };
	bool fromTop{ false };
	int shift{ 0 };         // 相对上一帧落点的位移
	bool sawMotion{ false };// 位移估计确认画面在滚动（不代表一定拼上了）
	bool tooLong{ false };
};

// 帧特征：横向抽样后的亮度与彩色，以及“与上一帧同位置不同”的像素掩码。
// 横向抽样把每帧的开销压到与宽度无关的量级，纵向保留全分辨率以保证位移精度。
struct FrameProfile
{
	int w{ 0 };
	int h{ 0 };
	int stride{ 0 };
	int cols{ 0 };
	std::vector<Byte> lum;     // cols*h：亮度
	std::vector<Byte> rgb;     // cols*h*3：抽样彩色（判断“整帧是否没变”）
	std::vector<Byte> mask;    // cols*h：1 = 这个抽样点两帧不同
	std::vector<int> rowMask;  // h：这一行的掩码点数
	int colStepX{ 1 };         // 抽样列在实际帧里的步长

	int sampleX(int c) const
	{
		int x = c * colStepX + colStepX / 2;
		return std::min(w - 1, x);
	}

	void build(const Byte* bgra, int width, int height, int rowStride)
	{
		w = width;
		h = height;
		stride = rowStride;
		cols = std::clamp(width / 3, 32, 320);
		if (cols > width) cols = width;
		const int step = std::max(1, width / cols);
		colStepX = step;
		lum.assign((size_t)cols * h, 0);
		rgb.assign((size_t)cols * h * 3, 0);
		mask.assign((size_t)cols * h, 0);
		rowMask.assign((size_t)h, 0);
		for (int y = 0; y < h; ++y) {
			const Byte* row = bgra + (size_t)y * rowStride;
			Byte* lumRow = lum.data() + (size_t)y * cols;
			Byte* rgbRow = rgb.data() + (size_t)y * cols * 3;
			for (int c = 0; c < cols; ++c) {
				int x = c * step + step / 2;
				if (x >= width) x = width - 1;
				const Byte* p = row + (size_t)x * 4;
				const int b = p[0], g = p[1], r = p[2];
				lumRow[c] = (Byte)((r * 77 + g * 150 + b * 29) >> 8);
				rgbRow[(size_t)c * 3] = (Byte)r;
				rgbRow[(size_t)c * 3 + 1] = (Byte)g;
				rgbRow[(size_t)c * 3 + 2] = (Byte)b;
			}
		}
	}

	bool sameAs(const FrameProfile& o) const
	{
		if (o.h != h || o.cols != cols) return false;
		if (memcmp(lum.data(), o.lum.data(), lum.size()) != 0) return false;
		return memcmp(rgb.data(), o.rgb.data(), rgb.size()) == 0;
	}

	// 标记“这个抽样点在两帧的同一位置变了没有”，并统计每行的掩码点数。
	void markChanged(const FrameProfile& prev)
	{
		if (prev.h != h || prev.cols != cols || prev.lum.size() != lum.size()) {
			std::fill(mask.begin(), mask.end(), (Byte)1);
			std::fill(rowMask.begin(), rowMask.end(), cols);
			return;
		}
		for (int y = 0; y < h; ++y) {
			const Byte* la = lum.data() + (size_t)y * cols;
			const Byte* lb = prev.lum.data() + (size_t)y * cols;
			const Byte* ra = rgb.data() + (size_t)y * cols * 3;
			const Byte* rb = prev.rgb.data() + (size_t)y * cols * 3;
			Byte* mrow = mask.data() + (size_t)y * cols;
			int n = 0;
			for (int c = 0; c < cols; ++c) {
				const int dl = std::abs((int)la[c] - (int)lb[c]);
				const int i = c * 3;
				const int dr = std::abs((int)ra[i] - (int)rb[i])
					+ std::abs((int)ra[i + 1] - (int)rb[i + 1])
					+ std::abs((int)ra[i + 2] - (int)rb[i + 2]);
				const Byte m = (dl > 4 || dr > 12) ? (Byte)1 : (Byte)0;
				mrow[c] = m;
				n += m;
			}
			rowMask[(size_t)y] = n;
		}
	}
};

// 与上一帧逐像素相同的顶部/底部行游程。只比较内容列：最外 1 像素是窗口边框，
// 右侧一段是滚动条，它们会各自独立重绘，不能参与“这一行有没有变”的判定。
inline int identicalEdgeRun(const Byte* a, const Byte* b, int w, int h, int stride, bool fromBottom)
{
	if (!a || !b || w < 8 || h < 4) return 0;
	const int rightIgnore = std::clamp(w / 40, 16, 40);
	const int x0 = 1;
	const int x1 = std::max(x0 + 8, w - rightIgnore);
	const size_t bytes = (size_t)(x1 - x0) * 4;
	int run = 0;
	for (int i = 0; i < h; ++i) {
		const int y = fromBottom ? (h - 1 - i) : i;
		const Byte* ra = a + (size_t)y * stride + (size_t)x0 * 4;
		const Byte* rb = b + (size_t)y * stride + (size_t)x0 * 4;
		if (memcmp(ra, rb, bytes) != 0) break;
		++run;
	}
	return run;
}

// 位移估计：两帧之间内容整体移动了多少行。它只服务“预测”，最终落点由画布裁定，
// 所以这里的取舍偏保守：拿不准就报“不可信”，让画布来找。
class Matcher
{
public:
	static constexpr int kTrustedPercent = 75;  // 超过帧高 3/4 的位移不再作为预测
	static constexpr int kSearchPercent = 80;
	static constexpr int kMatchTol = 6;         // 灰度差 <= 6 视为“同一内容”
	static constexpr double kAcceptBad = 0.55;  // 变过的像素里最多允许这么多对不上
	static constexpr double kSecondGap = 0.12;  // 次优候选必须差这么多，否则算歧义
	static constexpr int kMinOverlapRows = 24;

	// prev：上一张可信帧；cur：新帧（掩码由 cur.markChanged(prev) 得到，两帧对称）
	Step match(const FrameProfile& prev, const Byte* prevPix,
		const FrameProfile& cur, const Byte* curPix, int hint = 0, bool haveHint = false) const
	{
		Step st;
		const int h = prev.h;
		const int cols = prev.cols;
		if (h < 24 || cols < 8 || cur.h != h || cur.cols != cols) return st;

		st.staticTop = identicalEdgeRun(prevPix, curPix, prev.w, h, prev.stride, false);
		st.staticBottom = identicalEdgeRun(prevPix, curPix, prev.w, h, prev.stride, true);

		// 掩码点按行建索引（CSR）：后面只遍历这些点，与画面宽度无关
		std::vector<int> maskIdx;
		std::vector<int> rowBegin((size_t)h + 1, 0);
		maskIdx.reserve((size_t)cols * h / 4);
		int activeRows = 0;
		for (int r = 0; r < h; ++r) {
			rowBegin[(size_t)r] = (int)maskIdx.size();
			const Byte* mrow = cur.mask.data() + (size_t)r * cols;
			int n = 0;
			for (int c = 0; c < cols; ++c) {
				if (!mrow[c]) continue;
				maskIdx.push_back(c);
				++n;
			}
			if (n > 0) ++activeRows;
		}
		rowBegin[(size_t)h] = (int)maskIdx.size();
		const int maskedPixels = (int)maskIdx.size();
		if (maskedPixels == 0) return st;
		if (activeRows < std::max(12, h / 16) || maskedPixels < 400) return st;

		st.topIsBar = edgeIsBar(cur, st.staticTop, true);
		st.bottomIsBar = edgeIsBar(cur, st.staticBottom, false);

		// 固定在画面上下沿的栏（吸顶/吸底）不是文档内容，出现在任一帧的这个行区间里
		// 就不能参与比较，否则“内容行 vs 固定栏行”会凭空产生巨大差异。
		const int skipTop = st.staticTop;
		const int skipBottom = st.staticBottom;

		const int minOverlap = std::max(kMinOverlapRows, h / 6);
		const int maxShift = std::max(8, h * kSearchPercent / 100);

		const MaskIndex mi{ maskIdx, rowBegin };

		// 粗搜用的稀疏采样：每行最多取若干个掩码点
		struct Sample { int r; int c; Byte v; };
		std::vector<Sample> samples;
		samples.reserve(4096);
		const int rowStep = std::max(1, h / 180);
		for (int r = 0; r < h; r += rowStep) {
			if (r < skipTop || r >= h - skipBottom) continue;
			const Byte* lumRow = prev.lum.data() + (size_t)r * cols;
			const int b = rowBegin[(size_t)r];
			const int e = rowBegin[(size_t)r + 1];
			if (e <= b) continue;
			const int stride = std::max(1, (e - b) / 12);
			for (int i = b; i < e && (int)samples.size() < 4096; i += stride)
				samples.push_back(Sample{ r, maskIdx[(size_t)i], lumRow[maskIdx[(size_t)i]] });
			if ((int)samples.size() >= 4096) break;
		}
		if (samples.size() < 64) return st;

		auto coarseBad = [&](int d) -> double {
			int matched = 0;
			int total = 0;
			for (const auto& s : samples) {
				const int rr = s.r + d;
				if (rr < skipTop || rr >= h - skipBottom) continue;
				const int diff = std::abs((int)s.v - (int)cur.lum[(size_t)rr * cols + s.c]);
				if (diff <= kMatchTol) ++matched;
				++total;
			}
			if (total < std::max(48, (int)samples.size() / 6)) return 1e9;
			return 1.0 - (double)matched / total;
		};

		struct Peak { int d; double bad; };
		std::vector<Peak> peaks;
		peaks.reserve(256);
		for (int d = -maxShift; d <= maxShift; ++d) {
			const double s = coarseBad(d);
			if (s < 1e8) peaks.push_back(Peak{ d, s });
		}
		if (peaks.empty()) return st;
		// 等间距滚动时，重复的行/图案会造出若干几乎一样好的候选。给“离上一次位移远”
		// 的候选一点惩罚，让靠近预测的排在前面；惩罚很小，真跳变时仍能胜出。
		const double hintScale = std::max(8.0, (double)h / 8.0);
		auto rank = [&](int d, double bad) {
			if (!haveHint) return bad;
			const double dev = std::min(1.0, std::abs(d - hint) / hintScale);
			return bad + 0.15 * dev;
		};
		std::sort(peaks.begin(), peaks.end(), [&](const Peak& a, const Peak& b) {
			const double ra = rank(a.d, a.bad);
			const double rb = rank(b.d, b.bad);
			if (ra != rb) return ra < rb;
			return std::abs(a.d) < std::abs(b.d);
		});

		std::vector<int> cands{ 0 };
		std::vector<int> centers;
		int picked = 0;
		for (const auto& p : peaks) {
			bool separate = true;
			for (int c : centers) {
				if (std::abs(c - p.d) < 8) { separate = false; break; }
			}
			if (!separate) continue;
			centers.push_back(p.d);
			for (int k = -2; k <= 2; ++k) cands.push_back(p.d + k);
			if (++picked >= 2) break;
		}
		std::sort(cands.begin(), cands.end());
		cands.erase(std::unique(cands.begin(), cands.end()), cands.end());

		int bestD = 0;
		double bestBad = 1e9;
		int secondD = 0;
		double secondBad = 1e9;
		for (int d : cands) {
			if (std::abs(d) > maxShift) continue;
			const double c = fineBad(prev, cur, mi, d, minOverlap, skipTop, skipBottom);
			if (c >= 1e8) continue;
			// 差值在千分之几以内视为平手，这时优先靠预测近的
			if (c < bestBad - 0.003 || (c <= bestBad + 0.003 && rank(d, c) < rank(bestD, bestBad))) {
				bestBad = std::min(bestBad, c);
				bestD = d;
			}
		}
		if (bestBad > 1e8) return st;
		for (int d : cands) {
			if (std::abs(d - bestD) < 6) continue;
			const double c = fineBad(prev, cur, mi, d, minOverlap, skipTop, skipBottom);
			if (c < secondBad) {
				secondBad = c;
				secondD = d;
			}
		}
		if (bestD == 0) return st; // 没有位移：画面在动但没有整体滚动
		if (bestBad > kAcceptBad) return st;
		if (secondBad < bestBad + kSecondGap && std::abs(secondD - bestD) >= 8) return st;
		if (std::abs(bestD) > h * kTrustedPercent / 100) {
			st.outOfRange = true;
			st.shift = bestD;
			return st;
		}
		st.shift = bestD;
		st.confident = true;
		return st;
	}

private:
	// 掩码点的行索引：rowBegin[r] ~ rowBegin[r+1] 是第 r 行被标记的列号区间
	struct MaskIndex
	{
		const std::vector<int>& cols;
		const std::vector<int>& rowBegin;
	};

	static double fineBad(const FrameProfile& prev, const FrameProfile& cur, const MaskIndex& mi,
		int d, int minOverlap, int skipTop, int skipBottom)
	{
		const int h = prev.h;
		const int cols = prev.cols;
		if (std::abs(d) >= h) return 1e9;
		const int r0 = std::max(0, -d);
		const int r1 = std::min(h, h - d);
		if (r1 - r0 < minOverlap) return 1e9;
		struct RowStat { int n; int miss; };
		std::vector<RowStat> rows;
		rows.reserve((size_t)(r1 - r0));
		for (int r = r0; r < r1; ++r) {
			if (r < skipTop || r >= h - skipBottom) continue;
			const int rr = r + d;
			if (rr < skipTop || rr >= h - skipBottom) continue;
			const int b = mi.rowBegin[(size_t)r];
			const int e = mi.rowBegin[(size_t)r + 1];
			if (e <= b) continue;
			const Byte* a = prev.lum.data() + (size_t)r * cols;
			const Byte* c = cur.lum.data() + (size_t)rr * cols;
			int miss = 0;
			for (int i = b; i < e; ++i) {
				const int col = mi.cols[(size_t)i];
				if (std::abs((int)a[col] - (int)c[col]) > kMatchTol) ++miss;
			}
			rows.push_back(RowStat{ e - b, miss });
		}
		if ((int)rows.size() < std::max(8, minOverlap / 2)) return 1e9;
		// 去掉最差 10% 的行（局部动画/悬浮元素），剩下的按点数加权
		std::sort(rows.begin(), rows.end(), [](const RowStat& a, const RowStat& b) {
			return (double)a.miss / a.n < (double)b.miss / b.n;
		});
		const size_t keep = std::max<size_t>(1, rows.size() * 9 / 10);
		long long total = 0;
		long long miss = 0;
		for (size_t i = 0; i < keep; ++i) {
			total += rows[i].n;
			miss += rows[i].miss;
		}
		if (total == 0) return 1e9;
		return (double)miss / (double)total;
	}

	// 静态带是不是“固定栏”：要有横向纹理（整片空白不算），且紧贴它内侧的内容确实动过
	static bool edgeIsBar(const FrameProfile& cur, int run, bool fromTop)
	{
		const int h = cur.h;
		if (run < 6 || run >= h * 9 / 10) return false;
		const int step = std::max(1, run / 8);
		double tex = 0.0;
		int n = 0;
		for (int i = 0; i < run; i += step) {
			const int y = fromTop ? i : (h - 1 - i);
			const Byte* row = cur.lum.data() + (size_t)y * cur.cols;
			int mn = 255, mx = 0;
			for (int c = 0; c < cur.cols; ++c) {
				mn = std::min(mn, (int)row[c]);
				mx = std::max(mx, (int)row[c]);
			}
			tex += (mx - mn);
			++n;
		}
		if (n == 0 || tex / n < 8.0) return false;
		int insideChanged = 0;
		for (int i = 0; i < 4; ++i) {
			const int y = fromTop ? (run + i) : (h - 1 - run - i - 1);
			if (y < 0 || y >= h) continue;
			insideChanged += (cur.rowMask[(size_t)y] > 0) ? 1 : 0;
		}
		return insideChanged > 0;
	}
};

// 长图拼装：画布始终是一段连续的文档像素，画布行号就是文档行号。
//   pos       当前帧第 0 行落在画布里的位置
//   canvasH   画布高度，同时也是“曾经到过的最深位置”（高水位）
class Stitcher
{
public:
	bool begin(const Byte* frame, int w, int h, int stride, int maxHeight, size_t maxBytes)
	{
		reset();
		if (!frame || w < 8 || h < 24) return false;
		w_ = w;
		h_ = h;
		rowBytes_ = (size_t)w * 4;
		maxHeight_ = std::max(h, maxHeight);
		maxBytes_ = std::max<size_t>((size_t)h * rowBytes_, maxBytes);
		canvas_.assign((size_t)h * rowBytes_, 0);
		for (int y = 0; y < h; ++y)
			memcpy(canvas_.data() + (size_t)y * rowBytes_, frame + (size_t)y * stride, rowBytes_);
		canvasH_ = h;
		pos_ = 0;
		predicted_ = 0;
		prev_.build(frame, w, h, stride);
		prevPixels_.assign((size_t)h * rowBytes_, 0);
		for (int y = 0; y < h; ++y)
			memcpy(prevPixels_.data() + (size_t)y * rowBytes_, frame + (size_t)y * stride, rowBytes_);
		started_ = true;
		return true;
	}

	void reset()
	{
		started_ = false;
		canvas_.clear();
		prevPixels_.clear();
		prev_ = FrameProfile{};
		canvasH_ = 0;
		pos_ = 0;
		predicted_ = 0;
		fixedTop_ = fixedBottom_ = 0;
		candTop_ = candBottom_ = 0;
		topHits_ = bottomHits_ = 0;
		topMiss_ = bottomMiss_ = 0;
		lastShift_ = 0;
		haveShift_ = false;
		tooLong_ = false;
		lostMotion_ = 0;
	}

	bool ready() const { return started_; }
	int width() const { return w_; }
	int height() const { return canvasH_; }
	int viewportTop() const { return pos_; }
	int contentTop() const { return fixedTop_; }
	int contentBottom() const { return std::max(fixedTop_ + 8, h_ - fixedBottom_); }
	const Byte* data() const { return canvas_.empty() ? nullptr : canvas_.data(); }
	size_t bytes() const { return canvas_.size(); }
	bool tooLong() const { return tooLong_; }
	int stride() const { return w_ * 4; }
	int maxHeight() const { return maxHeight_; }

	// 推入一帧。落点由画布裁定（见 locate）；找不到可靠落点的帧不写画布，
	// 但位移预测继续推进，下一帧仍然能接上。
	Stitched push(const Byte* frame, int stride)
	{
		Stitched out;
		if (!started_ || !frame) return out;
		if (tooLong_) {
			out.tooLong = true;
			return out;
		}
		FrameProfile cur;
		cur.build(frame, w_, h_, stride);
		if (cur.sameAs(prev_)) return out; // 页面没动
		cur.markChanged(prev_);

		const Step step = matcher_.match(prev_, prevPixels_.data(), cur, frame, lastShift_, haveShift_);
		const bool trusted = step.confident && !step.outOfRange;
		// “画面在滚动”这个判断要包含“滚太快、超出可拼接范围”的情况：
		// 那种帧虽然不采纳，但它确实证明了页面在动（用来提示用户慢一点）。
		out.sawMotion = step.shift != 0;
		if (trusted) predicted_ -= step.shift;

		// 预测可信时窗口可以窄（预测通常只差几行）；拿不准就把窗口放大，让画布自己找。
		const int win = trusted ? std::max(32, h_ / 24) : std::max(96, h_ / 2);
		Placed placed = locate(frame, stride, cur, predicted_, win);
		if (!placed.ok) {
			// 画布上没有可信落点。一直拼不上但画面确实在动，说明画布落后了
			// （比如用户按了一次翻页键）：按“接在画布外侧”继续，宁可留一段空隙，
			// 也不要一直停在原地。
			if (out.sawMotion) ++lostMotion_;
			if (lostMotion_ >= kRelocalize) {
				lostMotion_ = 0;
				placed.ok = true;
				placed.top = geometricTop(canvasH_);
			}
		}
		if (!placed.ok) {
			// 这一帧不写，但预测与基准照常前进，下一次滚动仍然能接上。
			setBaseline(std::move(cur), frame, stride);
			return out;
		}
		lostMotion_ = 0;
		out.accepted = true;
		const int oldPos = pos_;
		pos_ = placed.top;
		out.shift = oldPos - pos_;
		if (out.shift != 0) {
			updateBands(step);
			if (pos_ + contentBottom() > canvasH_) growDown(frame, stride, out);
			else if (pos_ + contentTop() < 0) growUp(frame, stride, out);
			if (tooLong_) out.tooLong = true;
		}
		// 向上补图会让画布坐标整体平移，预测要按最终位置重算
		predicted_ = pos_;
		lastShift_ = out.shift;
		haveShift_ = true;
		setBaseline(std::move(cur), frame, stride);
		return out;
	}

	// 页面停稳后补抓的清晰帧：直接把画布底部这段换成新抓的像素，
	// 去掉快滚过程中留下的运动模糊。先在 ±2 行内确认这一帧确实和画布底部对得上。
	bool refreshTail(const Byte* frame, int stride)
	{
		if (!started_ || !frame || canvasH_ < 24) return false;
		const int ct = contentTop();
		const int cb = contentBottom();
		const int sampleRows = std::min(std::min(cb - ct, h_), std::min(canvasH_, 240));
		if (sampleRows < 8 || w_ <= 8) return false;
		const int dstStart = canvasH_ - sampleRows;

		auto rowBad = [&](int s) -> double {
			const int base = pos_ - s;
			const int srcStart = dstStart - base;
			if (srcStart < ct || srcStart + sampleRows > cb) return 1e9;
			int matched = 0;
			int total = 0;
			for (int y = 0; y < sampleRows; y += 2) {
				const Byte* a = canvas_.data() + (size_t)(dstStart + y) * rowBytes_;
				const Byte* b = frame + (size_t)(srcStart + y) * (size_t)stride;
				for (int x = 2; x < w_ - 2; x += 7) {
					const size_t o = (size_t)x * 4;
					const int diff = std::abs((int)a[o] - (int)b[o])
						+ std::abs((int)a[o + 1] - (int)b[o + 1])
						+ std::abs((int)a[o + 2] - (int)b[o + 2]);
					if (diff <= 24) ++matched;
					++total;
				}
			}
			if (total == 0) return 1e9;
			return 1.0 - (double)matched / total;
		};
		int bestShift = 0;
		double bestBad = 1e9;
		for (int s = -2; s <= 2; ++s) {
			const double b = rowBad(s);
			if (b < bestBad) {
				bestBad = b;
				bestShift = s;
			}
		}
		if (bestBad > 0.25) return false;
		const int base = pos_ - bestShift;
		const int dstEnd = std::min(canvasH_, base + cb);
		const int start = std::max(0, dstEnd - (cb - ct));
		const int count = dstEnd - start;
		const int srcStart = start - base;
		if (count < 8 || srcStart < ct || srcStart + count > cb) return false;
		memcpy(canvas_.data() + (size_t)start * rowBytes_,
			frame + (size_t)srcStart * (size_t)stride, (size_t)count * rowBytes_);
		return true;
	}

private:
	// —— 落点判定 ——
	// 判据只有两条：和画布吻合（对不上的比例不高于 kPlaceBadCap），且位置唯一
	// （离得远的次优必须明显更差）。等分候选都挤在预测附近时，交给连续性决定。
	struct Placed
	{
		bool ok{ false };
		int top{ 0 };
	};

	static constexpr int kAnchorTol = 12;       // 比帧间比较宽松：容忍快滚帧的运动模糊
	static constexpr double kPlaceBadCap = 0.55;// 连这个都达不到，说明画布上根本没有它
	static constexpr double kPlaceGap = 0.06;   // 唯一性：远处的次优至少要差这么多
	static constexpr double kTieEps = 0.02;     // 视为“一样好”的容差
	static constexpr int kTieSpan = 40;         // 等分候选彼此超过这么多行就不敢选
	// 连续这么多帧“画面在动、画布上却没有它的位置”，就认定画布落后了，按接续处理
	static constexpr int kRelocalize = 10;

	// 帧放在 frameTop 处时，掩码像素和画布对不上的比例（0=全对上，1e9=没有可比区域）。
	// 除了像素比例，还要求“可比较的行数”足够多：只重叠十来行时，几行颜色接近就能
	// 凑出很高的吻合度，这种位置不能信。
	double canvasBadAt(const Byte* frame, int stride, const FrameProfile& cur, int frameTop,
		int rowStep, int colStep) const
	{
		int matched = 0;
		int total = 0;
		int rows = 0;
		for (int r = 0; r < h_; r += rowStep) {
			const int cy = frameTop + r;
			if (cy < 0 || cy >= canvasH_) continue;
			if (cur.rowMask[(size_t)r] == 0) continue;
			const Byte* fr = frame + (size_t)r * (size_t)stride;
			const Byte* cv = canvas_.data() + (size_t)cy * rowBytes_;
			const Byte* mrow = cur.mask.data() + (size_t)r * cur.cols;
			bool rowUsed = false;
			for (int c = 0; c < cur.cols; c += colStep) {
				if (!mrow[c]) continue;
				const size_t o = (size_t)cur.sampleX(c) * 4;
				const int lf = (fr[o + 2] * 77 + fr[o + 1] * 150 + fr[o] * 29) >> 8;
				const int lc = (cv[o + 2] * 77 + cv[o + 1] * 150 + cv[o] * 29) >> 8;
				if (std::abs(lf - lc) <= kAnchorTol) ++matched;
				++total;
				rowUsed = true;
			}
			if (rowUsed) ++rows;
		}
		// 抽样步长越大，要求的行数/点数按比例放宽，但“覆盖的纵向范围”要求不变
		const int needRows = std::max(6, std::max(16, h_ / 8) / std::max(1, rowStep));
		const int needTotal = std::max(48, 200 / std::max(1, rowStep * colStep));
		if (total < needTotal || rows < needRows) return 1e9;
		return 1.0 - (double)matched / total;
	}

	// 帧和画布几乎没有重叠时，内容只能接在画布外侧：上沿或下沿
	int geometricTop(int predicted) const
	{
		const bool appendSide = predicted + h_ / 2 >= canvasH_ / 2;
		return appendSide ? (canvasH_ - contentTop()) : -contentBottom();
	}

	Placed locate(const Byte* frame, int stride, const FrameProfile& cur, int predicted, int win) const
	{
		Placed out;
		const int minSpan = std::max(48, h_ / 6);
		const int overlapRows = std::min(canvasH_, predicted + h_) - std::max(0, predicted);
		// 重叠不足以验证：内容只能接在画布外侧（按几何接续）
		if (overlapRows < std::max(minSpan, h_ / 4)) {
			out.ok = true;
			out.top = geometricTop(predicted);
			return out;
		}
		// 先在小窗口里找（预测通常只差几行，便宜），不行再放宽
		out = scanAround(frame, stride, cur, predicted, win);
		if (!out.ok) out = scanAround(frame, stride, cur, predicted, std::max(96, h_ / 2));
		return out;
	}

	// 在 predicted ± win 范围内扫描候选位置并做唯一性判定
	Placed scanAround(const Byte* frame, int stride, const FrameProfile& cur, int predicted, int win) const
	{
		Placed out;
		const int minSpan = std::max(48, h_ / 6);
		const int lo = std::max(-h_ + minSpan, predicted - win);
		const int hi = std::min(canvasH_ - minSpan, predicted + win);
		if (hi < lo) return out;
		const int step = std::max(2, (hi - lo) / 100);
		struct Cand { int top; double bad; };
		std::vector<Cand> scan;
		scan.reserve((size_t)((hi - lo) / step + 1));
		for (int top = lo; top <= hi; top += step) {
			const double bad = canvasBadAt(frame, stride, cur, top, 4, 4);
			if (bad < 1e8) scan.push_back(Cand{ top, bad });
		}
		if (scan.empty()) {
			// 有重叠但一行都没法比较（掩码覆盖的纵向范围太窄）：不猜
			return out;
		}
		size_t bi = 0;
		for (size_t i = 1; i < scan.size(); ++i)
			if (scan[i].bad < scan[bi].bad) bi = i;
		int bestTop = scan[bi].top;
		double bestBad = scan[bi].bad;
		// 逐行确认最优位置（粗搜的步长可能跨过真正的位置）
		for (int top = std::max(lo, bestTop - step); top <= std::min(hi, bestTop + step); ++top) {
			const double bad = canvasBadAt(frame, stride, cur, top, 1, 2);
			if (bad < bestBad) {
				bestBad = bad;
				bestTop = top;
			}
		}
		if (bestBad > kPlaceBadCap) {
			// 画布上根本没有这段内容（大面积遮挡/只换了画面的一部分）：这一帧不用。
			return out;
		}
		// 唯一性：离得远的候选必须明显更差
		double altBad = 1e9;
		for (const auto& s : scan) {
			if (std::abs(s.top - bestTop) < 8) continue;
			altBad = std::min(altBad, s.bad);
		}
		if (altBad >= bestBad + kPlaceGap || altBad >= 1e8) {
			out.ok = true;
			out.top = bestTop;
			return out;
		}
		// 有和最优一样好的候选：滚动是连续的，选离预测最近的那个；
		// 但如果这些候选彼此隔得很远（重复内容），就不猜，交给下一帧。
		int tieTop = bestTop;
		int tieDist = std::abs(bestTop - predicted);
		bool farTie = false;
		for (const auto& s : scan) {
			if (s.bad > bestBad + kTieEps) continue;
			const int d = std::abs(s.top - predicted);
			if (d < tieDist) {
				tieDist = d;
				tieTop = s.top;
			}
		}
		for (const auto& s : scan) {
			if (s.bad > bestBad + kTieEps) continue;
			if (std::abs(s.top - tieTop) > kTieSpan) farTie = true;
		}
		if (!farTie && tieDist <= std::max(48, h_ / 6)) {
			out.ok = true;
			out.top = tieTop;
			return out;
		}
		// 重复内容且彼此隔得远：谁都不能信，这一帧不用，等下一帧。
		return out;
	}

	void setBaseline(FrameProfile&& cur, const Byte* frame, int stride)
	{
		prev_ = std::move(cur);
		prevPixels_.resize((size_t)h_ * rowBytes_);
		for (int y = 0; y < h_; ++y)
			memcpy(prevPixels_.data() + (size_t)y * rowBytes_, frame + (size_t)y * stride, rowBytes_);
	}

	static int spliceCrop(int contentH, int grow)
	{
		const int quarter = (contentH + 3) / 4;
		return std::clamp(std::max(contentH / 2, quarter + grow), 1, contentH);
	}

	bool ensureRoom(int rows) const
	{
		if (rows <= 0) return false;
		if (canvasH_ + rows > maxHeight_) return false;
		if ((size_t)(canvasH_ + rows) * rowBytes_ > maxBytes_) return false;
		return true;
	}

	// 覆盖式追加：条带比实际增长更高，多出来的部分覆盖到已有画布行上，
	// 最新抓到的像素赢，接缝自然消失。
	void growDown(const Byte* frame, int stride, Stitched& out)
	{
		const int ct = contentTop();
		const int cb = contentBottom();
		const int contentH = cb - ct;
		const int grow = std::min(pos_ + cb - canvasH_, contentH);
		if (grow <= 0) return;
		const int crop = spliceCrop(contentH, grow);
		const int overlap = std::clamp(crop - grow, 0, std::max(0, canvasH_ - 1));
		const int take = overlap + grow;
		const int srcTop = std::max(ct, cb - take);
		const int actual = cb - srcTop;
		if (actual <= 0) return;
		if (!ensureRoom(actual - overlap)) {
			tooLong_ = true;
			return;
		}
		if (overlap > 0) trimBottom(overlap);
		canvas_.insert(canvas_.end(),
			frame + (size_t)srcTop * (size_t)stride,
			frame + (size_t)(srcTop + actual) * (size_t)stride);
		canvasH_ += actual;
		out.grew = true;
		out.fromTop = false;
	}

	void growUp(const Byte* frame, int stride, Stitched& out)
	{
		const int ct = contentTop();
		const int cb = contentBottom();
		const int contentH = cb - ct;
		const int grow = std::min(-(pos_ + ct), contentH);
		if (grow <= 0) return;
		const int crop = spliceCrop(contentH, grow);
		const int overlap = std::clamp(crop - grow, 0, std::max(0, canvasH_ - 1));
		const int take = overlap + grow;
		const int srcEnd = std::min(cb, ct + take);
		const int actual = srcEnd - ct;
		if (actual <= 0) return;
		if (!ensureRoom(actual - overlap)) {
			tooLong_ = true;
			return;
		}
		if (overlap > 0) trimTop(overlap);
		canvas_.insert(canvas_.begin(),
			frame + (size_t)ct * (size_t)stride,
			frame + (size_t)(ct + actual) * (size_t)stride);
		canvasH_ += actual;
		pos_ += actual - overlap;
		out.grew = true;
		out.fromTop = true;
	}

	void trimBottom(int rows)
	{
		rows = std::min(rows, canvasH_ - 1);
		if (rows <= 0) return;
		canvas_.resize(canvas_.size() - (size_t)rows * rowBytes_);
		canvasH_ -= rows;
	}

	void trimTop(int rows)
	{
		rows = std::min(rows, canvasH_ - 1);
		if (rows <= 0) return;
		canvas_.erase(canvas_.begin(), canvas_.begin() + (size_t)rows * rowBytes_);
		canvasH_ -= rows;
	}

	// 固定栏（吸顶/吸底）识别：连续多帧在同一侧出现同样高度、有纹理、内侧内容确实动过的
	// 静态带才认定为固定栏。只影响“拼接时从哪取新行”，不改变已有画面。
	void updateBands(const Step& s)
	{
		auto upd = [&](int run, bool isBar, int& fixed, int& cand, int& hits, int& miss) {
			const int cap = std::max(8, h_ * 2 / 5);
			if (run >= 6 && isBar && run <= cap) {
				if (std::abs(run - cand) <= 3) ++hits;
				else {
					cand = run;
					hits = 1;
				}
				miss = 0;
				if (fixed == 0) {
					if (hits >= 2 || run >= 24) fixed = cand;
				}
				else if (hits >= 2 && std::abs(cand - fixed) > 2) {
					fixed = cand;
				}
			}
			else {
				hits = 0;
				if (fixed > 0 && run >= fixed - 6) {
					miss = 0;
					return;
				}
				if (++miss >= 3) {
					fixed = 0;
					cand = 0;
					hits = 0;
					miss = 0;
				}
			}
		};
		upd(s.staticTop, s.topIsBar, fixedTop_, candTop_, topHits_, topMiss_);
		upd(s.staticBottom, s.bottomIsBar, fixedBottom_, candBottom_, bottomHits_, bottomMiss_);
		if (fixedTop_ + fixedBottom_ > h_ * 3 / 5) {
			fixedTop_ = fixedBottom_ = 0;
			candTop_ = candBottom_ = 0;
			topHits_ = bottomHits_ = 0;
		}
	}

	Matcher matcher_;
	FrameProfile prev_;
	std::vector<Byte> prevPixels_;
	std::vector<Byte> canvas_;
	int w_{ 0 }, h_{ 0 };
	size_t rowBytes_{ 0 };
	int canvasH_{ 0 };
	int pos_{ 0 };
	int predicted_{ 0 };   // 下一帧应该在画布上的位置（由位移估计推进）
	int maxHeight_{ 36000 };
	size_t maxBytes_{ 256ull << 20 };
	bool started_{ false };
	bool tooLong_{ false };
	int fixedTop_{ 0 }, fixedBottom_{ 0 };
	int candTop_{ 0 }, candBottom_{ 0 };
	int topHits_{ 0 }, bottomHits_{ 0 };
	int topMiss_{ 0 }, bottomMiss_{ 0 };
	int lastShift_{ 0 };
	bool haveShift_{ false };
	int lostMotion_{ 0 };  // 连续“在动但拼不上”的帧数：用来判断画布是否落后
};

} // namespace LongStitch
