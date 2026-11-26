#pragma once

#include "fangcun/ResolvedSkin.hpp"

namespace fangcun::skin {

// Internal editing metadata. This deliberately does not change .fcskin v1:
// omitted values inherit the original builtin, including its material drawing.
struct BuiltinStyleTone {
    std::array<std::optional<ColorArgb>, colorNames.size()> colors{};
    std::optional<std::array<ColorArgb, 4>> glow;
};

struct BuiltinSkinStyle {
    std::string builtin, hash;
    std::optional<std::string> fontFamily;
    std::optional<float> pressOffsetDp;
    BuiltinStyleTone light, dark;
    const BuiltinStyleTone& tone(bool isDark) const { return isDark ? dark : light; }
};

inline bool validBuiltinStyleId(std::string_view id) {
    return id == "wood_type" || id == "typewriter" || id == "terminal" || id == "brutal";
}

inline std::optional<BuiltinSkinStyle> parseBuiltinSkinStyleIni(
    std::string_view content, std::string_view expectedBuiltin, std::string_view expectedHash) {
    if (content.empty() || content.size() > 16384 ||
        !validBuiltinStyleId(expectedBuiltin) || !validHash(expectedHash)) return {};
    std::map<std::string, std::string> values;
    while (!content.empty()) {
        const auto end = content.find('\n');
        auto line = content.substr(0, end);
        content = end == std::string_view::npos ? std::string_view{} : content.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.size() > 160) return {};
        for (unsigned char c : line) if (c < 32 || c > 126) return {};
        const auto equal = line.find('=');
        if (equal == std::string_view::npos || equal == 0 ||
            !values.emplace(std::string(line.substr(0, equal)), std::string(line.substr(equal + 1))).second) return {};
    }
    auto take = [&](const std::string& key) -> std::optional<std::string> {
        const auto found = values.find(key);
        if (found == values.end()) return {};
        auto value = std::move(found->second);
        values.erase(found);
        return value;
    };
    const auto version = take("schema_version"), builtin = take("builtin"), hash = take("hash");
    if (!version || *version != "1" || !builtin || *builtin != expectedBuiltin ||
        !hash || *hash != expectedHash) return {};
    BuiltinSkinStyle result;
    result.builtin = *builtin;
    result.hash = *hash;
    if (auto font = take("font_family")) {
        if (*font != "sans" && *font != "serif" && *font != "mono") return {};
        result.fontFamily = *font;
    }
    if (const auto press = take("press_offset_dp")) {
        double value = 0;
        const auto parsed = std::from_chars(press->data(), press->data() + press->size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != press->data() + press->size() ||
            !std::isfinite(value) || value < 0 || value > 2) return {};
        result.pressOffsetDp = static_cast<float>(value);
    }
    for (int isDark = 0; isDark < 2; ++isDark) {
        auto& tone = isDark ? result.dark : result.light;
        const std::string prefix = isDark ? "dark_" : "light_";
        for (size_t i = 0; i < colorNames.size(); ++i) {
            if (const auto value = take(prefix + std::string(colorNames[i]))) {
                auto color = parseColor(*value);
                if (!color) return {};
                tone.colors[i] = *color;
            }
        }
        std::array<ColorArgb, 4> glow{};
        size_t found = 0;
        for (size_t i = 0; i < glow.size(); ++i) {
            if (const auto value = take(prefix + "glow_" + std::to_string(i))) {
                const auto color = parseColor(*value);
                if (!color) return {};
                glow[i] = *color;
                ++found;
            }
        }
        if (found != 0 && found != glow.size()) return {};
        if (found) tone.glow = glow;
    }
    if (!values.empty()) return {};
    return result;
}

inline Theme overlayBuiltinTone(const BuiltinStyleTone& edit, Theme base) {
    for (size_t i = 0; i < edit.colors.size(); ++i)
        if (edit.colors[i]) base.colors[i] = *edit.colors[i];
    if (edit.glow) base.glow = *edit.glow;
    return base;
}

} // namespace fangcun::skin
