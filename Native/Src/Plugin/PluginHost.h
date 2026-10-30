#pragma once

#include <windows.h>
#include <string>
#include "SnowAirPlugin.h"

/*
 * Local test host for the standalone SnowAir rack plugin.
 *
 * It loads the DLL built from SnowAir-Rack and supplies the small host surface
 * that the plugin needs: theme roles, config, history/clipboard counts and the
 * action that opens history. This is intentionally a thin bridge; the rack UI
 * itself stays inside the plugin process.
 */
class PluginHost {
public:
    static PluginHost& instance();

    void init();
    void shutdown();

    bool loaded() const { return plugin_ != nullptr; }

    void onCapture(const std::wstring& path);
    void hangQuickPin(const std::wstring& path);
    void onSettingChanged(const std::wstring& key);

    int historyCount();
    int clipboardCount();
    void openHistory();
    void quickHang();
    void toggleRack();
    bool rackInstalled() const;
    std::wstring rackVersion() const;   // 插件已加载时返回其自报版本，否则空串
    bool uninstallRack();

    // Theme preview/apply. `preview` keeps the value out of config.json.
    void previewRackTheme(int theme);
    void applyRackTheme(int theme);
    void clearRackThemePreview();
    void setRackPreview(bool on);

    int themeOverrideForHost() const;
    bool rackPreviewPinnedForHost() const { return rackPreview_; }
    int historyCountForHost();
    int clipboardCountForHost();
    void openHistoryForHost();

private:
    PluginHost() = default;
    ~PluginHost() = default;
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

private:
    HMODULE module_ = nullptr;
    const SnowAirPlugin* plugin_ = nullptr;
    SnowAirHost host_{};
    int themeOverride_ = -1;
    bool rackPreview_ = false;
};
