#include "pch.h"
#include "CommandKeys.h"
#include "Setting.h"
#include <algorithm>

// 「系统按键」命令表与查表。表头说明见 CommandKeys.h。
namespace CommandKeys
{
namespace
{
	// 落盘的命名空间：仍走 Setting 的通用 per-tool 键值存储（与 toolbarLayout 同一套机制）
	constexpr const wchar_t* kStore{ L"commandKeys" };
	// getToolStr 用"带默认值的重载"取不到"键到底存不存在"，这里用一个不可能出现在键位里的哨兵串
	// 来区分「从没设过（用默认）」与「用户清空过（保持不绑）」。
	const std::wstring kAbsent{ L"\x01" };

	// —— 分组语言键（设置页显示时套 Lang；唯一来源是本表）——
	// ⚠ Lang::get 只按 "段1.段2" 两段取值，所以键名必须是两段 —— 别再写成 cmdkey.grp.xxx
	constexpr const wchar_t* kGrpCommon{ L"cmdkey.grpCommon" };
	constexpr const wchar_t* kGrpCapture{ L"cmdkey.grpCapture" };
	constexpr const wchar_t* kGrpRecord{ L"cmdkey.grpRecord" };
	constexpr const wchar_t* kGrpCanvas{ L"cmdkey.grpCanvas" };

	// —— 掩码缩写（语义见 CommandKeys.h）——
	constexpr unsigned kCapture = SurfaceCapture;
	constexpr unsigned kRecord = SurfaceRecord;
	constexpr unsigned kFullscreen = SurfaceFullscreen;
	// 截图会话与录屏会话共用同一套标注工具栏（通用命令 + 绘图工具都在两边）
	constexpr unsigned kSession = SurfaceCapture | SurfaceRecord;
	// 通用绘图工具：**每一个**工具栏上都有同一个按钮
	constexpr unsigned kDraw = SurfaceAll;
	// 撤销：截图 / 录屏 / 全屏画布的工具条上都有
	constexpr unsigned kUndo = SurfaceCapture | SurfaceRecord | SurfaceFullscreen;

	struct Entry {
		const wchar_t* id;
		const wchar_t* group;  // 分组语言键
		const wchar_t* label;  // 行标签语言键
		Scope scope;
		Scene scene;
		unsigned surfaces;
		bool implemented;  // false = 设置行先占位，功能待做
		bool readOnly;     // 只读提示行：只展示、不可改、也不派发
		const wchar_t* dflt; // 默认键位；空串 = 默认不绑
	};

	// ⚠ 顺序 = 设置页的分组块顺序与组内行顺序（唯一来源，别在设置页另抄一份）
	const Entry kEntries[] = {
		// ——— 常规：会话里的通用命令（窗口内）———
		// 整组只读：均为既有行为的固定键，设置页只作提示、不提供改绑；键位按钮置灰，行名称保持正常色。
		// 只读仅限制改绑，不影响派发（见 scan 与头文件说明）。
		//
		// ⚠ 关闭 / 完成 / 复制 / 存为文件 的掩码是 **SurfaceAll**（含全屏画布），不是 kSession：
		//   它们原为 onKey 里不分界面的分支 —— 画布中 Esc 需能退出（穿透中先退穿透）、
		//   Ctrl+C / Ctrl+S 需能复制保存且不关画布。
		{ L"cmd_close",       kGrpCommon, L"cmdkey.close",       Scope::Session, Scene::Capture, SurfaceAll, true, true, L"Esc" },
		{ L"cmd_done",        kGrpCommon, L"cmdkey.done",        Scope::Session, Scene::Capture, SurfaceAll, true, true, L"Enter" },
		{ L"cmd_copy",        kGrpCommon, L"cmdkey.copy",        Scope::Session, Scene::Capture, SurfaceAll, true, true, L"Ctrl+C" },
		// 工具栏上没有"清除全部标注"这颗按钮：纯命令键（只有系统按键能触发）
		{ L"cmd_clear",       kGrpCommon, L"cmdkey.clear",       Scope::Session, Scene::Capture, kSession, true, true,  L"Ctrl+X" },
		{ L"cmd_undo",        kGrpCommon, L"cmdkey.undo",        Scope::Session, Scene::Capture, kUndo,    true, true,  L"Ctrl+Z" },
		{ L"cmd_redo",        kGrpCommon, L"cmdkey.redo",        Scope::Session, Scene::Capture, kUndo,    true, true,  L"Ctrl+Shift+Z" },
		{ L"cmd_save",        kGrpCommon, L"cmdkey.save",        Scope::Session, Scene::Capture, SurfaceAll, true, true, L"Ctrl+S" },
		// 工具循环（Tab）：在工具栏的绘图工具之间循环切换。气泡里的子工具（包浆/水印/高亮）
		// 不单独计数（见 ToolCap::cycleTool）。
		{ L"cmd_quick_switch",kGrpCommon, L"cmdkey.quickSwitch", Scope::Session, Scene::Capture, SurfaceCapture | SurfaceRecord | SurfaceFullscreen, true, true, L"Tab" },
		// 右键逐层退出是既有行为，列出来只是"告诉用户有这么个键"
		{ L"cmd_exit_level",  kGrpCommon, L"cmdkey.exitLevel",   Scope::Session, Scene::Capture, kSession, true, true,  L"MouseRight" },
		// 选中标注后 Backspace 删除，既有行为
		{ L"cmd_delete_sel",  kGrpCommon, L"cmdkey.deleteSel",   Scope::Session, Scene::Capture, kSession, true, true,  L"Backspace" },

		// ——— 截图：工具栏工具与动作（窗口内）———
		// 行序同工具栏从左到右：显示光标 → 绘图工具(1..9、0) → 其余。
		// 只有绘图工具配了默认键位，其余一概留空、由用户自己绑。
		{ L"cmd_show_cursor",     kGrpCapture, L"cmdkey.showCursor",     Scope::Session, Scene::Capture, kCapture, true, false, L"`" },
		// 通用绘图工具：Scene 写 None（哪个场景都能命中），掩码 kDraw（参与所有界面的冲突判定）
		{ L"cmd_tool_rect",       kGrpCapture, L"cmdkey.toolRect",       Scope::Session, Scene::None, kDraw, true, false, L"1" },
		{ L"cmd_tool_arrow",      kGrpCapture, L"cmdkey.toolArrow",      Scope::Session, Scene::None, kDraw, true, false, L"2" },
		{ L"cmd_tool_pen",        kGrpCapture, L"cmdkey.toolPen",        Scope::Session, Scene::None, kDraw, true, false, L"3" },
		{ L"cmd_tool_text",       kGrpCapture, L"cmdkey.toolText",       Scope::Session, Scene::None, kDraw, true, false, L"4" },
		{ L"cmd_tool_serial",     kGrpCapture, L"cmdkey.toolSerial",     Scope::Session, Scene::None, kDraw, true, false, L"5" },
		{ L"cmd_tool_mosaic",     kGrpCapture, L"cmdkey.toolMosaic",     Scope::Session, Scene::None, kDraw, true, false, L"6" },
		{ L"cmd_tool_patina",     kGrpCapture, L"cmdkey.toolPatina",     Scope::Session, Scene::None, kDraw, true, false, L"7" },
		{ L"cmd_tool_watermark",  kGrpCapture, L"cmdkey.toolWatermark",  Scope::Session, Scene::None, kDraw, true, false, L"8" },
		{ L"cmd_tool_highlight",  kGrpCapture, L"cmdkey.toolHighlight",  Scope::Session, Scene::None, kDraw, true, false, L"9" },
		{ L"cmd_tool_eraser",     kGrpCapture, L"cmdkey.toolEraser",     Scope::Session, Scene::None, kDraw, true, false, L"0" },
		{ L"cmd_video_record",    kGrpCapture, L"cmdkey.videoRecord",    Scope::Session, Scene::Capture, kCapture, true, false, L"" },
		{ L"cmd_pin",             kGrpCapture, L"cmdkey.pin",            Scope::Session, Scene::Capture, kCapture, true, false, L"" },
		{ L"cmd_ocr",             kGrpCapture, L"cmdkey.ocr",            Scope::Session, Scene::Capture, kCapture, true, false, L"W" },
		// Native 用 ZXing（真实解码），所以这条是实装的（QT6 那边仍是纯桩）
		{ L"cmd_scan_qrcode",     kGrpCapture, L"cmdkey.scanQrcode",     Scope::Session, Scene::Capture, kCapture, true, false, L"Q" },
		{ L"cmd_translate",       kGrpCapture, L"cmdkey.translate",      Scope::Session, Scene::Capture, kCapture, true, false, L"F" },
		{ L"cmd_scroll_capture",  kGrpCapture, L"cmdkey.scrollCapture",  Scope::Session, Scene::Capture, kCapture, true, false, L"" },
		{ L"cmd_scroll_pause",    kGrpCapture, L"cmdkey.scrollPause",    Scope::Session, Scene::Capture, kCapture, true, false, L"Space" },

		// ——— 录屏：录屏会话里的控制 ———
		// 本组只有「开始/暂停录制」是**临时全局键**：焦点在被录的程序上，窗口内根本收不到，
		// 必须在录屏会话期间由全局热键代注册、会话一结束就撤。
		// GIF / 系统音量 / 麦克风**不是**，默认也不配键位 —— 它们只在工具栏有焦点时用得到，
		// 设成全局键或给默认值，只会白白占掉用户自己的 `G` `]` `\`。
		// 演示画布内也能录屏 → 掩码同时含 Fullscreen，参与和演示组之间的冲突判定。
		{ L"cmd_record_toggle",       kGrpRecord, L"cmdkey.recordToggle",       Scope::Global,  Scene::Record, kRecord | kFullscreen, true, false, L"F2" },
		{ L"cmd_toggle_gif",          kGrpRecord, L"cmdkey.toggleGif",          Scope::Session, Scene::Record, kRecord | kFullscreen, true, false, L"" },
		{ L"cmd_toggle_system_audio", kGrpRecord, L"cmdkey.toggleSystemAudio",  Scope::Session, Scene::Record, kRecord | kFullscreen, true, false, L"" },
		{ L"cmd_toggle_mic",          kGrpRecord, L"cmdkey.toggleMic",          Scope::Session, Scene::Record, kRecord | kFullscreen, true, false, L"" },

		// ——— 全屏画布（窗口内）———
		// 鼠标穿透同样是**临时全局键**：它本身就是把焦点交给别的窗，没有全局键切不回来。
		// 掩码含录屏 → 录屏会话期间也一并镜像。
		{ L"cmd_mouse_through",  kGrpCanvas, L"cmdkey.mouseThrough",  Scope::Global, Scene::Fullscreen, kRecord | kFullscreen, true, false, L"`" },
		// 工具栏上没有"隐藏/显示工具栏"这颗按钮：纯命令键（演示画布时收起整条工具栏）。
		// 它同样是**临时全局键** —— 穿透/收起之后焦点已经不在本软件上，窗口内收不到。
		{ L"cmd_toggle_toolbar", kGrpCanvas, L"cmdkey.toggleToolbar", Scope::Global, Scene::Fullscreen, kFullscreen, true, false, L"F1" },
	};

	const Entry* findEntry(const std::wstring& id)
	{
		for (const auto& e : kEntries) if (id == e.id) return &e;
		return nullptr;
	}

	// —— 命令 id → 工具栏动作（见头文件说明：两者不能混用）——
	struct ActionMap { const wchar_t* id; const wchar_t* action; const wchar_t* subTool; };
	const ActionMap kActionMap[] = {
		{ L"cmd_tool_rect",       L"rect", L"" },
		{ L"cmd_tool_arrow",      L"arrow", L"" },
		{ L"cmd_tool_pen",        L"pen", L"" },
		{ L"cmd_tool_text",       L"text", L"" },
		{ L"cmd_tool_serial",     L"number", L"" },
		{ L"cmd_tool_mosaic",     L"mosaic", L"" },
		{ L"cmd_tool_patina",     L"mosaic", L"patina" },
		{ L"cmd_tool_watermark",  L"mosaic", L"watermark" },
		{ L"cmd_tool_highlight",  L"mosaic", L"highlight" },
		{ L"cmd_tool_eraser",     L"eraser", L"" },
		{ L"cmd_video_record",    L"extra", L"" },
		{ L"cmd_mouse_through",   L"mouse-through", L"" },
		{ L"cmd_pin",             L"pin", L"" },
		{ L"cmd_ocr",             L"ocr", L"" },
		// 动作留空（不参与派发，runCommand 的显式分支负责 startQrcode）；子工具 id 供
		// 「快捷键提示」反查 —— 识别气泡里选「二维码」时该按钮应显示 Q
		{ L"cmd_scan_qrcode",     L"", L"qrcode" },
		{ L"cmd_translate",       L"translate", L"" },
		{ L"cmd_scroll_capture",  L"long", L"" },
	};
	const ActionMap* findAction(const std::wstring& id)
	{
		for (const auto& m : kActionMap) if (id == m.id) return &m;
		return nullptr;
	}

	// 「快捷键提示」的补充槽位映射：这几颗按钮的语义由「常规」组直接承载（派发走 runCommand 的
	// 显式分支），在 kActionMap 里没有动作 id，故单独列出。仅用于取键位，不参与派发。
	struct HintSlot { const wchar_t* slot; const wchar_t* id; };
	const HintSlot kHintSlots[] = {
		{ L"undo",      L"cmd_undo" },
		{ L"save",      L"cmd_save" },
		{ L"close",     L"cmd_close" },
		{ L"cancel",    L"cmd_close" },   // 全屏画布布局里这颗叫 cancel
		{ L"clipboard", L"cmd_done" },
	};

	// —— 键名口径与 Ling::App::regHotKey / 设置页录制完全一致（同 HotkeyHook 的解析）——
	bool parseChord(const std::wstring& s, unsigned& mods, unsigned& key)
	{
		mods = 0; key = 0;
		std::wstring lower = s;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
		for (auto& part : Ling::Util::splitStr(lower, L'+')) {
			if (part == L"ctrl") mods |= MOD_CONTROL;
			else if (part == L"alt") mods |= MOD_ALT;
			else if (part == L"shift") mods |= MOD_SHIFT;
			else if (part == L"win" || part == L"lwin" || part == L"rwin") mods |= MOD_WIN;
			else key = Ling::Util::strToKey(part);
		}
		return key != 0;
	}

	unsigned liveMods()
	{
		unsigned m = 0;
		if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
		if (GetKeyState(VK_MENU) & 0x8000) m |= MOD_ALT;
		if (GetKeyState(VK_SHIFT) & 0x8000) m |= MOD_SHIFT;
		if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN;
		return m;
	}

	// 表内顺序查找：命中一条"掩码与 surface 有交集、已实装"的命令。
	// ⚠ readOnly 的**不跳过**：只读 = 只挡"改绑"，键位仍按默认值**照旧派发**
	//   （「常规」整组、层级退出、删除标注都是这一类）。
	//
	// 匹配优先级：**修饰键完全一致 > 子集命中**。
	//   子集命中（(mods & m) == m）是为了照顾"按住 Shift 打个 1"这类边角（键码仍是 VK_1），
	//   但如果只按子集算，`Ctrl+Shift+Z`（重做）会先撞上排在更前面的 `Ctrl+Z`（撤销）——
	//   所以先把精确匹配的那条挑出来，挑不到才回退到第一个子集命中。
	std::wstring scan(unsigned mods, unsigned key, unsigned surface)
	{
		if (key == 0 || surface == 0) return {};
		std::wstring loose;
		for (const auto& e : kEntries) {
			if (!e.implemented) continue;
			if ((e.surfaces & surface) == 0) continue;
			unsigned m = 0, k = 0;
			const std::wstring chord = binding(e.id);
			if (!parseChord(chord, m, k)) continue;
			if (k != key || (mods & m) != m) continue;
			if (mods == m) return e.id;
			if (loose.empty()) loose = e.id;
		}
		return loose;
	}
}

bool isKnownId(const std::wstring& id) { return findEntry(id) != nullptr; }
std::wstring labelKey(const std::wstring& id) { const auto* e = findEntry(id); return e ? e->label : id; }
Scope scopeOf(const std::wstring& id) { const auto* e = findEntry(id); return e ? e->scope : Scope::Session; }
Scene sceneOf(const std::wstring& id) { const auto* e = findEntry(id); return e ? e->scene : Scene::None; }
bool implemented(const std::wstring& id) { const auto* e = findEntry(id); return e ? e->implemented : false; }
bool readOnly(const std::wstring& id) { const auto* e = findEntry(id); return e ? e->readOnly : false; }
unsigned surfaceMaskForId(const std::wstring& id) { const auto* e = findEntry(id); return e ? e->surfaces : SurfaceAll; }
std::wstring defaultBinding(const std::wstring& id) { const auto* e = findEntry(id); return e ? e->dflt : L""; }

std::wstring binding(const std::wstring& id)
{
	auto* s = Setting::get();
	if (!s) return defaultBinding(id);
	const std::wstring stored = s->getToolStr(kStore, id, kAbsent);
	if (stored == kAbsent) return defaultBinding(id);   // 从没设过 → 用默认
	return stored;                                      // 可能是空串（用户清空过 → 保持不绑）
}

void setBinding(const std::wstring& id, const std::wstring& chord)
{
	if (!isKnownId(id)) return;
	if (!chord.empty() && readOnly(id)) return;   // 只读行不许**改绑**（清空仍放行：「覆盖」要先解绑旧主）
	if (auto* s = Setting::get()) s->setToolStr(kStore, id, chord);
}

void resetBindings()
{
	auto* s = Setting::get();
	if (!s) return;
	// 删掉覆盖项而非写入默认值：删掉后 binding() 走"从没设过"的分支，与出厂状态完全一致
	for (const auto& e : kEntries) s->removeToolKey(kStore, e.id);
}

bool anyCustomBinding()
{
	auto* s = Setting::get();
	if (!s) return false;
	for (const auto& e : kEntries) {
		if (s->getToolStr(kStore, e.id, kAbsent) != kAbsent) return true;
	}
	return false;
}

std::vector<std::wstring> groupOrder()
{
	std::vector<std::wstring> out;
	for (const auto& e : kEntries) {
		const std::wstring g{ e.group };
		if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
	}
	return out;
}

std::vector<std::wstring> idsInGroup(const std::wstring& gkey)
{
	std::vector<std::wstring> out;
	for (const auto& e : kEntries) if (gkey == e.group) out.push_back(e.id);
	return out;
}

bool groupIsGlobal(const std::wstring& gkey)
{
	for (const auto& e : kEntries) if (gkey == e.group) return e.scope == Scope::Global;
	return false;
}

bool boundToOther(const std::wstring& chord, const std::wstring& forId,
	std::wstring* otherLabelKey, std::wstring* otherId)
{
	if (chord.empty()) return false;
	const unsigned mine = surfaceMaskForId(forId);
	if (mine == 0) return false;
	for (const auto& e : kEntries) {
		if (forId == e.id) continue;
		if ((e.surfaces & mine) == 0) continue;                 // 跨界面（无交集）不算冲突
		if (_wcsicmp(binding(e.id).c_str(), chord.c_str()) != 0) continue;
		if (otherLabelKey) *otherLabelKey = e.label;
		if (otherId) *otherId = e.id;
		return true;
	}
	return false;
}

bool validChord(const std::wstring& chord)
{
	if (chord.empty()) return false;
	unsigned m = 0, k = 0;
	return parseChord(chord, m, k);
}

bool reservedChord(const std::wstring& chord, std::wstring* ownerLabelKey)
{
	if (chord.empty()) return false;
	for (const auto& e : kEntries) {
		if (!e.readOnly) continue;
		if (_wcsicmp(binding(e.id).c_str(), chord.c_str()) != 0) continue;
		if (ownerLabelKey) *ownerLabelKey = e.label;
		return true;
	}
	return false;
}

std::wstring actionForKey(unsigned vk, unsigned surface)
{
	return scan(liveMods(), vk, surface);
}

std::wstring actionForChord(const std::wstring& chord, unsigned surface)
{
	unsigned m = 0, k = 0;
	if (!parseChord(chord, m, k)) return {};   // 鼠标键（MouseRight 等）由调用方自己处理
	return scan(m, k, surface);
}

std::wstring toolbarActionForId(const std::wstring& id)
{
	const auto* m = findAction(id);
	return m ? m->action : L"";
}

std::wstring subToolForId(const std::wstring& id)
{
	const auto* m = findAction(id);
	return m ? m->subTool : L"";
}

std::wstring hintChord(const std::wstring& slotId, const std::wstring& effective, unsigned surface)
{
	if (slotId.empty() || surface == 0) return {};
	// 1) 有效工具是气泡里的子工具 → 按子工具 id 命中（包浆/水印/高亮/二维码）
	if (!effective.empty() && effective != slotId) {
		for (const auto& e : kEntries) {
			const auto* m = findAction(e.id);
			if (!m || !m->subTool || !*m->subTool) continue;
			if (effective != m->subTool) continue;
			if ((e.surfaces & surface) == 0) continue;
			return binding(e.id);
		}
	}
	// 2) 否则按槽位自身的动作命中（表内顺序，mosaic 槽取到的是 cmd_tool_mosaic）
	for (const auto& e : kEntries) {
		if ((e.surfaces & surface) == 0) continue;
		if (slotId != toolbarActionForId(e.id)) continue;
		return binding(e.id);
	}
	// 3) kActionMap 未覆盖的槽位（撤销 / 保存 / 关闭 / 完成）→ 查补充表
	for (const auto& m : kHintSlots) {
		if (slotId != m.slot) continue;
		if ((surfaceMaskForId(m.id) & surface) == 0) continue;
		return binding(m.id);
	}
	return {};
}

} // namespace CommandKeys
