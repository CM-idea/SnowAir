#include "pch.h"
#include "SelfTest.h"
#include "BlacklistMatch.h"
#include "ConfigMerge.h"
#include "Shape/RoundBox.h"
#include "Shape/LinkGeom.h"
#include "Shape/ShapeBase.h"
#include "Shape/ArrowPoly.h"
#include "Win/PixLens.h"
#include "Win/CursorImage.h"
#include "Win/LongMatch.h"
#include <cstdio>

namespace
{
	FILE* g_log{ nullptr };
	int g_fail{ 0 };

	void check(bool ok, const wchar_t* name)
	{
		if (!ok) ++g_fail;
		if (g_log) fwprintf(g_log, L"[%s] %s\n", ok ? L"PASS" : L"FAIL", name);
	}

	bool approx(float a, float b, float eps = 0.001f)
	{
		return std::fabs(a - b) <= eps;
	}

	// 三次贝塞尔在参数 t 处的有符号曲率
	double penCurvature(const ArrowPolyState::PenSeg& s, float t)
	{
		if (s.line) return 0.0;
		const double u = 1.0 - t;
		const double d1x = 3.0 * (u * u * (s.c1.x - s.p0.x) + 2.0 * u * t * (s.c2.x - s.c1.x) + t * t * (s.p1.x - s.c2.x));
		const double d1y = 3.0 * (u * u * (s.c1.y - s.p0.y) + 2.0 * u * t * (s.c2.y - s.c1.y) + t * t * (s.p1.y - s.c2.y));
		const double d2x = 6.0 * (u * (s.c2.x - 2.0 * s.c1.x + s.p0.x) + t * (s.p1.x - 2.0 * s.c2.x + s.c1.x));
		const double d2y = 6.0 * (u * (s.c2.y - 2.0 * s.c1.y + s.p0.y) + t * (s.p1.y - 2.0 * s.c2.y + s.c1.y));
		const double speed = std::hypot(d1x, d1y);
		if (speed < 1e-9) return 0.0;
		return (d1x * d2y - d1y * d2x) / (speed * speed * speed);
	}

	double bezX(const ArrowPolyState::PenSeg& s, float t)
	{
		const double u = 1.0 - t;
		return s.p0.x * u * u * u + 3.0 * s.c1.x * u * u * t + 3.0 * s.c2.x * u * t * t + s.p1.x * t * t * t;
	}
	double bezY(const ArrowPolyState::PenSeg& s, float t)
	{
		const double u = 1.0 - t;
		return s.p0.y * u * u * u + 3.0 * s.c1.y * u * u * t + 3.0 * s.c2.y * u * t * t + s.p1.y * t * t * t;
	}
}

int RunSelfTest()
{
	wchar_t exe[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir(exe);
	const auto slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	_wfopen_s(&g_log, (dir + L"SnowAir-selftest.log").c_str(), L"w, ccs=UTF-8");

	// —— RoundBox ——
	const D2D1_RECT_F r10x4{ 0.f, 0.f, 10.f, 4.f };
	check(approx(RoundBox::maxRadius(r10x4), 2.f), L"RoundBox::maxRadius(10x4) == 2");

	float radii[4]{ 100.f, 100.f, 100.f, 100.f };
	float clamped[4]{};
	RoundBox::clamp(r10x4, radii, clamped);
	check(clamped[0] <= 2.f && clamped[1] <= 2.f && clamped[2] <= 2.f && clamped[3] <= 2.f,
		L"RoundBox::clamp 半径不超过短边一半");

	float squareRadii[4]{ 0.f, 0.f, 0.f, 0.f };
	check(RoundBox::isSquare(squareRadii), L"RoundBox::isSquare 全零为直角");
	float some[4]{ 2.f, 0.f, 0.f, 0.f };
	check(!RoundBox::isSquare(some), L"RoundBox::isSquare 非零不为直角");

	// —— LinkGeom：轴线交点 ——
	{
		float none[4]{ 0.f, 0.f, 0.f, 0.f };
		D2D1_POINT_2F p{};
		const bool ok = LinkGeom::edgeOnRect(5.f, 3.f, none, 0.f, p);
		check(ok && approx(p.x, 5.f) && approx(p.y, 0.f), L"LinkGeom::edgeOnRect 0° 交于右中点");
		D2D1_POINT_2F e{};
		check(LinkGeom::edgeOnEllipse(5.f, 3.f, 0.f, e) && approx(e.x, 5.f),
			L"LinkGeom::edgeOnEllipse 0° 交于右端点");
	}
	check(LinkGeom::insideRect(r10x4, 5.f, 2.f), L"LinkGeom::insideRect 内部为真");
	check(!LinkGeom::insideRect(r10x4, 11.f, 2.f), L"LinkGeom::insideRect 外部为假");
	{
		const D2D1_POINT_2F c{ 0.f, 0.f };
		check(LinkGeom::insideEllipse(c, 5.f, 3.f, 4.f, 0.f), L"LinkGeom::insideEllipse 内部为真");
		check(!LinkGeom::insideEllipse(c, 5.f, 3.f, 0.f, 3.5f), L"LinkGeom::insideEllipse 外部为假");
	}

	// —— 颜色打包 ——
	check(ShapeBase::packColor(D2D1::ColorF(1.f, 0.5f, 0.f, 0.25f)) == 0xFF800040u,
		L"ShapeBase::packColor 1/0.5/0/0.25 -> 0xFF800040");
	check(ShapeBase::packColor(D2D1::ColorF(0.f, 0.f, 0.f, 0.f)) == 0x00000000u,
		L"ShapeBase::packColor 全零 -> 0");

	// —— 钢笔曲线：曲率贴合样条 ——
	{
		auto buildPen = [](const std::vector<D2D1_POINT_2F>& pts) {
			ArrowPolyState st;
			st.pen = true;
			st.pts = pts;
			st.penStraight.assign(pts.size() - 1, false);
			return st;
		};
		const std::vector<D2D1_POINT_2F> arcPts{
			D2D1::Point2F(119.f, 382.f), D2D1::Point2F(428.f, 102.f),
			D2D1::Point2F(497.f, 101.f), D2D1::Point2F(806.f, 381.f) };
		const auto arcSegs = buildPen(arcPts).penSegments();
		double kMin = 1e9, kMax = 0.f;
		double kFirst = 0.f, kLast = 0.f;
		for (size_t i = 0; i < arcSegs.size(); i++) {
			for (int j = 0; j <= 40; j++) {
				const double k = penCurvature(arcSegs[i], (float)j / 40.f);
				if (i == 0 && j == 0) kFirst = k;
				if (i + 1 == arcSegs.size() && j == 40) kLast = k;
				kMin = std::min(kMin, k);
				kMax = std::max(kMax, k);
			}
		}
		// 前两个锚点与末锚点的外接圆：四点近似共圆时，参考曲线就是这条圆弧
		const double ax = arcPts[0].x, ay = arcPts[0].y;
		const double bx = arcPts[1].x, by = arcPts[1].y;
		const double cx = arcPts[3].x, cy = arcPts[3].y;
		const double dd = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
		const double ox = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay)
			+ (cx * cx + cy * cy) * (ay - by)) / dd;
		const double oy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx)
			+ (cx * cx + cy * cy) * (bx - ax)) / dd;
		const double radius = std::hypot(ax - ox, ay - oy);
		double dev = 0.f;
		for (const auto& sg : arcSegs)
			for (int j = 1; j <= 40; j++) {
				const double px = bezX(sg, (float)j / 40.f), py = bezY(sg, (float)j / 40.f);
				dev = std::max(dev, std::abs(std::hypot(px - ox, py - oy) - radius));
			}
		wchar_t buf[256]{};
		swprintf_s(buf, L"钢笔曲线 切线迭代求解 κ∈[%.5f,%.5f] 端 κ %.5f/%.5f 偏离外接圆 %.2fpx",
			kMin, kMax, kFirst, kLast, dev);
		check(kMin >= 0.f && kMax < 0.005f && dev < 30.f, buf);

		// 共线锚点必须仍是直线
		const std::vector<D2D1_POINT_2F> linePts{
			D2D1::Point2F(0.f, 0.f), D2D1::Point2F(100.f, 0.f),
			D2D1::Point2F(200.f, 0.f), D2D1::Point2F(300.f, 0.f) };
		const auto lineSegs = buildPen(linePts).penSegments();
		double lineDev = 0.f;
		for (const auto& sg : lineSegs)
			for (int j = 1; j <= 40; j++)
				lineDev = std::max(lineDev, std::fabs(bezY(sg, (float)j / 40.f)));
		swprintf_s(buf, L"钢笔曲线 共线锚点保持直线 法向偏移 %.3fpx", lineDev);
		check(lineDev < 0.05f, buf);

		// 预览跟手：同一笔连续下点，已定型段应当完全不动
		ArrowPolyState warm = buildPen(arcPts);
		const auto still = warm.penSegments();
		warm.penPreview = true;
		warm.penPreviewPt = D2D1::Point2F(806.f, 389.f);
		warm.penCacheValid = false;
		const auto moved = warm.penSegments();
		double drift = 0.f;
		for (size_t i = 0; i + 1 < still.size() && i + 1 < moved.size(); i++)
			for (int j = 0; j <= 20; j++) {
				drift = std::max(drift, std::hypot(
					bezX(moved[i], (float)j / 20.f) - bezX(still[i], (float)j / 20.f),
					bezY(moved[i], (float)j / 20.f) - bezY(still[i], (float)j / 20.f)));
			}
		swprintf_s(buf, L"钢笔曲线 预览移动 8px 定型段最大位移 %.2fpx", drift);
		check(drift < 1.0f, buf);
	}

	// —— 放大镜落位：只避让、不记忆（默认右下，放不下才换边，放得下立刻回右下） ——
	{
		const float W = 128.f, H = 190.f, G = 12.f;
		const float AL = 0.f, AT = 0.f, AR = 2560.f, AB = 1080.f;
		auto inside = [&](const PixLensBox& b) {
			return b.x >= AL - 0.5f && b.y >= AT - 0.5f
				&& b.x + W <= AR + 0.5f && b.y + H <= AB + 0.5f;
		};
		// 外壳是「取样点旁边的取色窗」，压住光标就等于挡住了要看的那一格
		auto coversCursor = [&](const PixLensBox& b, float cx, float cy) {
			return cx > b.x && cx < b.x + W && cy > b.y && cy < b.y + H;
		};
		auto run = [&](float cx, float cy, int wantQuad, const wchar_t* name) {
			PixLensBox b{};
			const int q = pixLensQuadrant(cx, cy, W, H, G, AL, AT, AR, AB, &b);
			wchar_t buf[256]{};
			swprintf_s(buf, L"放大镜落位 %s 象限=%d 盒=(%.0f,%.0f)", name, q, b.x, b.y);
			check(q == wantQuad && inside(b) && !coversCursor(b, cx, cy), buf);
		};

		run(1280.f, 540.f, 0, L"屏幕中央→右下");
		run(2540.f, 540.f, 1, L"右边→左下");
		run(1280.f, 1060.f, 2, L"下边→右上");
		run(2540.f, 1060.f, 3, L"右下角→左上");
		run(5.f, 5.f, 0, L"左上角→右下");
		run(1280.f, 5.f, 0, L"上边→右下");
		run(5.f, 540.f, 0, L"左边→右下");
		// 只避让不记忆：从右下角挪回屏幕中央必须立刻回右下
		run(2540.f, 1060.f, 3, L"右下角（避让）");
		run(1280.f, 540.f, 0, L"离开边缘立刻回右下");

		// 全屏扫描：任何位置外壳都要整块留在显示器内，且不压住取样点
		bool sweepOk = true;
		for (float cx = 1.f; cx < 2560.f; cx += 37.f)
			for (float cy = 1.f; cy < 1080.f; cy += 41.f) {
				PixLensBox b{};
				pixLensQuadrant(cx, cy, W, H, G, AL, AT, AR, AB, &b);
				if (!inside(b) || coversCursor(b, cx, cy)) sweepOk = false;
			}
		check(sweepOk, L"放大镜落位 全屏扫描均不出界、不压光标");

		// 显示器比外壳还小：没有完整解，返回 -1，但仍要夹在区域内
		PixLensBox tiny{};
		const int tq = pixLensQuadrant(50.f, 50.f, W, H, G, 0.f, 0.f, 100.f, 100.f, &tiny);
		check(tq == -1 && tiny.x >= 0.f && tiny.y >= 0.f && tiny.x <= 100.f && tiny.y <= 100.f,
			L"放大镜落位 区域过小时夹回区域内");
	}

	// —— 光标位图解析：单色（工字）与彩色光标都必须能解出可见像素 ——
	{
		HDC screenDc = GetDC(nullptr);
		HDC memDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
		auto tryCursor = [&](LPCWSTR id, const wchar_t* name) {
			HCURSOR cur = LoadCursorW(nullptr, id);
			ICONINFO ii{};
			int w = 0, h = 0;
			std::vector<BYTE> px;
			const bool ok = cur && GetIconInfo(cur, &ii)
				&& CursorImage::ChooseSize(ii, &w, &h)
				&& memDc && CursorImage::Extract(memDc, ii, w, h, px)
				&& px.size() == (size_t)w * h * 4;
			if (ii.hbmColor) DeleteObject(ii.hbmColor);
			if (ii.hbmMask) DeleteObject(ii.hbmMask);
			wchar_t buf[192]{};
			swprintf_s(buf, L"光标位图 %s 解析 %dx%d 且有不透明像素", name, w, h);
			check(ok, buf);
		};
		tryCursor(IDC_IBEAM, L"工字");
		tryCursor(IDC_ARROW, L"箭头");
		tryCursor(IDC_HAND, L"手型");
		tryCursor(IDC_SIZEALL, L"移动");
		if (memDc) DeleteDC(memDc);
		if (screenDc) ReleaseDC(nullptr, screenDc);
	}

	// —— 配置三路合并：不同键的修改共存、同键最后写入者赢、删除生效 ——
	{
		using namespace winrt::Windows::Data::Json;
		const auto parse = [](const wchar_t* s) { return JsonObject::Parse(s); };
		auto latest = parse(LR"({"common":{"lang":"zh","autoStart":true},"features":{"tray":1},"other":"keep"})");
		auto base = parse(LR"({"common":{"lang":"zh","autoStart":true},"features":{"tray":1}})");
		auto mine = parse(LR"({"common":{"lang":"en","autoStart":true},"features":{"tray":2},"toolPin":{"x":1}})");
		ConfigMerge::MergeObjects(latest, base, mine);
		check(std::wstring{ latest.GetNamedObject(L"common").GetNamedString(L"lang", L"") } == L"en",
			L"配置合并 自己改的键生效");
		check(latest.GetNamedObject(L"features").GetNamedNumber(L"tray") == 2,
			L"配置合并 同键最后写入者赢");
		check(std::wstring{ latest.GetNamedString(L"other", L"") } == L"keep",
			L"配置合并 保留对方/其它键");
		check(latest.GetNamedObject(L"toolPin").GetNamedNumber(L"x") == 1,
			L"配置合并 新增对象键");

		auto latest2 = parse(LR"({"features":{"a":1},"keep":true})");
		auto base2 = parse(LR"({"features":{"a":1}})");
		auto mine2 = parse(LR"({"keep":true})");
		ConfigMerge::MergeObjects(latest2, base2, mine2);
		check(!latest2.HasKey(L"features") && latest2.GetNamedBoolean(L"keep"),
			L"配置合并 删除的键生效");
	}

	// —— 窗口黑名单：标题会变，必须同时认任务管理器里的进程名 ——
	{
		using BlacklistMatch::matches;
		// AI 对话类软件的标题就是当前对话，标题里完全看不到应用名
		check(matches(L"DeepSeek Harness", L"帮我改一下黑名单逻辑", L"DeepSeek Harness", L"DeepSeek Harness"),
			L"黑名单 标题随对话变化时按进程名命中");
		// 开始菜单显示名和进程名不同：Illustrator.exe / Code.exe
		check(matches(L"Adobe Illustrator 2026", L"未命名-1.ai @ 100% (RGB/预览)", L"Illustrator", L"Adobe Illustrator"),
			L"黑名单 显示名与进程名不同（Illustrator）也命中");
		check(matches(L"Visual Studio Code", L"Setting.cpp - SnowAir", L"Code", L"Visual Studio Code"),
			L"黑名单 进程名是显示名里的一个词（Code）也命中");
		check(matches(L"ChatGPT.exe", L"新对话", L"ChatGPT", L"ChatGPT"),
			L"黑名单 词条带 .exe 也能匹配进程名");
		check(matches(L"deepseek-harness", L"某个会话", L"DeepSeek Harness", L""),
			L"黑名单 空白/连字符写法差异也能匹配");
		check(matches(L"Figma", L"设计稿 - Figma", L"Figma", L"Figma"),
			L"黑名单 标题没变时仍按标题命中（旧词条不受影响）");
		check(!matches(L"AI", L"email from boss", L"outlook", L"Microsoft Outlook"),
			L"黑名单 两字母词条不会误伤 email");
		check(matches(L"AI", L"AI 绘画 - 豆包", L"Doubao", L"豆包"),
			L"黑名单 两字母词条按整词命中标题");
		check(!matches(L"", L"任意标题", L"anything", L""), L"黑名单 空词条不匹配");
	}

	// —— 长截图拼接：位移估计 + 覆盖式拼接 ——
	// 合成一页内容（文字行 + 照片块 + 渐变条），按真实滚动位置切帧，验证：
	//   1. 各种步长（含一次滚过大半个视口）都能定位并拼上，画面逐行对得上；
	//   2. 吸顶/吸底固定栏不会被反复拼进长图，也不会留在长图中间；
	//   3. 来回滚动不重复写入；
	//   4. 快滚帧（带运动模糊）之后补抓的清晰帧能刷新底部条带；
	//   5. 固定背景（整帧背景不变）时只靠滚动的内容定位；
	//   6. 噪声、慢滚、纯动画、跳太远都不会把长图拼坏；
	//   7. 从页面中间往上滚能向上长；
	//   8. 固定背景 + 长串滚动时正文连续、不重复（按文字像素逐行核对）。
	{
		const int W = 640, H = 720, PAGE_H = 12000;
		const int barTop = 56, barBottom = 48; // 吸顶/吸底栏高度
		std::vector<BYTE> page((size_t)W * PAGE_H * 4, 250);
		for (int y = 0; y < PAGE_H; ++y) {
			BYTE* row = page.data() + (size_t)y * W * 4;
			const int line = y / 24;
			const bool textRow = (y % 24) < 16;
			const bool photo = (line % 37) >= 30;
			const bool gradient = (line % 53) >= 48;
			for (int x = 0; x < W; ++x) {
				int v = 250;
				if (textRow && x > 30 && x < W - 40) {
					uint32_t hv = (uint32_t)y * 2654435761u ^ (uint32_t)(x / 3) * 40503u ^ (uint32_t)line;
					hv ^= hv >> 13; hv *= 1274126177u; hv ^= hv >> 16;
					if (hv % 5 < 2) v = 25 + (int)(hv % 40);
				}
				if (photo) {
					uint32_t hv = (uint32_t)y * 2246822519u ^ (uint32_t)x * 3266489917u;
					hv ^= hv >> 15; hv *= 668265263u; hv ^= hv >> 13;
					v = 40 + (int)(hv % 170);
				}
				else if (gradient) {
					v = 60 + (int)((y % 130) * 120 / 130) + (x * 20 / W);
				}
				BYTE* p = row + (size_t)x * 4;
				p[0] = p[1] = p[2] = (BYTE)v;
				p[3] = 255;
			}
		}
		// 固定栏：带边框与竖条，页面本身永远不出现这些像素
		std::vector<BYTE> topBarPix((size_t)W * barTop * 4);
		std::vector<BYTE> bottomBarPix((size_t)W * barBottom * 4);
		for (int y = 0; y < barTop; ++y) {
			for (int x = 0; x < W; ++x) {
				BYTE* p = topBarPix.data() + ((size_t)y * W + x) * 4;
				const int v = (y < 2 || y >= barTop - 2) ? 60 : (((x / 40) % 2) ? 90 : 210);
				p[0] = p[1] = p[2] = (BYTE)v;
				p[3] = 255;
			}
		}
		for (int y = 0; y < barBottom; ++y) {
			for (int x = 0; x < W; ++x) {
				BYTE* p = bottomBarPix.data() + ((size_t)y * W + x) * 4;
				const int v = (y < 2 || y >= barBottom - 2) ? 70 : (((x / 30) % 2) ? 110 : 200);
				p[0] = p[1] = p[2] = (BYTE)v;
				p[3] = 255;
			}
		}
		auto pageRow = [&](int y) -> const BYTE* { return page.data() + (size_t)y * W * 4; };

		struct FrameOpt { bool bars{ false }; int blur{ 0 }; };
		auto makeFrame = [&](int scroll, const FrameOpt& opt) {
			std::vector<BYTE> f((size_t)W * H * 4, 250);
			for (int y = 0; y < H; ++y) {
				const int sy = std::clamp(scroll + y, 0, PAGE_H - 1);
				memcpy(f.data() + (size_t)y * W * 4, pageRow(sy), (size_t)W * 4);
			}
			if (opt.blur > 0) {
				// 垂直方向把相邻两行混合：模拟快速滚动时的运动模糊
				std::vector<BYTE> src = f;
				for (int y = 0; y < H; ++y) {
					const int y2 = std::min(H - 1, y + opt.blur);
					BYTE* dst = f.data() + (size_t)y * W * 4;
					const BYTE* a = src.data() + (size_t)y * W * 4;
					const BYTE* b = src.data() + (size_t)y2 * W * 4;
					for (int i = 0; i < W * 4; ++i) dst[i] = (BYTE)(((int)a[i] + (int)b[i]) / 2);
				}
			}
			if (opt.bars) {
				memcpy(f.data(), topBarPix.data(), topBarPix.size());
				memcpy(f.data() + (size_t)(H - barBottom) * W * 4, bottomBarPix.data(), bottomBarPix.size());
			}
			return f;
		};

		auto compareRows = [&](const std::vector<BYTE>& canvas, int canvasH, int fromRow, const wchar_t* name) {
			int bad = -1;
			for (int r = fromRow; r < canvasH && bad < 0; ++r) {
				if (memcmp(canvas.data() + (size_t)r * W * 4, pageRow(r), (size_t)W * 4) != 0) bad = r;
			}
			wchar_t buf[192]{};
			if (bad < 0) swprintf_s(buf, L"%s：画面与页面逐行一致（%d 行）", name, canvasH);
			else swprintf_s(buf, L"%s：第 %d 行与页面对不上", name, bad);
			check(bad < 0, buf);
		};

		struct RunResult { bool ok{ false }; int scroll{ 0 }; int maxScroll{ 0 }; int canvasH{ 0 }; std::vector<BYTE> canvas; std::vector<int> shifts; };
		auto runSteps = [&](const std::vector<int>& steps, const FrameOpt& opt) {
			RunResult out;
			LongStitch::Stitcher st;
			auto first = makeFrame(0, opt);
			out.ok = st.begin(first.data(), W, H, W * 4, 36000, 256ull << 20);
			if (!out.ok) return out;
			for (int step : steps) {
				out.scroll = std::clamp(out.scroll + step, 0, PAGE_H - H);
				if (out.scroll > out.maxScroll) out.maxScroll = out.scroll;
				auto f = makeFrame(out.scroll, opt);
				const auto r = st.push(f.data(), W * 4);
				out.shifts.push_back(r.shift);
			}
			out.canvasH = st.height();
			if (st.data() && st.bytes() > 0) out.canvas.assign(st.data(), st.data() + st.bytes());
			return out;
		};

		// 1) 普通页面：各种步长（含一次滚过大半个视口）
		{
			FrameOpt opt;
			const std::vector<int> steps{ 120, 240, 360, 420, 60, 33 };
			auto r = runSteps(steps, opt);
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 大步小步混合：画布高度 %d（应为 %d）", r.canvasH, r.maxScroll + H);
			check(r.ok && r.canvasH == r.maxScroll + H, buf);
			compareRows(r.canvas, r.canvasH, 0, L"长截图拼接 大步小步混合");
			bool shiftsOk = (r.shifts.size() == steps.size());
			for (size_t i = 0; shiftsOk && i < steps.size(); ++i)
				shiftsOk = std::abs(r.shifts[i] + steps[i]) <= 1;
			check(shiftsOk, L"长截图拼接 每步位移都估对（误差 <= 1 行）");
		}

		// 2) 吸顶/吸底固定栏：新行只从内容区取，固定栏不会被反复拼进去
		{
			FrameOpt opt;
			opt.bars = true;
			const std::vector<int> steps{ 120, 240, 360, 400, 180 };
			auto r = runSteps(steps, opt);
			const int expectH = r.scroll + (H - barBottom);
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 固定栏：画布高度 %d（应为 %d）", r.canvasH, expectH);
			check(r.ok && std::abs(r.canvasH - expectH) <= 2, buf);
			compareRows(r.canvas, r.canvasH, barTop, L"长截图拼接 固定栏");
			// 顶部保留首帧的固定栏，而不是页面内容
			bool topBarKept = r.canvas.size() >= (size_t)barTop * W * 4
				&& memcmp(r.canvas.data(), topBarPix.data(), topBarPix.size()) == 0;
			check(topBarKept, L"长截图拼接 顶部固定栏保留在长图开头");
		}

		// 3) 来回滚动：回看已拼区域不重复写入
		{
			FrameOpt opt;
			auto r = runSteps({ 300, 300, -200, 400, -100 }, opt);
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 来回滚动：画布高度 %d（应为 %d）", r.canvasH, r.maxScroll + H);
			check(r.ok && r.canvasH == r.maxScroll + H, buf);
			compareRows(r.canvas, r.canvasH, 0, L"长截图拼接 来回滚动");
		}

		// 4) 快滚帧带运动模糊，最后用停稳帧刷新底部条带
		{
			FrameOpt opt;
			opt.blur = 2;
			LongStitch::Stitcher st;
			auto first = makeFrame(0, opt);
			bool ok = st.begin(first.data(), W, H, W * 4, 36000, 256ull << 20);
			int scroll = 0;
			bool shiftsOk = true;
			for (int step : { 220, 260, 240 }) {
				scroll += step;
				auto f = makeFrame(scroll, opt);
				const auto r = st.push(f.data(), W * 4);
				if (!r.accepted || std::abs(r.shift + step) > 1) shiftsOk = false;
			}
			check(ok && shiftsOk, L"长截图拼接 运动模糊帧仍能估对位移");
			auto sharp = makeFrame(scroll, FrameOpt{});
			const bool refreshed = st.refreshTail(sharp.data(), W * 4);
			int bad = -1;
			const int canvasH = st.height();
			for (int r = std::max(0, canvasH - 300); r < canvasH && bad < 0; ++r) {
				if (memcmp(st.data() + (size_t)r * W * 4, pageRow(r), (size_t)W * 4) != 0) bad = r;
			}
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 停稳帧刷新底部条带（首个不一致行 %d）", bad);
			check(refreshed && bad < 0, buf);
		}

		// 5) 固定背景：整帧背景每帧一样，只有滚动的内容在动
		{
			std::vector<BYTE> backdrop((size_t)W * H * 4);
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					uint32_t hv = (uint32_t)y * 2654435761u ^ (uint32_t)x * 40503u;
					hv ^= hv >> 13; hv *= 1274126177u; hv ^= hv >> 16;
					BYTE* p = backdrop.data() + ((size_t)y * W + x) * 4;
					p[0] = p[1] = p[2] = (BYTE)(30 + hv % 160);
					p[3] = 255;
				}
			}
			auto frameWithBackdrop = [&](int scroll) {
				auto f = backdrop;
				for (int y = 0; y < H; ++y) {
					const int docY = std::clamp(scroll + y, 0, PAGE_H - 1);
					const BYTE* src = pageRow(docY);
					BYTE* dst = f.data() + (size_t)y * W * 4;
					for (int x = 0; x < W; ++x) {
						const BYTE* s = src + (size_t)x * 4;
						if (s[0] < 200) memcpy(dst + (size_t)x * 4, s, 4); // 只有文字像素盖到背景上
					}
				}
				return f;
			};
			LongStitch::Stitcher st;
			auto f0 = frameWithBackdrop(0);
			bool ok = st.begin(f0.data(), W, H, W * 4, 36000, 256ull << 20);
			int scroll = 0, badShifts = 0;
			for (int step : { 160, 220, 180, 240 }) {
				scroll += step;
				auto f = frameWithBackdrop(scroll);
				const auto r = st.push(f.data(), W * 4);
				if (!r.accepted || std::abs(r.shift + step) > 1) ++badShifts;
			}
			check(ok && badShifts == 0, L"长截图拼接 固定背景下只靠滚动内容定位");
		}

		// 6) 完全重复的帧不增长；一次跳太远（没有重叠）不写入
		{
			LongStitch::Stitcher st;
			FrameOpt opt;
			auto first = makeFrame(0, opt);
			bool ok = st.begin(first.data(), W, H, W * 4, 36000, 256ull << 20);
			auto again = makeFrame(0, opt);
			const auto r0 = st.push(again.data(), W * 4);
			const bool stable0 = !r0.accepted && !r0.grew && st.height() == H;
			auto jump = makeFrame(H * 4, opt); // 位移远超帧高：没有重叠
			const auto r1 = st.push(jump.data(), W * 4);
			const bool stable1 = !r1.grew && st.height() == H;
			check(ok && stable0, L"长截图拼接 画面没动时不增长");
			check(stable1, L"长截图拼接 跳太远的一帧不写入");
		}

		// 7) 每帧带轻微噪声：噪声不能把定位带偏
		{
			auto noisy = [&](std::vector<BYTE> f, unsigned seed, int amp) {
				unsigned s = seed;
				for (size_t i = 0; i + 3 < f.size(); i += 4) {
					s = s * 1664525u + 1013904223u;
					const int d = (int)((s >> 24) % (unsigned)(amp * 2 + 1)) - amp;
					f[i] = (BYTE)std::clamp((int)f[i] + d, 0, 255);
					f[i + 1] = (BYTE)std::clamp((int)f[i + 1] + d, 0, 255);
					f[i + 2] = (BYTE)std::clamp((int)f[i + 2] + d, 0, 255);
				}
				return f;
			};
			LongStitch::Stitcher st;
			auto f0 = noisy(makeFrame(0, FrameOpt{}), 7, 3);
			bool ok = st.begin(f0.data(), W, H, W * 4, 36000, 256ull << 20);
			int scroll = 0, bad = 0;
			for (int step : { 140, 260, 320, 190 }) {
				scroll += step;
				auto f = noisy(makeFrame(scroll, FrameOpt{}), 100 + (unsigned)scroll, 3);
				const auto r = st.push(f.data(), W * 4);
				if (!r.accepted || std::abs(r.shift + step) > 1) ++bad;
			}
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 帧间噪声下仍能定位（失败 %d 步）", bad);
			check(ok && bad == 0 && st.height() == scroll + H, buf);
		}

		// 8) 慢速滚动：每次只滚 1~3 行
		{
			LongStitch::Stitcher st;
			auto f0 = makeFrame(0, FrameOpt{});
			bool ok = st.begin(f0.data(), W, H, W * 4, 36000, 256ull << 20);
			int scroll = 0, bad = 0;
			for (int step : { 1, 2, 1, 3, 2, 3 }) {
				scroll += step;
				auto f = makeFrame(scroll, FrameOpt{});
				const auto r = st.push(f.data(), W * 4);
				if (!r.accepted || r.shift != -step) ++bad;
			}
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 慢滚 1~3 行（失败 %d 步）", bad);
			check(ok && bad == 0 && st.height() == scroll + H, buf);
		}

		// 9) 只有局部动画、页面没有滚动：不能增长
		{
			LongStitch::Stitcher st;
			auto f0 = makeFrame(0, FrameOpt{});
			bool ok = st.begin(f0.data(), W, H, W * 4, 36000, 256ull << 20);
			int grew = 0;
			for (int k = 1; k <= 3; ++k) {
				auto f = makeFrame(0, FrameOpt{});
				for (int y = 300; y < 380; ++y)
					for (int x = 100; x < 260; ++x) {
						BYTE* p = f.data() + ((size_t)y * W + x) * 4;
						p[0] = p[1] = p[2] = (k % 2) ? 20 : 240;
					}
				const auto r = st.push(f.data(), W * 4);
				if (r.grew) ++grew;
			}
			check(ok && grew == 0 && st.height() == H, L"长截图拼接 只有局部动画时不增长");
		}

		// 10) 从页面中间开始、往上滚：长图向上长，且依然逐行对得上、不重复
		{
			LongStitch::Stitcher st;
			int scroll = 800;
			auto f0 = makeFrame(scroll, FrameOpt{});
			bool ok = st.begin(f0.data(), W, H, W * 4, 36000, 256ull << 20);
			bool shiftsOk = true;
			for (int step : { -100, -200, -300, -200 }) {
				scroll += step;
				auto f = makeFrame(scroll, FrameOpt{});
				const auto r = st.push(f.data(), W * 4);
				if (!r.accepted || r.shift != -step || !r.fromTop) shiftsOk = false;
			}
			int bad = -1;
			for (int y = 0; y < st.height() && bad < 0; ++y)
				if (memcmp(st.data() + (size_t)y * W * 4, pageRow(y), (size_t)W * 4) != 0) bad = y;
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 从中间往上滚：画布 %d 行（应为 %d），首个不一致行 %d",
				st.height(), 800 + H, bad);
			check(ok && shiftsOk && st.height() == 800 + H && bad < 0, buf);
		}

		// 11) 固定背景 + 长串自动滚动：正文必须连续、不能重复
		// 背景是每帧都一样、且始终比文字亮的浅色噪点，所以“文字像素”能单独拎出来核对。
		{
			std::vector<BYTE> backdrop((size_t)W * H * 4);
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					uint32_t hv = (uint32_t)y * 2654435761u ^ (uint32_t)x * 40503u;
					hv ^= hv >> 13; hv *= 1274126177u; hv ^= hv >> 16;
					BYTE* p = backdrop.data() + ((size_t)y * W + x) * 4;
					p[0] = p[1] = p[2] = (BYTE)(185 + hv % 60);
					p[3] = 255;
				}
			}
			auto frameWithBackdrop = [&](int scroll) {
				auto f = backdrop;
				for (int y = 0; y < H; ++y) {
					const int docY = std::clamp(scroll + y, 0, PAGE_H - 1);
					const BYTE* src = pageRow(docY);
					BYTE* dst = f.data() + (size_t)y * W * 4;
					for (int x = 0; x < W; ++x) {
						if (src[(size_t)x * 4] < 150) memcpy(dst + (size_t)x * 4, src + (size_t)x * 4, 4);
					}
				}
				return f;
			};
			LongStitch::Stitcher st;
			int scroll = 0;
			auto f0 = frameWithBackdrop(0);
			bool ok = st.begin(f0.data(), W, H, W * 4, 36000, 256ull << 20);
			int badAccept = 0;
			for (int i = 0; i < 20; ++i) {
				const int step = 110 + (i * 37) % 60; // 模拟自动滚动的不等步长
				scroll = std::clamp(scroll + step, 0, PAGE_H - H);
				auto f = frameWithBackdrop(scroll);
				const auto r = st.push(f.data(), W * 4);
				if (!r.accepted || !r.grew) ++badAccept;
			}
			int bad = -1;
			for (int y = 0; y < st.height() && bad < 0; ++y) {
				const BYTE* cv = st.data() + (size_t)y * W * 4;
				const BYTE* pg = pageRow(std::clamp(y, 0, PAGE_H - 1));
				int diff = 0, total = 0;
				for (int x = 2; x < W - 2; x += 3) {
					const bool a = cv[(size_t)x * 4] < 150;
					const bool b = pg[(size_t)x * 4] < 150;
					if (a != b) ++diff;
					++total;
				}
				if (diff > total / 50) bad = y;
			}
			wchar_t buf[192]{};
			swprintf_s(buf, L"长截图拼接 固定背景长串滚动：%d 行（应为 %d），失手 %d 步，首个重复/错位行 %d",
				st.height(), scroll + H, badAccept, bad);
			check(ok && badAccept == 0 && st.height() == scroll + H && bad < 0, buf);
		}
	}
	if (g_log)
	{
		fwprintf(g_log, L"\n失败项：%d\n", g_fail);
		fclose(g_log);
	}
	return g_fail;
}
