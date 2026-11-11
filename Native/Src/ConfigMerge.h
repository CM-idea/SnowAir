#pragma once
// 配置文件三路合并：将本进程相对基线的改动合并到磁盘最新配置，避免用旧内存整体覆盖。
//
// config.json 可能被多台电脑同时修改。若直接整份写入：
//   1. 会覆盖另一台电脑刚写入的设置；
//   2. 会使 OneDrive 判定双方均有修改，从而生成冲突副本。
// 合并后，不同键的改动可以共存；同一键被同时修改时，以最后写入者为准。
#include <winrt/Windows.Data.Json.h>
#include <string>
#include <vector>

namespace ConfigMerge {
using namespace winrt::Windows::Data::Json;

// latest: 磁盘上的最新配置（就地修改）
// base:   本进程最后一次读取或写入的配置（合并基线）
// mine:   本进程当前内存中的配置
// 规则：mine 相对 base 的新增、修改、删除应用到 latest；latest 中其他键保持不变。
// 对象递归合并；数组和标量按整体比较。
inline void MergeObjects(JsonObject& latest, const JsonObject& base, const JsonObject& mine)
{
    if (!latest) latest = JsonObject{};
    if (!base || !mine) return;

    // base 中存在、mine 中不存在的键表示本进程删除了该键
    std::vector<std::wstring> removed;
    for (auto it = base.First(); it.HasCurrent(); it.MoveNext()) {
        const std::wstring key{ it.Current().Key().c_str() };
        if (!mine.HasKey(key.c_str())) removed.push_back(key);
    }
    for (const auto& key : removed) latest.Remove(key.c_str());

    for (auto it = mine.First(); it.HasCurrent(); it.MoveNext()) {
        const auto pair = it.Current();
        const std::wstring key{ pair.Key().c_str() };
        const auto mineVal = pair.Value();

        // 本进程新增的键
        if (!base.HasKey(key.c_str())) {
            latest.SetNamedValue(key.c_str(), mineVal);
            continue;
        }

        const auto baseVal = base.GetNamedValue(key.c_str());
        if (mineVal.Stringify() == baseVal.Stringify()) continue;   // 本进程未修改该键

        if (mineVal.ValueType() == JsonValueType::Object) {
            // 双方均为对象：递归合并，保留对方在同一对象中的其他改动
            JsonObject latestChild{ nullptr };
            if (latest.HasKey(key.c_str())) {
                const auto latestVal = latest.GetNamedValue(key.c_str());
                if (latestVal.ValueType() == JsonValueType::Object) latestChild = latestVal.GetObject();
            }
            if (!latestChild) latestChild = JsonObject{};
            MergeObjects(latestChild, baseVal.GetObject(), mineVal.GetObject());
            latest.SetNamedValue(key.c_str(), latestChild);
            continue;
        }

        // 标量或数组：同一键被双方修改，以最后写入者为准
        latest.SetNamedValue(key.c_str(), mineVal);
    }
}
} // namespace ConfigMerge
