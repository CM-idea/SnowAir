#include "pch.h"
#include <climits>
#include <dwmapi.h>
#include <commdlg.h>
// 顺序有讲究：oleacc.h 必须在 UIAutomation*.h 之前，否则 ITextProvider 会重复定义
#include <oleacc.h>
#include <UIAutomationClient.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <include/Ling.h>
#include "CutMask.h"
#include "../Tool/ToolbarTheme.h"
#include "../Tool/ToolbarChrome.h"
#include "../Tool/IconCodes.h"
using namespace Microsoft::WRL;

namespace {
	bool isCloaked(HWND hwnd)
	{
		BOOL cloaked = FALSE;
		return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
	}

	bool isDesktopOrShell(HWND hwnd)
	{
		wchar_t cls[256]{};
		const int n = GetClassNameW(hwnd, cls, 256);
		if (n <= 0) return false;
		return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0
			|| wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
	}

	bool isOwnProcess(HWND hwnd)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		return pid != 0 && pid == GetCurrentProcessId();
	}

	D2D1_RECT_F clientRectFromScreen(const RECT& screen, Ling::WinBase* win)
	{
		float l = (float)(screen.left - win->x);
		float t = (float)(screen.top - win->y);
		float r = (float)(screen.right - win->x);
		float b = (float)(screen.bottom - win->y);
		l = std::clamp(l, 0.f, (float)win->w);
		t = std::clamp(t, 0.f, (float)win->h);
		r = std::clamp(r, 0.f, (float)win->w);
		b = std::clamp(b, 0.f, (float)win->h);
		return D2D1::RectF(l, t, r, b);
	}

	// 无障碍（UIA）客户端对象：每个查询线程一份（查询跑在工作线程上，按线程建最省事也最安全）
	struct UiaCtx
	{
		IUIAutomation* automation{ nullptr };
		IUIAutomation2* automation2{ nullptr };            // 只为按剩余预算设调用超时（可能为空）
		IUIAutomationCacheRequest* cacheRequest{ nullptr };

		void init()
		{
			if (automation) return;
			if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
				IID_IUIAutomation, reinterpret_cast<void**>(&automation))) || !automation)
				return;
			// 超时能力（IUIAutomation2）：下钻每次跨进程调用前按剩余预算设连接/事务超时，
			// 一个卡住的提供者就吃不满整轮预算。拿不到就退化成"只有整轮 wall-clock 预算"。
			automation->QueryInterface(IID_IUIAutomation2, reinterpret_cast<void**>(&automation2));
			// 缓存请求 = 下钻的全部成本模型，三件事一起定：
			//  · 范围只要"元素自身 + 一层子节点"：逐层展开才有机会在看到巨额子节点列表时提前收手，
			//    一上来就要整棵子树等于把整页节点的编码成本一次付清；
			//  · 视图用 Control：下钻的目标是"用户能操作/看到的那一层"，纯布局包装节点不在这个视图里；
			//  · 矩形、屏幕外、控件类型三个属性随缓存带回：逐层展开全部读本地值，不再逐节点跨进程往返。
			if (SUCCEEDED(automation->CreateCacheRequest(&cacheRequest)) && cacheRequest) {
				cacheRequest->AddProperty(UIA_BoundingRectanglePropertyId);
				cacheRequest->AddProperty(UIA_IsOffscreenPropertyId);
				cacheRequest->AddProperty(UIA_ControlTypePropertyId);
				cacheRequest->put_TreeScope((TreeScope)(TreeScope_Element | TreeScope_Children));
				IUIAutomationCondition* controlView{ nullptr };
				if (SUCCEEDED(automation->get_ControlViewCondition(&controlView)) && controlView) {
					cacheRequest->put_TreeFilter(controlView);
					controlView->Release();
				}
			}
		}
	};

	// thread_local 的 UIA 实例故意不释放：线程退到进程尾巴上，释放反而容易踩 COM 关闭顺序
	UiaCtx& uiaCtx()
	{
		thread_local UiaCtx* ctx = []() { auto* c = new UiaCtx(); c->init(); return c; }();
		return *ctx;
	}

	/** 读布尔属性：1=true / 0=false / -1=属性不可用（不据此判定，避免误杀实现不全的提供者）。 */
	int boolProp(IUIAutomationElement* el, PROPERTYID id)
	{
		if (!el) return -1;
		VARIANT v;
		VariantInit(&v);
		int r = -1;
		if (SUCCEEDED(el->GetCurrentPropertyValue(id, &v)) && v.vt == VT_BOOL)
			r = (v.boolVal == VARIANT_TRUE) ? 1 : 0;
		VariantClear(&v);
		return r;
	}

	/** 读整数属性：返回属性值，取不到返回 -1。 */
	long intProp(IUIAutomationElement* el, PROPERTYID id)
	{
		if (!el) return -1;
		VARIANT v;
		VariantInit(&v);
		long r = -1;
		if (SUCCEEDED(el->GetCurrentPropertyValue(id, &v)) && v.vt == VT_I4)
			r = v.lVal;
		VariantClear(&v);
		return r;
	}

	/** “屏幕上没有这个元素”判定：只用 offscreen 这一条硬事实（未绘制/滚出视口，树里只剩旧位置）。
	 *  不按“内容元素/控件元素”再筛一道 —— 不少普通 DIV/容器在语义视图里两者都不算，按那条规则
	 *  会把它们当成"莫须有"拒掉（表现就是"有些 DIV 找不到"）。属性取不到（-1）时不拒。 */
	bool isPhantomElement(IUIAutomationElement* el)
	{
		if (!el) return true;
		return boolProp(el, UIA_IsOffscreenPropertyId) == 1;
	}

	bool rectContainsPt(const RECT& r, POINT pt)
	{
		return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
	}

	RECT monitorRectAt(POINT screen)
	{
		HMONITOR mon = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{ sizeof(MONITORINFO) };
		if (mon && GetMonitorInfo(mon, &mi)) return mi.rcMonitor;
		return RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
	}

	bool hwndBounds(HWND hwnd, RECT& out)
	{
		if (!hwnd) return false;
		if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out, sizeof(out)))
			&& out.right - out.left >= 4 && out.bottom - out.top >= 4)
			return true;
		return GetWindowRect(hwnd, &out)
			&& out.right - out.left >= 4 && out.bottom - out.top >= 4;
	}

	LONGLONG rectArea(const RECT& r)
	{
		const LONGLONG w = (std::max)(0L, r.right - r.left);
		const LONGLONG h = (std::max)(0L, r.bottom - r.top);
		return w * h;
	}

	bool rectValid(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

	bool rectEqual(const RECT& a, const RECT& b)
	{
		return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
	}

	/** 相交；没有交集返回零矩形（调用方用 rectValid 判掉）。 */
	RECT intersectRect(const RECT& a, const RECT& b)
	{
		RECT r{ (std::max)(a.left, b.left), (std::max)(a.top, b.top),
			(std::min)(a.right, b.right), (std::min)(a.bottom, b.bottom) };
		if (!rectValid(r)) return RECT{ 0, 0, 0, 0 };
		return r;
	}

	RECT hwndBoundsRect(HWND hwnd)
	{
		RECT r{};
		hwndBounds(hwnd, r);
		return r;
	}

	/** 整块级大框判定（按覆盖比例）：宽高都到参考区 4/5 才算。
	 *  不能用"宽高都近乎全尺寸"：实测命中测试与下钻都会给出几乎铺满内容区（如 2537×914 对 2552×914）
	 *  的盒子，只差十几像素 → 整页盒被当成具体元素发布，屏幕上就是画一个巨大的"莫须有框"，
	 *  而且本该接管的下钻分支永远走不到。真正的具体元素几乎不可能同时占满两维。 */
	bool isPageLevelRect(const RECT& r, const RECT& refBase)
	{
		if (!rectValid(r) || !rectValid(refBase)) return false;
		return (r.right - r.left) >= (refBase.right - refBase.left) * 4 / 5
			&& (r.bottom - r.top) >= (refBase.bottom - refBase.top) * 4 / 5;
	}

	/** 不该被当作查找目标的窗口：不可见 / 最小化 / 幽灵（DWM cloaked）/ 桌面外壳 / 鼠标穿透。 */
	bool isExcludedCaptureWindow(HWND hwnd)
	{
		if (!hwnd) return true;
		if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || isCloaked(hwnd) || isDesktopOrShell(hwnd))
			return true;
		const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
		return (ex & WS_EX_TRANSPARENT) != 0;
	}

	/** 自家进程的窗口一律不是目标：截图遮罩自己、工具条，以及各种隐藏辅助窗 ——
	 *  不排除的话命中测试会返回遮罩自己的元素，高亮出页面上根本没有的"莫须有"框。 */
	bool isBadWindow(HWND hwnd, HWND exclude)
	{
		if (!hwnd || hwnd == exclude) return true;
		if (isOwnProcess(hwnd)) return true;
		return isExcludedCaptureWindow(hwnd);
	}

	// ---------------- 1) 锚点：点下的"内容子窗" ----------------
	/** 真正持有无障碍子树的那个子窗。
	 *  顶层窗的无障碍元素往往只是一层壳（Chromium 把网页渲染进独立子窗），从顶层窗下钻几层就
	 *  再无子节点、只能拿回整窗矩形。这里枚举**全部**后代子窗，只保留可见且含点的，取祖先层数
	 *  最深者（同深度取面积最小者）。不用 ChildWindowFromPointEx：覆盖整个客户区的合成层会被它
	 *  抢先命中，而合成层没有可用的无障碍子树。判定只看几何与可见性，与目标框架无关。 */
	HWND deepestContainingChild(HWND top, POINT pt)
	{
		struct Pick {
			HWND top{ nullptr };
			HWND best{ nullptr };
			int depth{ -1 };
			LONGLONG area{ 0 };
			POINT pt{};
		} pick;
		pick.top = top;
		pick.pt = pt;
		EnumChildWindows(top, [](HWND h, LPARAM lp) -> BOOL {
			auto* p = reinterpret_cast<Pick*>(lp);
			if (!IsWindowVisible(h)) return TRUE;
			RECT r{};
			if (!GetWindowRect(h, &r) || !rectContainsPt(r, p->pt)) return TRUE;
			int depth = 0;
			for (HWND up = GetParent(h); up && up != p->top && depth < 64; up = GetParent(up)) ++depth;
			const LONGLONG area = (LONGLONG)(r.right - r.left) * (LONGLONG)(r.bottom - r.top);
			if (depth > p->depth || (depth == p->depth && area < p->area)) {
				p->depth = depth;
				p->area = area;
				p->best = h;
			}
			return TRUE;
		}, reinterpret_cast<LPARAM>(&pick));
		return pick.best ? pick.best : top;
	}

	// ---------------- 2) 无障碍树逐层下钻 ----------------
	struct DrillNode {
		IUIAutomationElement* el{ nullptr }; // 持有引用，由 DrillTree 统一释放
		RECT rect{};                         // 已裁剪到窗口范围
		int parent{ -1 };
		bool structural{ false };            // Pane/Group：可能是纯包裹层，回溯时可穿过
		bool expanded{ false };
		bool dead{ false };                  // 取子失败（提供者报错），不再重试
		std::vector<int> children;           // 兄弟顺序 = 绘制顺序
	};

	/** 是否"结构容器"（Pane/Group）—— 只有这类才允许回溯穿过 */
	bool isStructuralKind(IUIAutomationElement* el, IUIAutomationCacheRequest* creq)
	{
		CONTROLTYPEID ct{ 0 };
		const HRESULT hr = creq ? el->get_CachedControlType(&ct) : el->get_CurrentControlType(&ct);
		if (FAILED(hr)) return false;
		return ct == UIA_PaneControlTypeId || ct == UIA_GroupControlTypeId;
	}

	/** 逐层下钻：每层只向提供者要**一层**子节点（缓存请求限 Element|Children 并预缓存矩形/
	 *  屏幕外/控件类型），逐层按需展开，不一次拉整棵树 —— 一次拉整棵树的代价是整页所有节点的
	 *  跨进程编码，几十毫秒起步，而其中绝大多数分支根本不会走。 */
	struct DrillTree {
		IUIAutomation* automation{ nullptr };
		IUIAutomationCacheRequest* creq{ nullptr };
		IUIAutomation2* automation2{ nullptr };
		std::vector<DrillNode> nodes;
		POINT pt{}; // 本次查询点（展开时判定"是否含点"用）

		~DrillTree()
		{
			for (auto& n : nodes)
				if (n.el) n.el->Release();
		}

		/** 按剩余预算给本次跨进程调用设超时（只有 IUIAutomation2 提供；拿不到就跳过） */
		void armTimeout(DWORD remainingMs)
		{
			if (!automation2) return;
			const DWORD ms = (std::max)((DWORD)1, (std::min)(remainingMs, (DWORD)60000));
			automation2->put_ConnectionTimeout(ms);
			automation2->put_TransactionTimeout(ms);
		}

		/** 展开一层子节点。返回 false = 这一层拿不到（预算尽 / 提供者报错）；空子节点是合法的叶子。
		 *  ⚠ 全程按**下标**访问 nodes，绝不持有元素引用：nodes 是 vector，push_back 会把存储搬到
		 *  新地址；一旦引用跨过 push_back，父节点的 children 就写进了已释放的内存 —— 症状是下钻
		 *  永远只展开一层、随后"没有含点子节点"，而每一步看起来都成功。 */
		bool expand(int index, DWORD deadline)
		{
			if (nodes[index].expanded) return true;
			if (nodes[index].dead || !nodes[index].el) return false;
			const LONGLONG remaining = (LONGLONG)deadline - (LONGLONG)GetTickCount();
			if (remaining <= 1) return false;
			armTimeout((DWORD)remaining);

			// 根节点在建树时已经带好一层缓存；其余节点用 BuildUpdatedCache 现取这一层
			IUIAutomationElement* holder{ nullptr };
			if (index == 0) {
				nodes[0].el->AddRef();
				holder = nodes[0].el;
			}
			else if (FAILED(nodes[index].el->BuildUpdatedCache(creq, &holder)) || !holder) {
				nodes[index].dead = true;
				return false;
			}
			IUIAutomationElementArray* arr{ nullptr };
			const HRESULT hr = holder->GetCachedChildren(&arr);
			if (FAILED(hr)) {
				holder->Release();
				nodes[index].dead = true;
				return false;
			}
			const RECT winRect = nodes[0].rect;
			if (arr) {
				int len = 0;
				arr->get_Length(&len);
				for (int i = 0; i < len; ++i) {
					IUIAutomationElement* child{ nullptr };
					if (FAILED(arr->GetElement(i, &child)) || !child) continue;
					BOOL off = FALSE;
					const HRESULT ohr = creq ? child->get_CachedIsOffscreen(&off)
						: child->get_CurrentIsOffscreen(&off);
					RECT br{};
					const HRESULT bhr = creq ? child->get_CachedBoundingRectangle(&br)
						: child->get_CurrentBoundingRectangle(&br);
					if ((SUCCEEDED(ohr) && off) || FAILED(bhr)) {
						child->Release();
						continue;
					}
					// 先按窗口范围裁剪、不相交的直接丢（坐标已失效的陈旧节点都过不了这关），
					// 屏幕外的同样跳过（几何上"含点"，屏幕上并没有它）
					if (rectValid(winRect)) br = intersectRect(br, winRect);
					if (br.right - br.left < 2 || br.bottom - br.top < 2) {
						child->Release();
						continue;
					}
					DrillNode node;
					node.el = child;
					node.rect = br;
					node.parent = index;
					node.structural = isStructuralKind(child, creq);
					const int self = (int)nodes.size();
					nodes.push_back(node);
					nodes[index].children.push_back(self); // 重新取下标：push_back 可能已搬移存储
				}
				arr->Release();
			}
			nodes[index].expanded = true;
			holder->Release();
			return true;
		}

		/** 本层含点的最后一个兄弟；没有返回 -1。
		 *  同一个位置被多个兄弟覆盖时，排在后面的那个是视觉上盖在上面的那个。 */
		int lastContainingChild(int index) const
		{
			const std::vector<int>& sibs = nodes[index].children;
			for (int i = (int)sibs.size() - 1; i >= 0; --i)
				if (rectContainsPt(nodes[sibs[i]].rect, pt)) return sibs[i];
			return -1;
		}

		/** 走死时的回溯：沿"与父同框的结构容器"向上退，找**更早**的、同样含点的兄弟分支。
		 *  实测 Chromium 窗口外层就是这个形状：一个与内容分支等尺寸的包裹层排在它后面，自身和
		 *  子节点都不含点 —— 不回退的话下钻在第一层就断，只能给出整窗框。只认"结构容器 + 与父
		 *  同框"两个条件：真实控件、形状不同的容器框架一律原样保留优先级。 */
		int earlierOverlappingBranch(int index) const
		{
			int cur = index;
			while (true) {
				const int parent = nodes[cur].parent;
				if (parent < 0 || !nodes[cur].structural
					|| !rectEqual(nodes[parent].rect, nodes[cur].rect))
					return -1;
				const std::vector<int>& sibs = nodes[parent].children;
				// 只找排在 cur 之前的兄弟：排在后面的在绘制顺序上压在 cur 之上，不可能被 cur 挡住
				for (int i = 0; i < (int)sibs.size(); ++i) {
					const int ci = sibs[i];
					if (ci == cur) break;
					if (rectContainsPt(nodes[ci].rect, pt)) return ci; // 更早兄弟里最靠后的那个
				}
				cur = parent;
			}
		}
	};

	bool drillWindowTree(IUIAutomation* automation, IUIAutomationCacheRequest* creq,
		IUIAutomation2* automation2, HWND hwnd, POINT pt, DWORD budgetMs, RECT& out)
	{
		if (!automation || !hwnd) return false;
		DrillTree tree;
		tree.automation = automation;
		tree.creq = creq;
		tree.automation2 = automation2;
		tree.pt = pt;

		// 根 = 窗口自身的无障碍元素（ElementFromHandleBuildCache 会顺带缓存一层子节点）
		IUIAutomationElement* root{ nullptr };
		if (creq) {
			if (FAILED(automation->ElementFromHandleBuildCache(hwnd, creq, &root)) || !root)
				return false;
		}
		else if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) {
			return false;
		}
		RECT wr{};
		const RECT winRect = GetWindowRect(hwnd, &wr) ? wr : RECT{ 0, 0, 0, 0 };
		DrillNode rootNode;
		rootNode.el = root; // 引用交给节点表
		rootNode.rect = winRect;
		tree.nodes.push_back(rootNode);

		const DWORD deadline = GetTickCount() + budgetMs;
		constexpr int kMaxSteps = 80;
		int cur = 0;
		for (int step = 0; step < kMaxSteps; ++step) {
			if ((LONGLONG)GetTickCount() >= (LONGLONG)deadline) break;
			if (!tree.expand(cur, deadline)) break;
			const int child = tree.lastContainingChild(cur);
			if (child >= 0) {
				cur = child;
				continue;
			}
			const int alt = tree.earlierOverlappingBranch(cur);
			if (alt < 0) break;
			cur = alt;
		}

		// 终点向上第一个"不是整个窗口"的矩形 = 最具体的那层；全是窗口范围就说明没钻出东西
		for (int i = cur; i >= 0; i = tree.nodes[i].parent) {
			const RECT r = tree.nodes[i].rect;
			if (r.right - r.left < 2 || r.bottom - r.top < 2) continue;
			if (rectValid(winRect) && rectEqual(r, winRect)) continue;
			if (!rectContainsPt(r, pt)) continue;
			out = r;
			return true;
		}
		return false;
	}

	// ---------------- 3) 坐标命中测试（与 F12 选择器同源） ----------------
	// 「临时点穿」是不是我们挂上去的 —— 查询线程加，UI 线程也必须能立刻撤（CutMask::cancelHoverQuery）
	std::atomic<bool> g_pointThroughApplied{ false };
	std::atomic<LONG> g_pointThroughPrevEx{ 0 };
	std::atomic<HWND> g_pointThroughHwnd{ nullptr };

	/** UIA ElementFromPoint：让提供者直接回答"点下是什么元素"，现代 Chrome/Edge 是对其内部
	 *  可访问性树的命中测试，能直达比"窗口根 + 树遍历"更准的 DOM/Shadow DOM 叶节点。
	 *
	 *  ⚠ 命中测试是**窗口级**的：遮罩若参与命中测试，返回的永远只是"遮罩自己的元素"，目标
	 *  自己的命中测试（与 F12 选择器同一套逻辑）就被挡在外面。故只在这一次调用期间给遮罩
	 *  临时加 WS_EX_TRANSPARENT（画面不变、不隐藏、不动 Z 序），查完立刻还原；鼠标有按键
	 *  按下时绝不点穿 —— 那一下会漏给下层窗口。 */
	bool pointHitElement(IUIAutomation* automation, HWND excludeOv, POINT pt, RECT& out)
	{
		if (!automation) return false;
		IUIAutomationElement* el{ nullptr };
		LONG ovEx{ 0 };
		bool passthrough{ false };
		if (excludeOv && IsWindow(excludeOv)
			&& !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)
			&& !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)
			&& !(GetAsyncKeyState(VK_MBUTTON) & 0x8000)) {
			ovEx = GetWindowLongW(excludeOv, GWL_EXSTYLE);
			if (!(ovEx & WS_EX_TRANSPARENT)) {
				g_pointThroughPrevEx.store(ovEx);
				g_pointThroughHwnd.store(excludeOv);
				SetWindowLongW(excludeOv, GWL_EXSTYLE, ovEx | WS_EX_TRANSPARENT);
				g_pointThroughApplied.store(true);
				passthrough = true;
			}
			// 遮罩本来就带 WS_EX_TRANSPARENT（用户开了穿透）：绝不能"还原"，那会把穿透状态抹掉
		}
		const HRESULT phr = automation->ElementFromPoint(pt, &el);
		if (passthrough) {
			// UI 线程可能已经"按下即撤"抢先还原过了：那就绝不能再改一次样式 —— 重复的
			// GWL_EXSTYLE 变更会让窗口重新合成，等于把"闪一下"又带回来
			if (g_pointThroughApplied.exchange(false))
				SetWindowLongW(excludeOv, GWL_EXSTYLE, ovEx);
		}
		if (FAILED(phr) || !el) return false;
		RECT r{};
		const HRESULT rhr = el->get_CurrentBoundingRectangle(&r);
		const bool phantom = isPhantomElement(el);
		const long pid = intProp(el, UIA_ProcessIdPropertyId);
		UIA_HWND hwndVal{ nullptr };
		el->get_CurrentNativeWindowHandle(&hwndVal);
		el->Release();
		if (FAILED(rhr) || !rectValid(r)) return false;
		if (phantom || !rectContainsPt(r, pt)) return false;
		// 全屏根容器/桌面：不采纳，交给下层细化
		const RECT virt{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
			GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
			GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
		if (rectValid(virt) && (r.right - r.left) >= (virt.right - virt.left) - 2
			&& (r.bottom - r.top) >= (virt.bottom - virt.top) - 2)
			return false;
		// 自家进程的元素一律不采纳：命中测试是**全局**命中的，光标下最顶层就是我们的遮罩，它基本
		// 总会返回"遮罩自己"的元素；这类元素的包围盒在页面上对应不到任何东西，就是"莫须有元素"。
		if (pid > 0 && pid == (long)GetCurrentProcessId()) return false;
		if (hwndVal) {
			const HWND h = reinterpret_cast<HWND>(hwndVal);
			if (h == excludeOv || isOwnProcess(h) || isExcludedCaptureWindow(h)) return false;
		}
		out = r;
		return true;
	}

	/** MSAA 机会回退：同样是"提供者自己的命中测试"，不依赖树遍历。现代 Chrome/Edge 是纯 UIA
	 *  provider，AccessibleObjectFromWindow(OBJID_CLIENT) 直接失败 → 本函数快速返回空；老
	 *  WebView / 某些第三方浏览器 / Firefox 树异常时，accHitTest 可补 UIA 的不足。 */
	RECT msaaHitAt(HWND hwnd, POINT pt)
	{
		if (!hwnd || !IsWindow(hwnd)) return RECT{ 0, 0, 0, 0 };
		IAccessible* acc{ nullptr };
		if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,
			reinterpret_cast<void**>(&acc))) || !acc)
			return RECT{ 0, 0, 0, 0 };
		VARIANT self{};
		VariantInit(&self);
		self.vt = VT_I4;
		self.lVal = CHILDID_SELF;
		VARIANT hit{};
		VariantInit(&hit);
		RECT out{ 0, 0, 0, 0 };
		auto acceptRect = [&](long l, long t, long w, long h) {
			if (w <= 0 || h <= 0) return;
			const RECT r{ l, t, l + w, t + h };
			if (rectContainsPt(r, pt) && w >= 2 && h >= 2) out = r;
		};
		const HRESULT hhr = acc->accHitTest(pt.x, pt.y, &hit);
		if (SUCCEEDED(hhr) && hit.vt == VT_DISPATCH && hit.pdispVal) {
			// accHitTest 直接返回了子对象本身（VT_DISPATCH）
			IAccessible* sub{ nullptr };
			if (SUCCEEDED(hit.pdispVal->QueryInterface(IID_IAccessible,
				reinterpret_cast<void**>(&sub))) && sub) {
				long l = 0, t = 0, w = 0, h = 0;
				if (SUCCEEDED(sub->accLocation(&l, &t, &w, &h, self))) acceptRect(l, t, w, h);
				sub->Release();
			}
		}
		else if (SUCCEEDED(hhr) && hit.vt == VT_I4) {
			if (hit.lVal == CHILDID_SELF) {
				long l = 0, t = 0, w = 0, h = 0;
				if (SUCCEEDED(acc->accLocation(&l, &t, &w, &h, self))) acceptRect(l, t, w, h);
			}
			else {
				// 命中子对象 ID：先父对象带子 ID 直查，不支持再 get_accChild 换独立实例
				VARIANT cv{};
				VariantInit(&cv);
				cv.vt = VT_I4;
				cv.lVal = hit.lVal;
				long l = 0, t = 0, w = 0, h = 0;
				if (SUCCEEDED(acc->accLocation(&l, &t, &w, &h, cv))) {
					acceptRect(l, t, w, h);
				}
				else {
					IDispatch* disp{ nullptr };
					if (SUCCEEDED(acc->get_accChild(cv, &disp)) && disp) {
						IAccessible* childAcc{ nullptr };
						if (SUCCEEDED(disp->QueryInterface(IID_IAccessible,
							reinterpret_cast<void**>(&childAcc))) && childAcc) {
							long l2 = 0, t2 = 0, w2 = 0, h2 = 0;
							if (SUCCEEDED(childAcc->accLocation(&l2, &t2, &w2, &h2, self)))
								acceptRect(l2, t2, w2, h2);
							childAcc->Release();
						}
						disp->Release();
					}
				}
				VariantClear(&cv);
			}
		}
		VariantClear(&self);
		VariantClear(&hit);
		acc->Release();
		return out;
	}

	// ---------------- 4) 桌面图标 ----------------
	/** 桌面图标宿主（SysListView32）定位：桌面层在元素查找主链里被 isDesktopOrShell 整体排除
	 *  （否则会选中壁纸/整屏），这里在 Progman / WorkerW 及其全部后代里找覆盖 pt 的可见列表窗。 */
	HWND desktopIconListViewAt(POINT pt, HWND exclude)
	{
		struct Ctx {
			POINT pt;
			HWND exclude;
			HWND hit{ nullptr };
		} c{ pt, exclude, nullptr };

		EnumWindows([](HWND top, LPARAM lp) -> BOOL {
			auto* cc = reinterpret_cast<Ctx*>(lp);
			if (top == cc->exclude || !IsWindowVisible(top) || IsIconic(top)) return TRUE;
			wchar_t cls[256]{};
			if (GetClassNameW(top, cls, 256) <= 0) return TRUE;
			if (wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0) return TRUE;
			RECT tr{};
			if (!GetWindowRect(top, &tr) || !rectContainsPt(tr, cc->pt)) return TRUE;
			EnumChildWindows(top, [](HWND child, LPARAM lp2) -> BOOL {
				auto* cc2 = reinterpret_cast<Ctx*>(lp2);
				if (child == cc2->exclude || !IsWindowVisible(child)) return TRUE;
				wchar_t ccls[256]{};
				if (GetClassNameW(child, ccls, 256) <= 0) return TRUE;
				if (wcscmp(ccls, L"SysListView32") != 0) return TRUE;
				RECT cr{};
				if (!GetWindowRect(child, &cr) || !rectContainsPt(cr, cc2->pt)) return TRUE;
				cc2->hit = child;
				return FALSE;
			}, reinterpret_cast<LPARAM>(cc));
			return cc->hit ? FALSE : TRUE;
		}, reinterpret_cast<LPARAM>(&c));
		return c.hit;
	}

	/** 一次性扫出桌面图标列表全部直接子项矩形，供上层缓存 + 本地命中。只取直接子项、不下钻到
	 *  图标位图/名称文本，命中粒度就是整个"图标 + 名称"项（与资源管理器选中态一致）。
	 *  跨进程 RPC 由 200 项 / 50ms 双上限兜住。 */
	bool scanDesktopIconItems(IUIAutomation* automation, IUIAutomationCacheRequest* creq, HWND list,
		std::vector<RECT>& outRects)
	{
		outRects.clear();
		if (!automation || !list) return false;
		IUIAutomationElement* root{ nullptr };
		if (creq) {
			if (FAILED(automation->ElementFromHandleBuildCache(list, creq, &root)) || !root) return false;
		}
		else if (FAILED(automation->ElementFromHandle(list, &root)) || !root) return false;

		auto getRect = [creq](IUIAutomationElement* el, RECT* br) -> bool {
			if (creq && SUCCEEDED(el->get_CachedBoundingRectangle(br))) return true;
			return SUCCEEDED(el->get_CurrentBoundingRectangle(br));
		};

		bool ok = false;
		RECT rr{};
		if (getRect(root, &rr)) { // 根可读即视为列表可枚举（空桌面 = 可枚举但零项）
			IUIAutomationTreeWalker* walker{ nullptr };
			if (SUCCEEDED(automation->get_ControlViewWalker(&walker)) && walker) {
				IUIAutomationElement* child{ nullptr };
				const HRESULT fhr = creq ? walker->GetFirstChildElementBuildCache(root, creq, &child)
					: walker->GetFirstChildElement(root, &child);
				if (SUCCEEDED(fhr)) {
					ok = true;
					const DWORD t0 = GetTickCount();
					int guard = 0;
					while (child && guard++ < 200) {
						if (GetTickCount() - t0 > 50u) {
							child->Release();
							child = nullptr;
							break;
						}
						RECT cr{};
						if (getRect(child, &cr) && rectValid(cr)
							&& cr.right - cr.left >= 4 && cr.bottom - cr.top >= 4)
							outRects.push_back(cr);
						IUIAutomationElement* next{ nullptr };
						if (creq) walker->GetNextSiblingElementBuildCache(child, creq, &next);
						else walker->GetNextSiblingElement(child, &next);
						child->Release();
						child = next;
					}
				}
				walker->Release();
			}
		}
		root->Release();
		return ok;
	}

	/** 图标布局在数百毫秒内基本不变：缓存后每次 hover 只做本地"含点最小项"比对，不再每次
	 *  查询都跨进程枚举上百个子项（那是桌面查找元素卡顿/阻塞感的来源）。 */
	RECT desktopIconItemRectAt(POINT pt, HWND list, IUIAutomation* automation,
		IUIAutomationCacheRequest* creq)
	{
		struct Cache {
			std::vector<RECT> rects;
			HWND list{ nullptr };
			ULONGLONG stamp{ 0 };
		};
		static Cache cache; // 只在查询线程访问，无需加锁
		constexpr ULONGLONG kIconCacheMs{ 800 };
		const ULONGLONG now = GetTickCount64();
		if (list != cache.list || now - cache.stamp > kIconCacheMs) {
			std::vector<RECT> rects;
			if (scanDesktopIconItems(automation, creq, list, rects)) {
				cache.rects.swap(rects);
				cache.list = list;
				cache.stamp = now;
			}
			else {
				cache.list = nullptr; // 列表读不到：不留旧缓存，下次查询重试
				cache.rects.clear();
			}
		}
		if (cache.list != list) return RECT{ 0, 0, 0, 0 };
		RECT best{};
		for (const RECT& r : cache.rects) {
			if (rectContainsPt(r, pt) && (!rectValid(best) || rectArea(r) < rectArea(best)))
				best = r;
		}
		return best;
	}

	// ---------------- 5) 无障碍树惰性激活：机会性脉冲 ----------------
	// 唤醒只是兼容尝试，绝不是识别成功的前提；状态跨查询保持（只在查询线程用）
	constexpr int kMaxWakeAttempts{ 5 };
	constexpr ULONGLONG kWakeCooldownMs{ 300 };
	HWND g_wakeTop{ nullptr };
	ULONGLONG g_wakeMs{ 0 };
	int g_wakeCount{ 0 };
	bool g_lastCoarse{ false };

	/** 无障碍树是惰性的：目标进程收到首个 WM_GETOBJECT 之后才异步建树（实测 Chromium/Firefox
	 *  都要数百 ms）。树没建好时下钻只能停在整块级大节点上，故本轮没拿到具体元素时补一次脉冲。
	 *  同一窗口只在"上一轮仍是粗框、冷却期满、未达次数上限"时补，连续几次仍粗框即停，等鼠标
	 *  移动/切标签/换窗口再试。 */
	void nudgeAccessibility(HWND anchor, bool prevCoarse)
	{
		const ULONGLONG now = GetTickCount64();
		const bool newTop = anchor != g_wakeTop;
		if (newTop) {
			g_wakeTop = anchor;
			g_wakeCount = 0;
		}
		if (!newTop && !(prevCoarse && now - g_wakeMs >= kWakeCooldownMs
			&& g_wakeCount < kMaxWakeAttempts))
			return;
		g_wakeCount = newTop ? 1 : g_wakeCount + 1;
		g_wakeMs = now;
		if (anchor && IsWindow(anchor)) {
			DWORD_PTR res{ 0 };
			// SMTO_ABORTIFHUNG + 120ms：目标进程卡死也只等 120ms，绝不拖住查找线程
			SendMessageTimeoutW(anchor, WM_GETOBJECT, 0, OBJID_CLIENT, SMTO_ABORTIFHUNG, 120, &res);
		}
	}

	// ---------------- 6) 主链 ----------------
	/** 命中 screen 下最小可见元素包围盒（屏幕坐标）。coarse 回写"结果是否仍是整块级粗框"
	 *  （浏览器树尚未就绪/页面未加载完，需要悬停驻留继续重查直到细化）。
	 *  refine=false = 实时命中（鼠标还在移动，下钻给更小预算，必须尽快出结果）；
	 *  refine=true  = 驻留重查（同一固定点稳定后的重查，下钻用完整预算，钻得更深更准）。 */
	bool elementAtRaw(POINT screen, HWND exclude, bool refine, RECT& out, bool& coarse)
	{
		const bool prevCoarse = g_lastCoarse;
		g_lastCoarse = false;
		coarse = false;
		auto& ctx = uiaCtx();
		IUIAutomation* automation = ctx.automation;
		IUIAutomationCacheRequest* creq = ctx.cacheRequest;

		// EnumWindows 定顶层窗：遮罩自身 / 不可见 / 最小化 / 幽灵窗 / 桌面外壳一律排除，
		// 全程无窗口样式改动、零闪烁
		struct Ctx {
			POINT pt;
			HWND exclude;
			HWND found{ nullptr };
		} c{ screen, exclude, nullptr };
		EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
			auto* cc = reinterpret_cast<Ctx*>(lp);
			if (isBadWindow(hwnd, cc->exclude)) return TRUE;
			RECT r{};
			if (!GetWindowRect(hwnd, &r) || !rectContainsPt(r, cc->pt)) return TRUE;
			cc->found = hwnd;
			return FALSE;
		}, reinterpret_cast<LPARAM>(&c));

		if (!c.found) {
			// 桌面图标：桌面层(Progman/WorkerW)在主链里被整体跳过，顶层窗必为空。单独定位
			// SysListView32 后取"图标+名称"项，可让查找元素命中桌面的文件/文件夹；空白桌面
			// 无项则退回整屏兜底，不误报元素（全程不碰遮罩样式）。
			const HWND list = automation ? desktopIconListViewAt(screen, exclude) : nullptr;
			if (list) {
				const RECT r = desktopIconItemRectAt(screen, list, automation, creq);
				if (rectValid(r) && rectContainsPt(r, screen)) {
					out = r;
					return true;
				}
			}
			out = monitorRectAt(screen);
			coarse = true;
			return true;
		}

		const HWND anchor = deepestContainingChild(c.found, screen);
		const RECT anchorBounds = hwndBoundsRect(anchor);

		if (automation) {
			// 实时档 168ms（鼠标还在移动）；驻留档 1500ms（停稳了，可以钻深些）
			const DWORD budgetMs = refine ? 1500u : 168u;
			RECT drilled{};
			const bool drilledOk = drillWindowTree(automation, creq, ctx.automation2, anchor,
				screen, budgetMs, drilled);
			// 下钻回"整块级"矩形 = 其实没钻进去（多数情况是无障碍树还没建好）。这时绝不能当
			// 答案发布：那层壳会盖住真实元素，而且发布之后命中测试与无障碍脉冲两个真正能救场
			// 的分支就再也走不到了。
			if (drilledOk && !isPageLevelRect(drilled, anchorBounds)) {
				out = drilled;
				return true;
			}

			// 下钻钻不进去时，命中测试仍是可靠答案（浏览器标题栏/标签栏这类不在渲染子树里的
			// 控件也只有它能给）
			RECT hit{};
			if (pointHitElement(automation, exclude, screen, hit)
				&& !isPageLevelRect(hit, anchorBounds)) {
				out = hit;
				return true;
			}

			// MSAA 机会回退：同样是"提供者自己的命中测试"，不依赖树遍历；失败即空、零副作用
			const RECT ma = msaaHitAt(anchor, screen);
			if (rectValid(ma) && ma.right - ma.left >= 4 && ma.bottom - ma.top >= 4
				&& !isPageLevelRect(ma, anchorBounds)) {
				out = ma;
				return true;
			}

			// 到这里说明只能用"整块级"结果兜底 → 树可能还没建好，给锚点窗补一次脉冲，
			// 下一轮驻留重查再试
			nudgeAccessibility(anchor, prevCoarse);

			if (drilledOk && rectValid(drilled)
				&& drilled.right - drilled.left >= 4 && drilled.bottom - drilled.top >= 4) {
				out = drilled;
				coarse = true;
				return true;
			}
		}

		RECT r{};
		if (!hwndBounds(anchor, r) || r.right - r.left < 4 || r.bottom - r.top < 4)
			hwndBounds(c.found, r);
		out = r;
		coarse = true;
		return rectValid(r);
	}

	bool elementAt(POINT screen, HWND exclude, bool refine, RECT& out, bool& coarse)
	{
		const bool ok = elementAtRaw(screen, exclude, refine, out, coarse);
		// 按"光标所在那块屏幕"裁剪：不少容器节点的布局盒远大于屏幕（实测某列容器 1978×28081 =
		// 整个文档高度），整框看着就像"没找到元素"。只保留屏幕上真正看得见的那部分，与浏览器
		// F12 选择器的高亮行为一致；屏幕内的元素矩形不受影响。
		const RECT mon = monitorRectAt(screen);
		if (ok && rectValid(out) && rectValid(mon)) out = intersectRect(out, mon);
		return ok;
	}

	// ——— 元素查询工作线程 ———
	// 查询是跨进程调用（UIA/MSAA）。浏览器第一次被问时会现建无障碍树，实测冷启要几百毫秒 ——
	// 以前在 UI 线程上直接查，于是"头一次悬停/框选整个界面卡住，过一会儿才顺"（像要热机）。
	// 现在查询一律丢给工作线程，结果异步回 UI 线程：UI 线程从投递到应用结果全程不被阻塞。
	namespace HoverQuery
	{
		constexpr UINT kMsgResult{ WM_APP + 0x431 };
		// coarse = 结果仍是"整块级粗框"（无障碍树还没建好），UI 侧据此武装驻留细化重查
		using Sink = std::function<void(const RECT& rect, bool ok, uint64_t seq, bool coarse)>;

		std::mutex g_mtx;
		std::condition_variable g_cv;
		bool g_pending{ false };
		bool g_stop{ false };
		bool g_started{ false };
		POINT g_req{};
		HWND g_reqExclude{ nullptr }; // 遮罩自身 HWND：命中测试必须排除掉，否则命中"莫须有"
		bool g_reqRefine{ false };    // 驻留细化档：下钻用完整预算，钻得更深
		uint64_t g_reqSeq{ 0 };
		uint64_t g_seqSource{ 0 };
		HWND g_ui{ nullptr };
		HANDLE g_thread{ nullptr };
		// 只在 UI 线程读写（投递方与结果回调都是 UI 线程）
		Sink g_sink;

		struct Payload {
			RECT rect{};
			bool ok{ false };
			bool coarse{ false };
			uint64_t seq{ 0 };
		};

		LRESULT CALLBACK resultProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			if (msg == kMsgResult) {
				std::unique_ptr<Payload> p{ reinterpret_cast<Payload*>(lp) };
				if (p && g_sink) g_sink(p->rect, p->ok, p->seq, p->coarse);
				return 0;
			}
			return DefWindowProcW(hwnd, msg, wp, lp);
		}

		void workerMain()
		{
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			std::unique_lock lk(g_mtx);
			while (true) {
				g_cv.wait(lk, [] { return g_stop || g_pending; });
				if (g_stop) break;
				const POINT pt = g_req;
				const HWND exclude = g_reqExclude;
				const bool refine = g_reqRefine;
				const uint64_t seq = g_reqSeq;
				g_pending = false;   // 只做"最新那一次"：中途来的新请求直接覆盖，不排队
				HWND ui = g_ui;
				lk.unlock();

				RECT rect{};
				bool coarse = false;
				const bool ok = elementAt(pt, exclude, refine, rect, coarse);

				lk.lock();
				if (g_stop || !ui) continue;
				auto* payload = new Payload{ rect, ok, coarse, seq };
				if (!PostMessageW(ui, kMsgResult, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
			}
			lk.unlock();
			// thread_local 的 UIA 实例故意不释放：线程退到进程尾巴上，释放反而容易踩 COM 关闭顺序
			CoUninitialize();
		}

		void ensureStarted()
		{
			if (g_started) return;
			g_started = true;
			static bool registered = false;
			if (!registered) {
				WNDCLASSEXW wc{ sizeof(wc) };
				wc.lpfnWndProc = resultProc;
				wc.hInstance = GetModuleHandleW(nullptr);
				wc.lpszClassName = L"SnowAirHoverQuery";
				registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
			}
			if (registered) {
				// 结果回传用的隐藏窗口必须建在 UI 线程上（此处就是）
				g_ui = CreateWindowExW(0, L"SnowAirHoverQuery", L"", 0, 0, 0, 0, 0,
					HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
			}
			g_thread = CreateThread(nullptr, 0,
				[](LPVOID) -> DWORD { workerMain(); return 0; }, nullptr, 0, nullptr);
		}

		void setSink(Sink sink) { g_sink = std::move(sink); }

		uint64_t request(POINT screen, HWND exclude, bool refine)
		{
			ensureStarted();
			uint64_t seq = 0;
			{
				std::lock_guard lk(g_mtx);
				g_req = screen;
				g_reqExclude = exclude;
				g_reqRefine = refine;
				seq = ++g_seqSource;
				g_reqSeq = seq;
				g_pending = true;
			}
			g_cv.notify_one();
			return seq;
		}

		/** 作废在途查询：把序号推到比任何已投递请求都新的值，之后到达的旧结果由调用方按序号丢弃。 */
		uint64_t cancelPending()
		{
			std::lock_guard lk(g_mtx);
			g_pending = false;
			return ++g_seqSource;
		}
	}

	bool ptIn(const D2D1_RECT_F& r, float x, float y)
	{
		return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
	}

	float clampRadiusToRect(float radius, float w, float h)
	{
		if (radius <= 0.f) return 0.f;
		return std::min(radius, std::min(w, h) * 0.5f);
	}
}

CutMask::CutMask(Ling::WinBase* win) :win{ win }
{
	// 查询线程的结果回到 UI 线程后在这里落地：序号比当前投递旧的结果直接丢（光标早已经移开）
	HoverQuery::setSink([this](const RECT& r, bool ok, uint64_t seq, bool coarse) {
		// 结果回来 = 查询结束，输入保护可以撤。只有"最新那一次"的结果才算信箱排空；
		// 过期请求被"只做最新一次"合并丢弃时没有结果回来，靠 hoverGuardTimerId 的看门狗兜底。
		if (seq == hoverSeq_) endQueryGuard();
		if (!this->win || seq < hoverSeq_) return;
		hoverCoarse_ = coarse;
		const D2D1_RECT_F next = ok ? clientRectFromScreen(r, this->win) : D2D1::RectF();
		// 结果没变（含 1~2px 抖动）：仍是粗框就保留驻留重查，直到细化到位或预算耗尽
		const bool same = rectEq(hoverTarget_, next)
			|| (hoverTarget_.right > hoverTarget_.left && rectAlmost(hoverTarget_, next)
				&& rectContains(next, lastHoverQueryPos_));
		if (!same) setHoverTarget(next, true);
		if (coarse) scheduleHoverRefine();
		else stopHoverRefine();
	});
	strokeWidth = 2 * win->dpi;
	paddingTop *= win->dpi;
	paddingMargin *= win->dpi;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(Ling::Color(ToolbarTheme::infoText).getD2DColor(), brushText.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(Ling::Color(ToolbarTheme::infoBg).getD2DColor(), brushInfoBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x34C759), brushBorder.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushHandleFill.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(Ling::Color(ToolbarTheme::infoIconMuted).getD2DColor(), brushIcon.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(Ling::Color(ToolbarTheme::sliderTrack).getD2DColor(), brushSliderTrack.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(Ling::Color(ToolbarTheme::sliderFill).getD2DColor(), brushSliderFill.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(Ling::Color(ToolbarTheme::sliderThumb).getD2DColor(), brushSliderThumb.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(Ling::Color(selShadowColor_).getD2DColor(), brushSelShadow.GetAddressOf());
}

CutMask::~CutMask()
{
	// 先撤掉查询留下的状态（临时点穿 / 临时捕获 / 驻留重查），再摘回填：
	// 工作线程可能还有一趟结果在路上（那个回调捕获了 this）
	cancelHoverQuery();
	HoverQuery::setSink(nullptr);
}

bool CutMask::rectEq(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float eps)
{
	return std::fabs(a.left - b.left) < eps && std::fabs(a.top - b.top) < eps
		&& std::fabs(a.right - b.right) < eps && std::fabs(a.bottom - b.bottom) < eps;
}

bool CutMask::rectAlmost(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
{
	return std::fabs(a.left - b.left) <= 2.f && std::fabs(a.top - b.top) <= 2.f
		&& std::fabs((a.right - a.left) - (b.right - b.left)) <= 3.f
		&& std::fabs((a.bottom - a.top) - (b.bottom - b.top)) <= 3.f;
}

bool CutMask::rectContains(const D2D1_RECT_F& r, POINT pos)
{
	return pos.x >= r.left && pos.x < r.right && pos.y >= r.top && pos.y < r.bottom;
}

void CutMask::clearHoverAnim()
{
	hoverAnimating_ = false;
	if (win) win->killTimer(hoverAnimTimerId);
}

void CutMask::snapHoverTarget()
{
	clearHoverAnim();
	if (hoverTarget_.right > hoverTarget_.left && hoverTarget_.bottom > hoverTarget_.top)
		maskRect = hoverTarget_;
}

void CutMask::setHoverTarget(const D2D1_RECT_F& target, bool animate)
{
	hoverTarget_ = target;
	const float tw = target.right - target.left;
	const float th = target.bottom - target.top;
	if (tw < 2.f || th < 2.f) {
		clearHoverAnim();
		maskRect = {};
		makeLayout();
		win->refresh();
		return;
	}
	const float vw = maskRect.right - maskRect.left;
	// 首次出现：直接贴合，避免从 0 放大
	if (!animate || vw < 1.f) {
		clearHoverAnim();
		maskRect = target;
		makeLayout();
		win->refresh();
		return;
	}
	if (rectEq(maskRect, target, 0.35f)) {
		maskRect = target;
		clearHoverAnim();
		return;
	}
	// 只更新目标，显示矩形继续追赶 —— 不重开 from→to，避免 Out 缓动起步发冲
	hoverAnimating_ = true;
	win->setTimer(hoverTickMs, hoverAnimTimerId);
}

void CutMask::onHoverAnimTick(UINT timerId)
{
	if (timerId != hoverAnimTimerId) return;
	if (!hoverAnimating_) {
		if (win) win->killTimer(hoverAnimTimerId);
		return;
	}
	const float dt = (float)hoverTickMs / 1000.f;
	const float a = 1.f - std::exp(-hoverChaseRate * dt);
	auto chase = [a](float& v, float to) { v += (to - v) * a; };
	chase(maskRect.left, hoverTarget_.left);
	chase(maskRect.top, hoverTarget_.top);
	chase(maskRect.right, hoverTarget_.right);
	chase(maskRect.bottom, hoverTarget_.bottom);
	if (rectEq(maskRect, hoverTarget_, 0.4f)) {
		maskRect = hoverTarget_;
		clearHoverAnim();
	}
	makeLayout();
	win->refresh();
}

bool CutMask::highlight(POINT pos)
{
	const ULONGLONG now = GetTickCount64();
	const int moved = std::abs(pos.x - lastHoverQueryPos_.x) + std::abs(pos.y - lastHoverQueryPos_.y);
	// 元素查询是 UIA 跨进程调用（实测 Chrome 上 2~10ms 一次，深一点的 DOM 还要钻子节点）。
	// 原来的节流是「间隔不够 **且** 位移不够」才跳过 —— 鼠标一动每次 WM_MOUSEMOVE 都会去查
	// （125Hz 鼠标就是一秒上百次 UIA 调用），UI 线程被占满：悬停框跟不上、整屏重绘也卡，
	// 体感就是"查找元素的时候移动卡卡的"。
	// 现在改成：不管鼠标动多快都有硬下限；已经在当前目标框内时更慢（那只是在找更细的子节点）。
	constexpr ULONGLONG kMinIntervalMs{ 24 };
	constexpr ULONGLONG kInsideIntervalMs{ 90 };
	constexpr int kInsideMinMovePx{ 14 };
	if (lastHoverQueryMs_ > 0 && now - lastHoverQueryMs_ < kMinIntervalMs)
		return false;
	const bool inside = hoverTarget_.right > hoverTarget_.left && rectContains(hoverTarget_, pos);
	if (inside && lastHoverQueryMs_ > 0 && now - lastHoverQueryMs_ < kInsideIntervalMs
		&& moved < kInsideMinMovePx)
		return false;

	// 元素查询丢给工作线程（第一次问浏览器可能要几百毫秒，见 HoverQuery 的说明）。
	// 这里只记下"最新一次问的是哪儿"，结果回来时按序号判断是否还有效。
	// 投递前上"输入保护"：命中测试会给遮罩临时加 WS_EX_TRANSPARENT，那一瞬间鼠标按下会
	// 漏给下层窗口，用临时捕获兜住（结果回来 / 150ms 看门狗 / 按下时立刻撤）。
	beginQueryGuard();
	lastHoverQueryMs_ = now;
	lastHoverQueryPos_ = pos;
	hoverRefineTries_ = 0;   // 用户再次移动鼠标：重置驻留细化预算
	stopHoverRefine();       // 新的实时命中投出，原驻留细化定时器作废
	hoverSeq_ = HoverQuery::request(POINT{ pos.x + win->x, pos.y + win->y }, win->hwnd, false);
	return true;
}

void CutMask::beginQueryGuard()
{
	if (queryGuard_) return;
	if (!win || !win->hwnd || !IsWindow(win->hwnd)) return;
	// 已经按着鼠标（正在拖拽/画框）：绝不抢捕获
	if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return;
	// 捕获是线程态：只有拥有窗口的 UI 线程调用 GetCapture()/SetCapture() 才有效
	if (GetCapture() == win->hwnd) return; // 已经在自己手上
	SetCapture(win->hwnd);
	queryGuard_ = true;
	win->setTimer(kQueryGuardWatchdogMs, hoverGuardTimerId);
}

void CutMask::endQueryGuard()
{
	if (!queryGuard_) return;
	queryGuard_ = false;
	if (win) win->killTimer(hoverGuardTimerId);
	if (!win || !win->hwnd) return;
	// 查询期间用户真的按下了：这一下已被捕获送到本窗口，后续由 WinCap 自己 SetCapture 接管，
	// 这里就别再动捕获 —— ReleaseCapture 反而会把刚建立的拖拽捕获弄丢。
	if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return;
	if (GetCapture() == win->hwnd) ReleaseCapture();
}

void CutMask::cancelHoverQuery()
{
	stopHoverRefine();
	hoverCoarse_ = false;
	hoverRefineTries_ = 0;
	// 在途结果全部作废：按下之后 hover 高亮不该再来改矩形（会跟拖框写的 maskRect 打架）
	hoverSeq_ = HoverQuery::cancelPending();
	if (queryGuard_) {
		queryGuard_ = false;
		if (win) win->killTimer(hoverGuardTimerId);
		if (win && win->hwnd && GetCapture() == win->hwnd) ReleaseCapture();
	}
	// 命中测试的临时点穿若正挂着（工作线程刚加上）：UI 线程立刻还原，否则这一下按下会漏出去。
	// 两个线程谁先 exchange 成功谁负责还原，绝不会重复改样式（重复改会让窗口重新合成、闪一下）。
	if (g_pointThroughApplied.exchange(false)) {
		const HWND ov = g_pointThroughHwnd.load();
		if (ov && IsWindow(ov)) SetWindowLongW(ov, GWL_EXSTYLE, g_pointThroughPrevEx.load());
	}
}

void CutMask::scheduleHoverRefine()
{
	// 驻留细化预算：连续 5 次（每次 120ms + 一次细化查询）仍粗框就停 —— 与引擎侧
	// kMaxWakeAttempts=5 对齐，避免"页面本就无更细节点"时持续空转；鼠标一动就重置预算。
	if (hoverRefineTries_ >= 5 || hoverRefineTimerOn_) return;
	if (!(hoverTarget_.right > hoverTarget_.left) || !rectContains(hoverTarget_, lastHoverQueryPos_))
		return; // 光标已不在当前粗框内：以新请求为准，别对陈旧位置空转
	++hoverRefineTries_;
	hoverRefineTimerOn_ = true;
	win->setTimer(kHoverRefineMs, hoverRefineTimerId);
}

void CutMask::stopHoverRefine()
{
	if (!hoverRefineTimerOn_) return;
	hoverRefineTimerOn_ = false;
	if (win) win->killTimer(hoverRefineTimerId);
}

void CutMask::onHoverDwellTimer(UINT timerId)
{
	if (timerId == hoverGuardTimerId) {
		// 看门狗：请求被"只做最新一次"合并丢弃时不会有结果回来，捕获得自己放手
		endQueryGuard();
		return;
	}
	if (timerId != hoverRefineTimerId) return;
	stopHoverRefine();
	if (!win || !win->hwnd || !hoverCoarse_) return;
	if (!(hoverTarget_.right > hoverTarget_.left) || !rectContains(hoverTarget_, lastHoverQueryPos_))
		return;
	// 鼠标仍在动（离上次查询点超 6px）：本轮驻留期作废 —— 细化是重树遍历，只该发生在
	// 同一点真正停稳之后，移动中的细化会被"只做最新一次"直接丢掉
	POINT cur{};
	if (!GetCursorPos(&cur)) return;
	ScreenToClient(win->hwnd, &cur);
	if (std::abs(cur.x - lastHoverQueryPos_.x) > 6 || std::abs(cur.y - lastHoverQueryPos_.y) > 6)
		return;
	beginQueryGuard();
	hoverSeq_ = HoverQuery::request(POINT{ lastHoverQueryPos_.x + win->x, lastHoverQueryPos_.y + win->y },
		win->hwnd, true);
}

ComPtr<IDWriteTextLayout> CutMask::makeIconLayout(const wchar_t* code, float slot)
{
	ComPtr<IDWriteTextLayout> out;
	auto d2d = Ling::D2D::get();
	// 必须用绑了 customFontCollection 的 format；baseTextFormat 再 SetFontFamilyName 找不到图标字
	auto* format = d2d->getTextFormat(Icon::Family);
	d2d->dwriteFactory->CreateTextLayout(code, 1, format, slot, slot, out.GetAddressOf());
	if (out) {
		out->SetFontSize(Icon::SizeSm * win->dpi, { 0, 1 });
		out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}
	return out;
}

void CutMask::makeLayout()
{
	// 追赶动画中：尺寸文案用逻辑目标，避免 W×H 每帧跳变把信息栏撑抖
	const D2D1_RECT_F& labelR = (hoverTarget_.right > hoverTarget_.left)
		? hoverTarget_ : maskRect;
	const int ix = (int)std::lround(labelR.left);
	const int iy = (int)std::lround(labelR.top);
	const int iw = (int)std::lround(labelR.right - labelR.left);
	const int ih = (int)std::lround(labelR.bottom - labelR.top);
	const float gap = strokeWidth + 2.f * win->dpi;
	const float barH = ToolbarTheme::infoHeight * win->dpi;
	const float padX = ToolbarTheme::infoPadX * win->dpi;
	const float iconSlot = ToolbarTheme::infoIconSlot * win->dpi;
	const float iconGap = ToolbarTheme::infoIconGap * win->dpi;
	const float sliderW = ToolbarTheme::sliderWidth * win->dpi;
	const float valueW = ToolbarTheme::sliderValueWidth * win->dpi;
	const float valueGap = ToolbarTheme::sliderValueGap * win->dpi; // 与 PropSlider 滑条↔数值间距一致
	const float splitGap = ToolbarTheme::infoSplitterGap * win->dpi;
	const float swatch = swatchSize * win->dpi;
	const float swatchGap = ToolbarTheme::infoSwatchGap * win->dpi;
	const bool showRadiusSlider = selRadius_ > 0.f;
	const bool showSwatch = selShadowW_ > 0;

	auto placeBar = [&](float posW, float sizeW) {
		// X Y | 锁 | W×H px | 分隔 | 圆角 [滑条+数值] | 阴影 [色块]
		float content = posW + iconGap + iconSlot + iconGap + sizeW;
		content += splitGap + 1.f * win->dpi + splitGap;
		content += iconSlot;
		if (showRadiusSlider) content += iconGap + sliderW + valueGap + valueW;
		content += iconGap + iconSlot;
		if (showSwatch) content += swatchGap + swatch;
		const float barW = padX + content + padX;
		layoutRect = D2D1::RectF(maskRect.left, maskRect.top - gap - barH, maskRect.left + barW, maskRect.top - gap);
		if (layoutRect.top < 0.f) {
			layoutRect = D2D1::RectF(maskRect.left + paddingMargin, maskRect.top + paddingMargin,
				maskRect.left + paddingMargin + barW, maskRect.top + paddingMargin + barH);
		}
		float x = layoutRect.left + padX + posW + iconGap;
		const float y = layoutRect.top + (barH - iconSlot) * 0.5f;
		lockBtnRect = D2D1::RectF(x, y, x + iconSlot, y + iconSlot);
		x = lockBtnRect.right + iconGap;
		sizeTextX_ = x;
		x += sizeW + splitGap;
		const float splitH = 16.f * win->dpi;
		splitterRect = D2D1::RectF(x, layoutRect.top + (barH - splitH) * 0.5f, x + 1.f * win->dpi, layoutRect.top + (barH + splitH) * 0.5f);
		x = splitterRect.right + splitGap;
		radiusBtnRect = D2D1::RectF(x, y, x + iconSlot, y + iconSlot);
		x = radiusBtnRect.right + iconGap;
		if (showRadiusSlider) {
			radiusSliderRect = D2D1::RectF(x, layoutRect.top, x + sliderW, layoutRect.bottom);
			x = radiusSliderRect.right + valueGap;
			radiusValueRect = D2D1::RectF(x, layoutRect.top, x + valueW, layoutRect.bottom);
			x = radiusValueRect.right + iconGap;
		}
		else {
			radiusSliderRect = {};
			radiusValueRect = {};
		}
		shadowBtnRect = D2D1::RectF(x, y, x + iconSlot, y + iconSlot);
		x = shadowBtnRect.right;
		if (showSwatch) {
			x += swatchGap;
			const float sy = layoutRect.top + (barH - swatch) * 0.5f;
			shadowSwatchRect = D2D1::RectF(x, sy, x + swatch, sy + swatch);
		}
		else {
			shadowSwatchRect = {};
		}
	};

	if (layoutPos && layoutSize && ix == labelIx_ && iy == labelIy_ && iw == labelIw_ && ih == labelIh_) {
		DWRITE_TEXT_METRICS pm{}, sm{};
		layoutPos->GetMetrics(&pm);
		layoutSize->GetMetrics(&sm);
		placeBar(pm.width, sm.width);
		rebuildRadiusValueLayout();
		return;
	}
	labelIx_ = ix; labelIy_ = iy; labelIw_ = iw; labelIh_ = ih;
	layoutPos.Reset();
	layoutSize.Reset();
	auto d2d = Ling::D2D::get();
	layoutPos = d2d->makeTextLayout(std::format(L"X:{}  Y:{}", ix, iy), ToolbarTheme::infoFontSize * win->dpi);
	layoutSize = d2d->makeTextLayout(std::format(L"{}×{} px", iw, ih), ToolbarTheme::infoFontSize * win->dpi);
	if (!layoutPos || !layoutSize) return;
	DWRITE_TEXT_METRICS pm{}, sm{};
	layoutPos->GetMetrics(&pm);
	layoutSize->GetMetrics(&sm);
	placeBar(pm.width, sm.width);
	layoutPos->SetMaxWidth(pm.width + 2.f);
	layoutPos->SetMaxHeight(barH);
	layoutSize->SetMaxWidth(sm.width + 2.f);
	layoutSize->SetMaxHeight(barH);
	rebuildRadiusValueLayout();

	if (!lockLayout) lockLayout = makeIconLayout(Icon::UnlockAspect, iconSlot);
	if (!unlockLayout) unlockLayout = makeIconLayout(Icon::UnlockAspect, iconSlot);
	if (!radiusLayout) radiusLayout = makeIconLayout(Icon::RoundCorner, iconSlot);
	if (!shadowLayout) shadowLayout = makeIconLayout(Icon::Shadow, iconSlot);
}

void CutMask::applyAspectLock(D2D1_RECT_F& r) const
{
	if (lockRatio_ <= 0.f) return;
	const bool moveLeft = adjustHit == MaskHit::Left || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::BottomLeft;
	const bool moveTop = adjustHit == MaskHit::Top || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::TopRight;
	const bool moveRight = adjustHit == MaskHit::Right || adjustHit == MaskHit::TopRight || adjustHit == MaskHit::BottomRight;
	const bool moveBottom = adjustHit == MaskHit::Bottom || adjustHit == MaskHit::BottomLeft || adjustHit == MaskHit::BottomRight;
	float w = std::max(minSize, r.right - r.left);
	float h = std::max(minSize, r.bottom - r.top);
	const bool verticalOnly = (moveTop || moveBottom) && !moveLeft && !moveRight;
	if (verticalOnly) {
		h = std::max(minSize, h);
		w = std::max(minSize, h / lockRatio_);
	}
	else {
		w = std::max(minSize, w);
		h = std::max(minSize, w * lockRatio_);
	}
	if (moveLeft && !moveRight) r.left = r.right - w;
	else r.right = r.left + w;
	if (moveTop && !moveBottom) r.top = r.bottom - h;
	else r.bottom = r.top + h;
	if (r.left < 0.f) { r.right -= r.left; r.left = 0.f; }
	if (r.top < 0.f) { r.bottom -= r.top; r.top = 0.f; }
	if (r.right > win->w) { const float d = r.right - win->w; r.right -= d; r.left -= d; }
	if (r.bottom > win->h) { const float d = r.bottom - win->h; r.bottom -= d; r.top -= d; }
	if (r.left < 0.f) r.left = 0.f;
	if (r.top < 0.f) r.top = 0.f;
}

void CutMask::toggleAspectLock()
{
	if (lockRatio_ > 0.f) lockRatio_ = 0.f;
	else {
		const float w = std::max(1.f, maskRect.right - maskRect.left);
		const float h = std::max(1.f, maskRect.bottom - maskRect.top);
		lockRatio_ = h / w;
	}
	win->refresh();
}

void CutMask::toggleSelRadius()
{
	if (selRadius_ > 0.f) {
		lastSelRadius_ = selRadius_;
		selRadius_ = 0.f;
	}
	else {
		selRadius_ = lastSelRadius_ > 0.f ? lastSelRadius_ : 20.f;
	}
	makeLayout();
	win->refresh();
}

void CutMask::toggleSelShadow()
{
	if (selShadowW_ > 0) {
		lastSelShadowW_ = selShadowW_;
		selShadowW_ = 0;
	}
	else {
		selShadowW_ = lastSelShadowW_ > 0 ? lastSelShadowW_ : 10;
	}
	makeLayout();
	win->refresh();
}

void CutMask::rebuildRadiusValueLayout()
{
	radiusValueLayout.Reset();
	if (selRadius_ <= 0.f) return;
	auto d2d = Ling::D2D::get();
	radiusValueLayout = d2d->makeTextLayout(
		std::format(L"{}", static_cast<int>(std::round(selRadius_))),
		ToolbarTheme::infoFontSize * win->dpi);
	if (!radiusValueLayout) return;
	DWRITE_TEXT_METRICS tm{};
	radiusValueLayout->GetMetrics(&tm);
	radiusValueLayout->SetMaxWidth(ToolbarTheme::sliderValueWidth * win->dpi);
	radiusValueLayout->SetMaxHeight(ToolbarTheme::infoHeight * win->dpi);
	radiusValueLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	radiusValueLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void CutMask::setRadiusFromSliderX(float x)
{
	if (radiusSliderRect.right <= radiusSliderRect.left) return;
	// 与 Ling::Slider::posToValue 同一套：两端各留 thumbR
	const float r = sliderThumbR * win->dpi;
	const float len = (radiusSliderRect.right - radiusSliderRect.left) - 2.f * r;
	if (len <= 0.f) return;
	const float t = std::clamp((x - (radiusSliderRect.left + r)) / len, 0.f, 1.f);
	selRadius_ = std::round(1.f + t * 255.f); // 1..256
	lastSelRadius_ = selRadius_;
	rebuildRadiusValueLayout();
	win->refresh();
}

void CutMask::pickShadowColor()
{
	Ling::Color cur(selShadowColor_);
	static COLORREF custom[16]{};
	COLORREF cr = RGB(cur.r, cur.g, cur.b);
	CHOOSECOLORW cc{};
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = win ? win->hwnd : nullptr;
	cc.lpCustColors = custom;
	cc.rgbResult = cr;
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;
	if (!ChooseColorW(&cc)) return;
	const uint8_t a = cur.a ? cur.a : 64;
	selShadowColor_ = (uint32_t(GetRValue(cc.rgbResult)) << 24)
		| (uint32_t(GetGValue(cc.rgbResult)) << 16)
		| (uint32_t(GetBValue(cc.rgbResult)) << 8)
		| uint32_t(a);
	win->refresh();
}

InfoHit CutMask::hitInfoControl(POINT pos) const
{
	if (hideLabel || layoutRect.right <= layoutRect.left) return InfoHit::None;
	const float x = (float)pos.x, y = (float)pos.y;
	if (!ptIn(layoutRect, x, y)) return InfoHit::None;
	if (ptIn(lockBtnRect, x, y)) return InfoHit::Lock;
	if (ptIn(radiusBtnRect, x, y)) return InfoHit::Radius;
	if (selRadius_ > 0.f && ptIn(radiusSliderRect, x, y)) return InfoHit::RadiusSlider;
	if (selShadowW_ > 0 && ptIn(shadowSwatchRect, x, y)) return InfoHit::ShadowSwatch;
	if (ptIn(shadowBtnRect, x, y)) return InfoHit::Shadow;
	return InfoHit::None;
}

bool CutMask::infoTipAnchor(InfoHit hit, float& screenX, float& screenY) const
{
	const D2D1_RECT_F* r = nullptr;
	switch (hit) {
	case InfoHit::Lock: r = &lockBtnRect; break;
	case InfoHit::Radius: r = &radiusBtnRect; break;
	case InfoHit::Shadow: r = &shadowBtnRect; break;
	default: return false;
	}
	if (!r || r->right <= r->left) return false;
	screenX = (float)win->x + (r->left + r->right) * 0.5f;
	screenY = (float)win->y + r->top + 4.f * win->dpi;
	return true;
}

bool CutMask::onInfoDown(POINT pos)
{
	const auto hit = hitInfoControl(pos);
	if (hit == InfoHit::None) return false;
	infoDrag_ = (hit == InfoHit::RadiusSlider) ? hit : InfoHit::None;
	switch (hit) {
	case InfoHit::Lock: toggleAspectLock(); break;
	case InfoHit::Radius: toggleSelRadius(); break;
	case InfoHit::Shadow: toggleSelShadow(); break;
	case InfoHit::ShadowSwatch: pickShadowColor(); break;
	case InfoHit::RadiusSlider: setRadiusFromSliderX((float)pos.x); break;
	default: break;
	}
	return true;
}

bool CutMask::onInfoMove(POINT pos)
{
	if (infoDrag_ != InfoHit::RadiusSlider) return false;
	setRadiusFromSliderX((float)pos.x);
	return true;
}

void CutMask::onInfoUp()
{
	infoDrag_ = InfoHit::None;
}

void CutMask::startMakeRect(POINT pos)
{
	pressPos = pos;
	// 按下定格到逻辑目标，避免补间还在跑
	snapHoverTarget();
	makeLayout();
}

void CutMask::makeRect(POINT pos)
{
	clearHoverAnim();
	hoverTarget_ = {};
	auto [left, right] = std::minmax(pressPos.x, pos.x);
	auto [top, bottom] = std::minmax(pressPos.y, pos.y);
	maskRect.left = (float)left;
	maskRect.right = (float)right;
	maskRect.top = (float)top;
	maskRect.bottom = (float)bottom;
	makeLayout();
	win->refresh();
}

bool CutMask::hasRect() const
{
	return maskRect.right > maskRect.left && maskRect.bottom > maskRect.top;
}

void CutMask::clearRect()
{
	clearHoverAnim();
	hoverTarget_ = {};
	lastHoverQueryMs_ = 0;
	lastHoverQueryPos_ = {};
	maskRect = {};
	layoutPos = nullptr;
	layoutSize = nullptr;
	radiusValueLayout = nullptr;
	hideLabel = false;
	lockRatio_ = 0.f;
	selRadius_ = 0.f;
	selShadowW_ = 0;
	lockBtnRect = radiusBtnRect = radiusSliderRect = radiusValueRect = {};
	shadowBtnRect = shadowSwatchRect = splitterRect = {};
	sizeTextX_ = 0.f;
	infoDrag_ = InfoHit::None;
	labelIx_ = labelIy_ = -1;
	labelIw_ = labelIh_ = -1;
}

void CutMask::cancelAdjust()
{
	if (adjustHit == MaskHit::None) return;
	maskRect = adjustStartRect;
	adjustHit = MaskHit::None;
	makeLayout();
	win->refresh();
}

void CutMask::endAdjust()
{
	if (adjustHit == MaskHit::None) return;
	adjustHit = MaskHit::None;
	makeLayout();
}

bool CutMask::translateBy(float dx, float dy)
{
	if (!hasRect()) return false;
	const float bw = maskRect.right - maskRect.left;
	const float bh = maskRect.bottom - maskRect.top;
	float nl = maskRect.left + dx;
	float nt = maskRect.top + dy;
	nl = std::clamp(nl, 0.f, std::max(0.f, (float)win->w - bw));
	nt = std::clamp(nt, 0.f, std::max(0.f, (float)win->h - bh));
	if (nl == maskRect.left && nt == maskRect.top) return false;
	maskRect.left = nl;
	maskRect.top = nt;
	maskRect.right = nl + bw;
	maskRect.bottom = nt + bh;
	makeLayout();
	return true;
}

MaskHit CutMask::hitTest(POINT pos) const
{
	if (!hasRect()) return MaskHit::None;
	const float band = hitBandLogical * win->dpi;
	const auto& r = maskRect;
	const float x = (float)pos.x, y = (float)pos.y;
	const bool nearL = x >= r.left - band && x <= r.left + band;
	const bool nearR = x >= r.right - band && x <= r.right + band;
	const bool nearT = y >= r.top - band && y <= r.top + band;
	const bool nearB = y >= r.bottom - band && y <= r.bottom + band;
	const bool inX = x >= r.left - band && x <= r.right + band;
	const bool inY = y >= r.top - band && y <= r.bottom + band;
	if (nearL && nearT) return MaskHit::TopLeft;
	if (nearR && nearT) return MaskHit::TopRight;
	if (nearR && nearB) return MaskHit::BottomRight;
	if (nearL && nearB) return MaskHit::BottomLeft;
	if (nearL && inY) return MaskHit::Left;
	if (nearR && inY) return MaskHit::Right;
	if (nearT && inX) return MaskHit::Top;
	if (nearB && inX) return MaskHit::Bottom;
	if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return MaskHit::Inside;
	return MaskHit::None;
}

void CutMask::startAdjust(POINT pos)
{
	adjustHit = hitTest(pos);
	adjustPressPos = pos;
	adjustStartRect = maskRect;
	if (adjustHit != MaskHit::None && adjustHit != MaskHit::Inside) {
		adjust(pos);
	}
}

void CutMask::adjust(POINT pos)
{
	if (adjustHit == MaskHit::None) return;
	D2D1_RECT_F r = adjustStartRect;
	const float px = (float)pos.x, py = (float)pos.y;
	if (adjustHit == MaskHit::Inside) {
		const float rw = r.right - r.left;
		const float rh = r.bottom - r.top;
		const float left = std::clamp(r.left + px - (float)adjustPressPos.x, 0.f, win->w - rw);
		const float top = std::clamp(r.top + py - (float)adjustPressPos.y, 0.f, win->h - rh);
		r = D2D1::RectF(left, top, left + rw, top + rh);
	}
	else {
		const float cx = std::clamp(px, 0.f, win->w);
		const float cy = std::clamp(py, 0.f, win->h);
		switch (adjustHit) {
		case MaskHit::Left: r.left = cx; break;
		case MaskHit::Right: r.right = cx; break;
		case MaskHit::Top: r.top = cy; break;
		case MaskHit::Bottom: r.bottom = cy; break;
		case MaskHit::TopLeft: r.left = cx; r.top = cy; break;
		case MaskHit::TopRight: r.right = cx; r.top = cy; break;
		case MaskHit::BottomRight: r.right = cx; r.bottom = cy; break;
		case MaskHit::BottomLeft: r.left = cx; r.bottom = cy; break;
		default: break;
		}
		const float l = std::min(r.left, r.right), rr = std::max(r.left, r.right);
		const float t = std::min(r.top, r.bottom), b = std::max(r.top, r.bottom);
		r = D2D1::RectF(l, t, rr, b);
		const bool moveLeft = adjustHit == MaskHit::Left || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::BottomLeft;
		const bool moveTop = adjustHit == MaskHit::Top || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::TopRight;
		if (r.right - r.left < minSize) {
			if (moveLeft) r.left = r.right - minSize;
			else r.right = r.left + minSize;
		}
		if (r.bottom - r.top < minSize) {
			if (moveTop) r.top = r.bottom - minSize;
			else r.bottom = r.top + minSize;
		}
		applyAspectLock(r);
	}
	if (r.left == maskRect.left && r.top == maskRect.top &&
		r.right == maskRect.right && r.bottom == maskRect.bottom) return;
	maskRect = r;
	makeLayout();
}

void CutMask::refreshLabel()
{
	if (!hasRect()) return;
	makeLayout();
}

void CutMask::paintDim(ID2D1DeviceContext* ctx) const
{
	// 全屏路径挖洞，避免四块矩形交界露缝
	const float ww = (float)win->w, hh = (float)win->h;
	const float holeW = maskRect.right - maskRect.left;
	const float holeH = maskRect.bottom - maskRect.top;
	if (holeW <= 1.f || holeH <= 1.f) {
		ctx->FillRectangle(D2D1::RectF(0.f, 0.f, ww, hh), brushBg.Get());
		return;
	}

	auto factory = Ling::D2D::get()->d2dFactory;
	ComPtr<ID2D1RectangleGeometry> fullGeo;
	ComPtr<ID2D1Geometry> holeGeo;
	ComPtr<ID2D1PathGeometry> dimGeo;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(factory->CreateRectangleGeometry(D2D1::RectF(0.f, 0.f, ww, hh), fullGeo.GetAddressOf())))
		return;

	const float rad = clampRadiusToRect(selRadius_ * win->dpi, holeW, holeH);
	if (rad > 0.5f) {
		ComPtr<ID2D1RoundedRectangleGeometry> rr;
		if (FAILED(factory->CreateRoundedRectangleGeometry(
				D2D1::RoundedRect(maskRect, rad, rad), rr.GetAddressOf())))
			return;
		holeGeo = rr;
	}
	else {
		ComPtr<ID2D1RectangleGeometry> rect;
		if (FAILED(factory->CreateRectangleGeometry(maskRect, rect.GetAddressOf())))
			return;
		holeGeo = rect;
	}

	if (FAILED(factory->CreatePathGeometry(dimGeo.GetAddressOf()))) return;
	if (FAILED(dimGeo->Open(sink.GetAddressOf()))) return;
	if (FAILED(fullGeo->CombineWithGeometry(holeGeo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get())))
		return;
	sink->Close();
	ctx->FillGeometry(dimGeo.Get(), brushBg.Get());
}

void CutMask::paintSelShadow(ID2D1DeviceContext* ctx) const
{
	// 拖边/平移中不画；抬手后 endAdjust 清掉 adjustHit
	if (selShadowW_ <= 0 || adjustHit != MaskHit::None) return;
	// maskRect 已是物理像素；用描边环 + 二次衰减，避免实心叠层发脏、发硬
	const float sw = (float)selShadowW_;
	const float w = maskRect.right - maskRect.left;
	const float h = maskRect.bottom - maskRect.top;
	const float rad = clampRadiusToRect(selRadius_ * win->dpi, w, h);
	const float dy = 1.5f; // 轻微下移，更像投影

	auto factory = Ling::D2D::get()->d2dFactory;
	ComPtr<ID2D1RectangleGeometry> outerGeo;
	ComPtr<ID2D1Geometry> holeGeo;
	ComPtr<ID2D1PathGeometry> clipGeo;
	ComPtr<ID2D1GeometrySink> sink;
	const D2D1_RECT_F extent{
		maskRect.left - sw * 2.5f, maskRect.top - sw * 2.5f + dy,
		maskRect.right + sw * 2.5f, maskRect.bottom + sw * 2.5f + dy
	};
	if (FAILED(factory->CreateRectangleGeometry(extent, outerGeo.GetAddressOf()))) return;
	if (rad > 0.5f) {
		ComPtr<ID2D1RoundedRectangleGeometry> rr;
		if (FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(maskRect, rad, rad), rr.GetAddressOf()))) return;
		holeGeo = rr;
	}
	else {
		ComPtr<ID2D1RectangleGeometry> rect;
		if (FAILED(factory->CreateRectangleGeometry(maskRect, rect.GetAddressOf()))) return;
		holeGeo = rect;
	}
	if (FAILED(factory->CreatePathGeometry(clipGeo.GetAddressOf()))) return;
	if (FAILED(clipGeo->Open(sink.GetAddressOf()))) return;
	if (FAILED(outerGeo->CombineWithGeometry(holeGeo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get()))) return;
	sink->Close();

	const auto oldAA = ctx->GetAntialiasMode();
	ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	Ling::Color base(selShadowColor_);
	const float baseA = std::max(0.08f, base.a / 255.f);
	const float stroke = 1.25f;
	for (int i = 1; i <= selShadowW_; ++i) {
		const float t = (float)i / (float)std::max(1, selShadowW_);
		// 越靠外越淡；二次衰减比线性叠实心更柔
		const float a = baseA * (1.f - t) * (1.f - t) * 0.9f;
		if (a < 0.012f) continue;
		brushSelShadow->SetColor(D2D1::ColorF(base.r / 255.f, base.g / 255.f, base.b / 255.f, a));
		const float d = (float)i;
		const D2D1_RECT_F r{
			maskRect.left - d, maskRect.top - d + dy,
			maskRect.right + d, maskRect.bottom + d + dy
		};
		const float rr = rad > 0.5f ? rad + d * 0.25f : 0.f;
		if (rr > 0.5f)
			ctx->DrawRoundedRectangle(D2D1::RoundedRect(r, rr, rr), brushSelShadow.Get(), stroke);
		else
			ctx->DrawRectangle(r, brushSelShadow.Get(), stroke);
	}
	ctx->PopLayer();
	ctx->SetAntialiasMode(oldAA);
}

void CutMask::paintInfoIcons(ID2D1DeviceContext* ctx)
{
	auto drawIcon = [&](IDWriteTextLayout* lay, const D2D1_RECT_F& r, bool active) {
		if (!lay || r.right <= r.left) return;
		brushIcon->SetColor(Ling::Color(active ? Icon::ColorActive : ToolbarTheme::infoIconMuted).getD2DColor());
		ctx->DrawTextLayout({ r.left, r.top }, lay, brushIcon.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	};
	drawIcon(lockRatio_ > 0.f ? lockLayout.Get() : unlockLayout.Get(), lockBtnRect, lockRatio_ > 0.f);
	drawIcon(radiusLayout.Get(), radiusBtnRect, selRadius_ > 0.f);
	drawIcon(shadowLayout.Get(), shadowBtnRect, selShadowW_ > 0);

	// 与 Ling::Slider 同一套：圆角滑条（轨 4 / 钮 14 / 深轨·绿进度·浅钮）
	if (selRadius_ > 0.f && radiusSliderRect.right > radiusSliderRect.left) {
		const float w = radiusSliderRect.right - radiusSliderRect.left;
		const float h = radiusSliderRect.bottom - radiusSliderRect.top;
		const float r = sliderThumbR * win->dpi;
		const float th = sliderTrackH * win->dpi;
		const float len = w - 2.f * r;
		if (len > 0.f) {
			const float cy = radiusSliderRect.top + h * 0.5f;
			const float trackTop = cy - th * 0.5f;
			const float ratio = std::clamp((selRadius_ - 1.f) / 255.f, 0.f, 1.f);
			const float cx = radiusSliderRect.left + r + ratio * len;
			const float trackL = radiusSliderRect.left + r;
			ctx->FillRoundedRectangle(
				D2D1::RoundedRect(D2D1::RectF(trackL, trackTop, trackL + len, trackTop + th), th * 0.5f, th * 0.5f),
				brushSliderTrack.Get());
			if (ratio > 0.f) {
				ctx->FillRoundedRectangle(
					D2D1::RoundedRect(D2D1::RectF(trackL, trackTop, trackL + ratio * len, trackTop + th), th * 0.5f, th * 0.5f),
					brushSliderFill.Get());
			}
			ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brushSliderThumb.Get());
		}
		if (radiusValueLayout && radiusValueRect.right > radiusValueRect.left) {
			ctx->DrawTextLayout({ radiusValueRect.left, radiusValueRect.top }, radiusValueLayout.Get(),
				brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
		}
	}

	if (selShadowW_ > 0 && shadowSwatchRect.right > shadowSwatchRect.left) {
		// 色块用实色展示（alpha 另用于阴影羽化，色块上看不清）
		Ling::Color c(selShadowColor_);
		brushSelShadow->SetColor(D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f));
		const float rr = 2.f * win->dpi;
		ctx->FillRoundedRectangle(D2D1::RoundedRect(shadowSwatchRect, rr, rr), brushSelShadow.Get());
		brushIcon->SetColor(D2D1::ColorF(1.f, 1.f, 1.f, 0.35f));
		ctx->DrawRoundedRectangle(D2D1::RoundedRect(shadowSwatchRect, rr, rr), brushIcon.Get(), 1.f * win->dpi);
	}
}

void CutMask::paint(ID2D1DeviceContext* ctx)
{
	if (!hasRect()) return;
	if (showDim) paintDim(ctx);
	if (showBorder) paintSelShadow(ctx);
	if (drawQrcodeFill) {
		ComPtr<ID2D1SolidColorBrush> fill;
		if (SUCCEEDED(ctx->CreateSolidColorBrush(
			Ling::Color(ToolbarTheme::background).getD2DColor(), fill.GetAddressOf())) && fill) {
			const float ww = maskRect.right - maskRect.left;
			const float hh = maskRect.bottom - maskRect.top;
			const float rad = clampRadiusToRect(selRadius_ * win->dpi, ww, hh);
			if (rad > 0.5f)
				ctx->FillRoundedRectangle(D2D1::RoundedRect(maskRect, rad, rad), fill.Get());
			else
				ctx->FillRectangle(maskRect, fill.Get());
		}
	}

	if (showBorder) {
		const float half = strokeWidth * 0.5f;
		const float edgePad = strokeWidth + win->dpi;
		float l = maskRect.left, t = maskRect.top, r = maskRect.right, b = maskRect.bottom;
		if (l <= 0.5f) l = edgePad;
		else l -= half;
		if (t <= 0.5f) t = edgePad;
		else t -= half;
		if (r >= (float)win->w - 0.5f) r = (float)win->w - edgePad;
		else r += half;
		if (b >= (float)win->h - 0.5f) b = (float)win->h - edgePad;
		else b += half;
		if (r > l + 1.f && b > t + 1.f) {
			const float ww = r - l, hh = b - t;
			const float rad = clampRadiusToRect(selRadius_ * win->dpi, ww, hh);
			if (rad > 0.5f)
				ctx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, b), rad, rad), brushBorder.Get(), strokeWidth);
			else
				ctx->DrawRectangle(D2D1::RectF(l, t, r, b), brushBorder.Get(), strokeWidth);
		}
	}

	if (showHandles) {
		const float hs = handleLogical * win->dpi * 0.5f;
		const float cx = (maskRect.left + maskRect.right) * 0.5f;
		const float cy = (maskRect.top + maskRect.bottom) * 0.5f;
		const D2D1_POINT_2F pts[8] = {
			{ maskRect.left, maskRect.top }, { cx, maskRect.top }, { maskRect.right, maskRect.top },
			{ maskRect.right, cy }, { maskRect.right, maskRect.bottom }, { cx, maskRect.bottom },
			{ maskRect.left, maskRect.bottom }, { maskRect.left, cy },
		};
		for (auto& p : pts) {
			auto hr = D2D1::RectF(p.x - hs, p.y - hs, p.x + hs, p.y + hs);
			ctx->FillRectangle(hr, brushHandleFill.Get());
			ctx->DrawRectangle(hr, brushBorder.Get(), win->dpi);
		}
	}
	if (hideLabel || !layoutPos || !layoutSize) return;
	const float padX = ToolbarTheme::infoPadX * win->dpi;
	const float radius = ToolbarTheme::infoRadius * win->dpi;
	ToolbarChrome::paintInfoBar(ctx, layoutRect, radius, brushInfoBg.Get());
	DWRITE_TEXT_METRICS pm{}, sm{};
	layoutPos->GetMetrics(&pm);
	layoutSize->GetMetrics(&sm);
	const float barH = layoutRect.bottom - layoutRect.top;
	const float posY = layoutRect.top + (barH - pm.height) * 0.5f;
	const float sizeY = layoutRect.top + (barH - sm.height) * 0.5f;
	ctx->DrawTextLayout({ layoutRect.left + padX, posY }, layoutPos.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	ctx->DrawTextLayout({ sizeTextX_, sizeY }, layoutSize.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	if (splitterRect.right > splitterRect.left) {
		brushIcon->SetColor(D2D1::ColorF(1.f, 1.f, 1.f, 0.28f));
		ctx->FillRectangle(splitterRect, brushIcon.Get());
	}
	paintInfoIcons(ctx);
}

bool CutMask::applyExportEffects(std::vector<BYTE>& pixels, int& cw, int& ch) const
{
	if (cw <= 0 || ch <= 0 || pixels.empty()) return false;
	if (selRadius_ <= 0.f && selShadowW_ <= 0) return true;

	auto d2d = Ling::D2D::get();
	auto ctx = d2d->deviceContext.Get();
	const int pad = selShadowW_ > 0 ? selShadowW_ * 2 : 0;
	const int outW = cw + pad * 2;
	const int outH = ch + pad * 2;
	const float rad = clampRadiusToRect(selRadius_, (float)cw, (float)ch);

	D2D1_BITMAP_PROPERTIES1 srcProp{
		.pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_NONE }
	};
	ComPtr<ID2D1Bitmap1> srcBmp;
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU((UINT32)cw, (UINT32)ch), pixels.data(), (UINT32)cw * 4, &srcProp, srcBmp.GetAddressOf())))
		return false;

	D2D1_BITMAP_PROPERTIES1 tgtProp{
		.pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	// TARGET | CANNOT_DRAW 不能 Map；用中间 TARGET 再 Copy 到 CPU 位图
	tgtProp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
	ComPtr<ID2D1Bitmap1> tgtBmp;
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU((UINT32)outW, (UINT32)outH), nullptr, 0, &tgtProp, tgtBmp.GetAddressOf())))
		return false;

	ComPtr<ID2D1Image> oldTarget;
	ctx->GetTarget(oldTarget.GetAddressOf());
	ctx->SetTarget(tgtBmp.Get());
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

	const D2D1_RECT_F hole{
		(float)pad, (float)pad, (float)(pad + cw), (float)(pad + ch)
	};

	if (selShadowW_ > 0) {
		ComPtr<ID2D1SolidColorBrush> sh;
		ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.25f), sh.GetAddressOf());
		auto factory = d2d->d2dFactory;
		ComPtr<ID2D1RectangleGeometry> outerGeo;
		ComPtr<ID2D1Geometry> holeGeo;
		ComPtr<ID2D1PathGeometry> clipGeo;
		ComPtr<ID2D1GeometrySink> sink;
		factory->CreateRectangleGeometry(D2D1::RectF(0.f, 0.f, (float)outW, (float)outH), outerGeo.GetAddressOf());
		if (rad > 0.5f) {
			ComPtr<ID2D1RoundedRectangleGeometry> rr;
			factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(hole, rad, rad), rr.GetAddressOf());
			holeGeo = rr;
		}
		else {
			ComPtr<ID2D1RectangleGeometry> rect;
			factory->CreateRectangleGeometry(hole, rect.GetAddressOf());
			holeGeo = rect;
		}
		factory->CreatePathGeometry(clipGeo.GetAddressOf());
		clipGeo->Open(sink.GetAddressOf());
		outerGeo->CombineWithGeometry(holeGeo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
		sink->Close();
		ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
		Ling::Color base(selShadowColor_);
		const float baseA = std::max(0.08f, base.a / 255.f);
		const float dy = 1.5f;
		ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		for (int i = 1; i <= selShadowW_; ++i) {
			const float t = (float)i / (float)std::max(1, selShadowW_);
			const float a = baseA * (1.f - t) * (1.f - t) * 0.9f;
			if (a < 0.012f) continue;
			sh->SetColor(D2D1::ColorF(base.r / 255.f, base.g / 255.f, base.b / 255.f, a));
			const float d = (float)i;
			const D2D1_RECT_F r{ hole.left - d, hole.top - d + dy, hole.right + d, hole.bottom + d + dy };
			const float rr = rad > 0.5f ? rad + d * 0.25f : 0.f;
			if (rr > 0.5f) ctx->DrawRoundedRectangle(D2D1::RoundedRect(r, rr, rr), sh.Get(), 1.25f);
			else ctx->DrawRectangle(r, sh.Get(), 1.25f);
		}
		ctx->PopLayer();
	}

	if (rad > 0.5f) {
		ComPtr<ID2D1RoundedRectangleGeometry> clipR;
		d2d->d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(hole, rad, rad), clipR.GetAddressOf());
		ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipR.Get()), nullptr);
		ctx->DrawBitmap(srcBmp.Get(), hole);
		ctx->PopLayer();
	}
	else {
		ctx->DrawBitmap(srcBmp.Get(), hole);
	}

	ctx->EndDraw();
	ctx->SetTarget(oldTarget.Get());

	D2D1_BITMAP_PROPERTIES1 cpuProp{
		.pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBmp;
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU((UINT32)outW, (UINT32)outH), nullptr, 0, &cpuProp, cpuBmp.GetAddressOf())))
		return false;
	if (FAILED(cpuBmp->CopyFromBitmap(nullptr, tgtBmp.Get(), nullptr))) return false;
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
	pixels.resize((size_t)outW * outH * 4);
	for (int row = 0; row < outH; ++row) {
		CopyMemory(pixels.data() + (size_t)row * outW * 4,
			mapped.bits + (size_t)row * mapped.pitch, (size_t)outW * 4);
	}
	cpuBmp->Unmap();
	cw = outW;
	ch = outH;
	return true;
}
