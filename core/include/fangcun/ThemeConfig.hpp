#pragma once

#include "fangcun/WindowsThemeTokens.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string_view>

namespace fangcun {

enum class SkinPreset {
    SystemDefault,
    WoodType,
    Typewriter,
    Terminal,
    Brutal,
};

inline SkinPreset parseSkinPreset(std::string_view id) {
    if (id == "system_default") return SkinPreset::SystemDefault;
    if (id == "wood_type" || id == "swiss" || id == "square_cream" || id == "warm_grid") return SkinPreset::WoodType;
    if (id == "typewriter" || id == "eink" || id == "square_jade" || id == "mint") return SkinPreset::Typewriter;
    if (id == "terminal" || id == "metro" || id == "square_aluminum" || id == "native_light") return SkinPreset::Terminal;
    if (id == "brutal" || id == "square_amber" || id == "deep_blue") return SkinPreset::Brutal;
    return SkinPreset::SystemDefault;
}

enum class ThemeMode {
    System,
    Light,
    Dark,
};

struct ColorArgb {
    uint8_t a = 0xFF;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    static constexpr ColorArgb fromHex(uint32_t argb) {
        return ColorArgb{
            static_cast<uint8_t>((argb >> 24) & 0xFF),
            static_cast<uint8_t>((argb >> 16) & 0xFF),
            static_cast<uint8_t>((argb >> 8) & 0xFF),
            static_cast<uint8_t>(argb & 0xFF)
        };
    }

    constexpr uint32_t toColorRef() const {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16);
    }

    constexpr ColorArgb withAlpha(float alpha) const {
        float clamped = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
        return ColorArgb{
            static_cast<uint8_t>(clamped * 255.0f + 0.5f),
            r, g, b
        };
    }

    static ColorArgb lerp(ColorArgb x, ColorArgb y, float t) {
        float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        auto ch = [c](uint8_t a, uint8_t b) -> uint8_t {
            float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * c;
            int iv = static_cast<int>(v + 0.5f);
            return static_cast<uint8_t>(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
        };
        return ColorArgb{ch(x.a, y.a), ch(x.r, y.r), ch(x.g, y.g), ch(x.b, y.b)};
    }

    // sRGB relative luminance matching Jetpack Compose Color.luminance()
    float relativeLuminance() const {
        auto linearize = [](uint8_t u) -> float {
            float c = static_cast<float>(u) / 255.0f;
            return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        return 0.2126f * linearize(r) + 0.7152f * linearize(g) + 0.0722f * linearize(b);
    }
};

struct FangCunColors {
    ColorArgb keyboard;
    ColorArgb key;
    ColorArgb border;
    ColorArgb functionKey;
    ColorArgb text;
    ColorArgb secondary;
    ColorArgb accent;
    ColorArgb pressed;
    ColorArgb onAccent;
    ColorArgb onCandidate;
    ColorArgb hardShadow;

    bool isDarkKeyboard() const {
        return keyboard.relativeLuminance() < 0.2f;
    }
};

struct SkinMetrics {
    const char* id;
    const char* label;
    ThemeMode defaultThemeMode;
    float cornerRadiusDp;
    float restingShadowElevationDp;
    float ledgeDepthDp;
    std::array<ColorArgb, 4> glowColors;
};

// The supplied native palette also supplies the fallback settings window.
inline const SharpIme::WinThemeSpec& sharpTheme(SkinPreset skin) {
    switch (skin) {
        case SkinPreset::SystemDefault: return SharpIme::THEME_SYSTEM_DEFAULT;
        case SkinPreset::Typewriter: return SharpIme::THEME_TYPEWRITER;
        case SkinPreset::Terminal: return SharpIme::THEME_TERMINAL;
        case SkinPreset::Brutal: return SharpIme::THEME_BRUTAL;
        default: return SharpIme::THEME_WOOD_TYPE;
    }
}

inline const SharpIme::WinPalette& sharpPalette(SkinPreset skin, bool dark) {
    const auto& spec = sharpTheme(skin);
    return dark ? spec.dark : spec.light;
}

inline FangCunColors skinColors(SkinPreset skin, bool dark) {
    const auto& palette = sharpPalette(skin, dark);
    const auto color = ColorArgb::fromHex;
    return {color(palette.windowBg), color(dark ? 0xFF242424 : 0xFFFFFFFF),
        color(palette.windowOuterBorder), color(palette.windowHeaderBg), color(palette.textPrimary),
        color(palette.textSecondary), color(palette.accent), color(palette.candidateActiveBg),
        color(palette.accentText), color(palette.candidateActiveText), color(palette.hardShadowColor)};
}

inline ColorArgb accentForeground(const FangCunColors& colors) {
    return colors.onAccent;
}

// Accent fills keep their original hue. Small accent-colored labels need their
// own foreground check against the native settings plate and translucent cards.
inline ColorArgb readableSettingsAccent(const FangCunColors& colors) {
    const auto contrast = [](ColorArgb ink, ColorArgb surface) {
        const auto a = ink.relativeLuminance();
        const auto b = surface.relativeLuminance();
        return (std::max(a, b) + .05f) / (std::min(a, b) + .05f);
    };
    const auto card = ColorArgb::lerp(colors.keyboard, colors.key,
        colors.isDarkKeyboard() ? .35f : .55f);
    return contrast(colors.accent, colors.keyboard) >= 4.5f && contrast(colors.accent, card) >= 4.5f
        ? colors.accent : colors.text;
}

inline SkinMetrics skinMetrics(SkinPreset skin) {
    const std::array<ColorArgb, 4> glow{{ColorArgb::fromHex(0xFF92B8DF), ColorArgb::fromHex(0xFF83C9D0),
        ColorArgb::fromHex(0xFFB7BCE2), ColorArgb::fromHex(0xFFA5BDCF)}};
    switch (skin) {
        case SkinPreset::WoodType: return {"wood_type", "本地皮肤", ThemeMode::System, 0, 0, 0, glow};
        case SkinPreset::Typewriter: return {"typewriter", "本地皮肤", ThemeMode::System, 0, 0, 0, glow};
        case SkinPreset::Terminal: return {"terminal", "本地皮肤", ThemeMode::System, 0, 0, 0, glow};
        case SkinPreset::Brutal: return {"brutal", "本地皮肤", ThemeMode::System, 0, 0, 2, glow};
        default: return {"system_default", "系统默认", ThemeMode::System, 0, 0, 0, glow};
    }
}

inline bool resolveEffectiveDarkMode(ThemeMode mode, SkinPreset skin, bool systemIsDark) {
    switch (mode) {
        case ThemeMode::Light: return false;
        case ThemeMode::Dark: return true;
        case ThemeMode::System: return systemIsDark;
    }
    return skinMetrics(skin).defaultThemeMode == ThemeMode::Dark;
}

struct GlassKeyColors {
    ColorArgb top;
    ColorArgb bottom;
    ColorArgb ledge;
    ColorArgb edge;
    ColorArgb highlight;
    ColorArgb lowerEdge;
    float cornerRadiusDp;
    float ledgeDepthDp;
    float depressionDp;
    float shadowElevationDp;
    float borderWidthDp;
    float topHighlightWidthDp;
    float lowerEdgeWidthDp;
};

inline GlassKeyColors computeGlassKeyColors(
    SkinPreset skin, const FangCunColors& colors, float keyOpacity = 0.82f,
    bool pressed = false, bool functional = false, bool emphasized = false, bool selected = false) {
    const bool brutal = skin == SkinPreset::Brutal;
    auto fill = emphasized ? colors.accent : (selected ? colors.pressed : (functional ? colors.functionKey : colors.key));
    if (!emphasized && !selected) fill = fill.withAlpha(std::clamp(keyOpacity, 0.4f, 1.0f));
    const auto lowerFace = brutal ? fill : ColorArgb::lerp(fill, colors.border, .18f);
    return {fill, lowerFace, colors.hardShadow, colors.border, {}, {},
        skinMetrics(skin).cornerRadiusDp, brutal ? 2.0f : 0.0f,
        pressed && brutal ? 2.0f : 0.0f, 0.0f, brutal ? 1.5f : 1.0f, 0.0f, 0.0f};
}

struct BackdropGradientColors {
    bool isFlat = false;
    ColorArgb top;
    ColorArgb middle;
    ColorArgb bottom;
};

inline BackdropGradientColors computeBackdropGradient(SkinPreset, const FangCunColors& colors) {
    return {true, colors.keyboard, colors.keyboard, colors.keyboard};
}

// Header & Candidate layout constants from KeyboardHeaderLayout.kt & theme-tokens.json
namespace HeaderMetricsSpec {
    inline constexpr float kHeaderMinimumDp = 48.0f;
    inline constexpr float kCandidateSlotMinimumDp = 40.0f;
    inline constexpr int kFirstCandidateDefaultSp = 20;
    inline constexpr int kFirstCandidateMinSp = 16;
    inline constexpr int kFirstCandidateMaxSp = 30;
    inline constexpr int kOtherCandidateSp = 17;
    inline constexpr int kCandidateIndexSp = 9;
    inline constexpr float kCandidateIndexLineHeightSp = 12.0f;
    inline constexpr int kCodeSp = 11;
    inline constexpr float kCodeLineHeightSp = 14.0f;
    inline constexpr float kFirstBackgroundAlpha = 1.0f;
    inline constexpr float kFirstCornerDp = 0.0f;
    inline constexpr float kPendingAlpha = 0.5f;
    inline constexpr float kItemHorizontalPaddingDp = 10.0f;
    inline constexpr float kItemVerticalPaddingDp = 5.0f;
    inline constexpr const char* kMeasurementProbe = "AgjyÅ国\xF0\xA0\x80\x80\xF0\x9F\x98\x80\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\x92\xBB";
}

// Brand Tianzige (田字格) logo constants from theme-tokens.json
namespace BrandMark {
    inline constexpr ColorArgb FrameColor{0xFF, 0x29, 0x2B, 0x27};
    inline constexpr ColorArgb CenterTerracotta{0xFF, 0xB4, 0x55, 0x2D};
    inline constexpr ColorArgb CreamBackground{0xFF, 0xF4, 0xEE, 0xE2};
}

// Exact port of TypingGlow.kt state machine & pulse geometry
struct GlowPulse {
    int64_t id = 0;
    bool hasPosition = false;
    float x = 0.0f;
    float y = 0.0f;
    bool hasStarted = false;
    int64_t startedAtNanos = 0;
};

struct GlowRenderCircle {
    float cx = 0.0f;
    float cy = 0.0f;
    float radius = 0.0f;
    ColorArgb innerColor;
    ColorArgb midColor;
    bool isTrailing = false;
};

class TypingGlowEngine {
public:
    static constexpr int64_t kDurationNanos = 1'100'000'000LL;
    static constexpr int kMaxPulses = 6;
    static constexpr float kStartRadiusDp = 28.0f;
    static constexpr float kFadeInProgress = 0.065f;

    void setEnabled(bool enabled) {
        enabled_ = enabled;
        if (!enabled_) clear();
    }

    bool isEnabled() const { return enabled_; }

    void setMotionAllowed(bool allowed) {
        motionAllowed_ = allowed;
        if (!motionAllowed_) clear();
    }

    bool isMotionAllowed() const { return motionAllowed_; }

    int activeCount() const { return static_cast<int>(pulses_.size()); }

    void trigger(bool hasPos = false, float x = 0.0f, float y = 0.0f) {
        if (!enabled_ || !motionAllowed_) {
            clear();
            return;
        }
        if (hasPos && (!std::isfinite(x) || !std::isfinite(y))) return;
        if (static_cast<int>(pulses_.size()) >= kMaxPulses) {
            pulses_.erase(pulses_.begin());
        }
        pulses_.push_back(GlowPulse{nextId_++, hasPos, x, y, false, 0});
    }

    void clear() {
        pulses_.clear();
        frameTimeNanos_ = 0;
    }

    void advanceFrame(int64_t frameNanos) {
        if (!enabled_ || !motionAllowed_) {
            clear();
            return;
        }
        if (frameNanos < frameTimeNanos_) return;
        frameTimeNanos_ = frameNanos;
        for (int i = static_cast<int>(pulses_.size()) - 1; i >= 0; --i) {
            auto& p = pulses_[static_cast<size_t>(i)];
            if (!p.hasStarted) {
                p.hasStarted = true;
                p.startedAtNanos = frameNanos;
            } else if (frameNanos - p.startedAtNanos >= kDurationNanos) {
                pulses_.erase(pulses_.begin() + i);
            }
        }
    }

    std::vector<GlowRenderCircle> buildRenderCircles(
        float width,
        float height,
        float dpScale,
        bool dark,
        const std::array<ColorArgb, 4>& palette) const {
        std::vector<GlowRenderCircle> out;
        if (!enabled_ || !motionAllowed_ || pulses_.empty() || width <= 0.0f || height <= 0.0f) {
            return out;
        }
        const float minDim = std::min(width, height);
        const float startRadius = kStartRadiusDp * dpScale;
        const float maxRadius = std::min(180.0f * dpScale, std::max(100.0f * dpScale, minDim * 0.78f));

        for (const auto& pulse : pulses_) {
            int64_t age = pulse.hasStarted ? (frameTimeNanos_ - pulse.startedAtNanos) : 0;
            float progress = std::clamp(static_cast<float>(age) / static_cast<float>(kDurationNanos), 0.0f, 1.0f);
            float fadeIn = std::min(progress / kFadeInProgress, 1.0f);
            float intensity = fadeIn * std::pow(1.0f - progress, 1.5f);
            if (intensity <= 0.0f) continue;

            float expansion = 1.0f - std::pow(1.0f - progress, 3.0f);
            float radius = startRadius + (maxRadius - startRadius) * expansion;
            float direction = (pulse.id % 2 == 0) ? 1.0f : -1.0f;
            float ox = pulse.hasPosition ? pulse.x : (width * 0.5f);
            float oy = pulse.hasPosition ? pulse.y : (height * 0.5f);
            float cx = ox + (width * 0.10f * direction) * progress;
            float cy = oy + (-height * 0.16f) * progress;

            size_t colorIdx = static_cast<size_t>(pulse.id % 4);
            ColorArgb first = palette[colorIdx];
            ColorArgb second = palette[(colorIdx + 2) % 4];
            float alpha = intensity * (dark ? 0.65f : 0.78f);

            out.push_back(GlowRenderCircle{
                cx, cy, radius,
                first.withAlpha(alpha),
                second.withAlpha(alpha * 0.65f),
                false
            });
            out.push_back(GlowRenderCircle{
                cx + radius * 0.42f * direction,
                cy - radius * 0.15f,
                radius * 0.7f,
                second.withAlpha(alpha * 0.48f),
                second.withAlpha(0.0f),
                true
            });
        }
        return out;
    }

private:
    bool enabled_ = false;
    bool motionAllowed_ = true;
    std::vector<GlowPulse> pulses_;
    int64_t frameTimeNanos_ = 0;
    int64_t nextId_ = 0;
};

} // namespace fangcun

