#include "pch.h"
#include <algorithm>
#include "ToolbarStore.h"
#include "IconCodes.h"
#include "../Setting.h"

namespace ToolbarStore {

	// ==================== 布局 ====================

	namespace {
		std::vector<std::wstring> splitIds(const std::wstring& s)
		{
			std::vector<std::wstring> out;
			std::wstring cur;
			for (wchar_t c : s) {
				if (c == L',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
				else cur += c;
			}
			if (!cur.empty()) out.push_back(cur);
			return out;
		}
		std::wstring joinIds(const std::vector<std::wstring>& v)
		{
			std::wstring s;
			for (size_t i = 0; i < v.size(); i++) {
				if (i) s += L",";
				s += v[i];
			}
			return s;
		}
	}

	LayoutState defaultLayout(int kind)
	{
		LayoutState st;
		st.inactive = { L"split-extra" };
		if (kind == 0) {
			// 截图栏（kind=0）：
			//   椭圆已并入「矩形」的属性栏变体，**不再是独立槽位**；橡皮擦是独立槽位。
			st.active = { L"show-cursor", L"rect", L"arrow", L"pen",
				L"text", L"serial-number", L"mosaic", L"eraser", L"split-0", L"undo", L"split-1",
				L"extra", L"fixed", L"ocr", L"translate", L"scroll-screenshot",
				L"save", L"cancel", L"confirm" };
		}
		else if (kind == 1) {
			// 录屏栏（kind=1）：鼠标穿透 ┊ 录制/暂停（**同一个按钮**，
			// 不再是播放/暂停两个）・时长・系统声・麦克风 ┊ 标注… ┊ 动图/保存/取消/确认
			st.active = { L"mouse-through", L"split-3", L"video-play", L"record-time",
				L"video-audio", L"video-mic",
				L"split-0", L"rect", L"arrow", L"pen", L"text",
				L"serial-number", L"mosaic", L"eraser", L"split-1", L"undo", L"split-2",
				L"video-gif", L"save", L"cancel", L"confirm" };
		}
		else if (kind == 2) {
			// 演示画布（kind=2）：椭圆同样并入矩形，不单列
			st.active = { L"rect", L"arrow", L"pen", L"text",
				L"serial-number", L"mosaic", L"eraser", L"laser-pointer", L"split-0",
				L"undo", L"split-1", L"reset-canvas", L"mouse-through", L"cancel" };
		}
		else {
			// 贴图栏（kind=3）：椭圆不单列
			st.active = { L"show-border", L"split-0", L"rect", L"arrow",
				L"pen", L"text", L"serial-number", L"mosaic", L"eraser", L"split-1",
				L"undo", L"split-2", L"ocr", L"translate", L"cancel", L"confirm" };
		}
		return st;
	}

	LayoutState loadLayout(int kind)
	{
		auto def = defaultLayout(kind);
		LayoutState st;
		std::wstring key = std::to_wstring(kind);
		st.active = splitIds(Setting::get()->getToolStr(L"toolbarLayout", key + L"a", L""));
		if (st.active.empty()) return def;
		st.inactive = splitIds(Setting::get()->getToolStr(L"toolbarLayout", key + L"i", L""));
		if (st.inactive.empty()) st.inactive = { L"split-extra" };
		// 兼容旧配置：ellipse 已并入「矩形」（属性栏里的变体），不再是独立槽位。
		// 旧布局里残留的 ellipse 读进来就清掉，不必让用户点「恢复默认」重置整个布局。
		bool dropped = false;
		auto dropMerged = [&dropped](std::vector<std::wstring>& v) {
			const size_t n = v.size();
			v.erase(std::remove(v.begin(), v.end(), L"ellipse"), v.end());
			if (v.size() != n) dropped = true;
		};
		dropMerged(st.active);
		dropMerged(st.inactive);
		if (st.active.empty()) return def;
		if (dropped) saveLayout(kind, st);
		return st;
	}

	void saveLayout(int kind, const LayoutState& st)
	{
		std::wstring key = std::to_wstring(kind);
		Setting::get()->setToolStr(L"toolbarLayout", key + L"a", joinIds(st.active));
		Setting::get()->setToolStr(L"toolbarLayout", key + L"i", joinIds(st.inactive));
	}

	bool isDefault(int kind)
	{
		auto def = defaultLayout(kind);
		auto cur = loadLayout(kind);
		return cur.active == def.active && cur.inactive == def.inactive;
	}

	void restoreDefaults(int kind)
	{
		saveLayout(kind, defaultLayout(kind));
	}

	// ==================== 主题色板 ====================

	namespace {
		Colors g_custom{};
		int g_mode{ 0 };
		bool g_loaded{ false };
	}

	// 顺序：背景 / 停悬 / 图标 / 选中 / 框选
	Colors darkPreset()
	{
		return { 0x1E1E1FFF, 0x333333FF, 0xE0E0E5FF, 0x34C759FF, 0x34C759FF };
	}

	Colors lightPreset()
	{
		// 背景纯白（原来 0xEAEAEA 偏灰）；停悬 #E5E5E5
		return { 0xFFFFFFFF, 0xE5E5E5FF, 0x515151FF, 0x34C759FF, 0x34C759FF };
	}

	Colors systemPreset()
	{
		// 读系统深浅色：AppsUseLightTheme=0 深色，否则浅色
		DWORD light = 1;
		HKEY hKey = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER,
			L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			0, KEY_READ, &hKey) == ERROR_SUCCESS) {
			DWORD size = sizeof(DWORD);
			RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
				reinterpret_cast<LPBYTE>(&light), &size);
			RegCloseKey(hKey);
		}
		return light ? lightPreset() : darkPreset();
	}

	namespace {
		void loadOnce()
		{
			if (g_loaded) return;
			g_loaded = true;
			auto* s = Setting::get();
			g_mode = std::clamp(s->getToolUInt(L"toolbarPalette", L"mode", 1), 0u, 3u);
			auto& c = g_custom;
			// 键名沿用 c0/c1/c3/c4/c5：旧的 c2＝"停悬图标"已并入图标色，不再读取（老配置不丢色）。
			c.primary = s->getToolUInt(L"toolbarPalette", L"c0", darkPreset().primary);
			c.hover = s->getToolUInt(L"toolbarPalette", L"c1", darkPreset().hover);
			c.icon = s->getToolUInt(L"toolbarPalette", L"c3", darkPreset().icon);
			c.accent = s->getToolUInt(L"toolbarPalette", L"c4", darkPreset().accent);
			c.selection = s->getToolUInt(L"toolbarPalette", L"c5", darkPreset().selection);
		}
	}

	int modeId()
	{
		loadOnce();
		return g_mode;
	}

	Colors custom()
	{
		loadOnce();
		return g_custom;
	}

	Colors current()
	{
		loadOnce();
		switch (Mode(g_mode)) {
		case Mode::System: return systemPreset();
		case Mode::Light:  return lightPreset();
		case Mode::Custom: return g_custom;
		case Mode::Dark:
		default:           return darkPreset();
		}
	}

	void setModeId(int id)
	{
		loadOnce();
		if (id < 0 || id > 3) return;
		g_mode = id;
		Setting::get()->setToolUInt(L"toolbarPalette", L"mode", id);
	}

	void applyCustom(const Colors& c, bool persist)
	{
		loadOnce();
		g_mode = 3;
		g_custom = c;
		if (!persist) return;
		auto* s = Setting::get();
		s->setToolUInt(L"toolbarPalette", L"mode", 3);
		s->setToolUInt(L"toolbarPalette", L"c0", c.primary);
		s->setToolUInt(L"toolbarPalette", L"c1", c.hover);
		s->setToolUInt(L"toolbarPalette", L"c3", c.icon);   // c2 为旧的"停悬图标"，不再写入
		s->setToolUInt(L"toolbarPalette", L"c4", c.accent);
		s->setToolUInt(L"toolbarPalette", L"c5", c.selection);
	}

	const wchar_t* slotName(int slot)
	{
		static const wchar_t* names[] = { L"背景", L"停悬", L"图标", L"选中", L"框选" };
		return (slot >= 0 && slot < 5) ? names[slot] : L"";
	}

	std::wstring iconOf(const std::wstring& id)
	{
		if (id == L"show-cursor") return Icon::Cursor;
		if (id == L"rect") return Icon::Rect;
		if (id == L"ellipse") return Icon::Ellipse;
		if (id == L"arrow") return Icon::Arrow;
		if (id == L"pen") return Icon::Pen;
		if (id == L"text") return Icon::Text;
		if (id == L"serial-number") return Icon::Number;
		if (id == L"mosaic") return Icon::Mosaic;
		if (id == L"eraser") return Icon::Eraser;
		if (id == L"undo") return Icon::Undo;
		if (id == L"extra") return Icon::Video;
		if (id == L"fixed") return Icon::Pin;
		if (id == L"ocr") return Icon::Ocr;
		if (id == L"translate") return Icon::Translate;
		if (id == L"scroll-screenshot") return Icon::LongShot;
		if (id == L"save") return Icon::Save;
		if (id == L"cancel") return Icon::Cancel;
		if (id == L"confirm") return Icon::Done;
		if (id == L"video-play") return Icon::Play;   // 录制/暂停同一按钮
		if (id == L"video-pause") return Icon::Pause;
		if (id == L"video-audio") return Icon::Audio;
		if (id == L"video-mic") return Icon::MicOn;
		if (id == L"video-folder") return Icon::Folder;
		if (id == L"video-gif") return Icon::Gif;
		if (id == L"laser-pointer") return Icon::FadePen;
		if (id == L"reset-canvas") return Icon::Redo;
		if (id == L"mouse-through") return Icon::Pierce;
		if (id == L"show-border") return Icon::DashRect;
		return L"";
	}
}
