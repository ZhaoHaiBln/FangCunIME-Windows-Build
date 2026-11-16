#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fangcun {

enum class SkinPreset {
    NativeLight, // 原生轻白
    WarmGrid,    // 米白田字格
    Mint,        // 薄荷绿
    DeepBlue,    // 深蓝珊瑚
};

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

// Exact brand palette ported from Android Theme.kt & theme-tokens.json (v1.5.1)
inline FangCunColors skinColors(SkinPreset skin, bool dark) {
    switch (skin) {
        case SkinPreset::NativeLight:
            return dark
                ? FangCunColors{{0xFF,0x19,0x1D,0x26}, {0xFF,0x30,0x36,0x43}, {0xFF,0x47,0x50,0x5F}, {0xFF,0x3C,0x43,0x50}, {0xFF,0xF2,0xF5,0xFA}, {0xFF,0xAF,0xBA,0xCB}, {0xFF,0x89,0xB4,0xFF}, {0xFF,0x2B,0x3A,0x55}}
                : FangCunColors{{0xFF,0xEE,0xF1,0xF6}, {0xFF,0xFF,0xFF,0xFF}, {0xFF,0xC9,0xD0,0xDC}, {0xFF,0xDF,0xE5,0xEE}, {0xFF,0x20,0x26,0x32}, {0xFF,0x62,0x6D,0x80}, {0xFF,0x17,0x63,0xE8}, {0xFF,0xE5,0xEE,0xFF}};
        case SkinPreset::WarmGrid:
            return dark
                ? FangCunColors{{0xFF,0x24,0x23,0x1E}, {0xFF,0x37,0x36,0x2E}, {0xFF,0x55,0x53,0x45}, {0xFF,0x45,0x42,0x37}, {0xFF,0xF4,0xF1,0xE6}, {0xFF,0xC2,0xBE,0xAA}, {0xFF,0xEA,0xA4,0x81}, {0xFF,0x49,0x33,0x26}}
                : FangCunColors{{0xFF,0xF1,0xEE,0xE5}, {0xFF,0xFF,0xFE,0xF9}, {0xFF,0xCD,0xC7,0xB9}, {0xFF,0xE4,0xDF,0xD3}, {0xFF,0x30,0x2E,0x26}, {0xFF,0x6F,0x6B,0x5E}, {0xFF,0xA3,0x4E,0x2D}, {0xFF,0xF3,0xE3,0xD6}};
        case SkinPreset::Mint:
            return dark
                ? FangCunColors{{0xFF,0x14,0x27,0x22}, {0xFF,0x25,0x40,0x39}, {0xFF,0x3D,0x5E,0x52}, {0xFF,0x30,0x4E,0x43}, {0xFF,0xED,0xF8,0xF2}, {0xFF,0xA1,0xC8,0xB7}, {0xFF,0x8A,0xD9,0xB7}, {0xFF,0x23,0x4B,0x3A}}
                : FangCunColors{{0xFF,0xE9,0xF3,0xEF}, {0xFF,0xFA,0xFF,0xFD}, {0xFF,0xBF,0xD7,0xCC}, {0xFF,0xD6,0xE8,0xE0}, {0xFF,0x20,0x3E,0x34}, {0xFF,0x56,0x77,0x6A}, {0xFF,0x17,0x73,0x59}, {0xFF,0xDD,0xEF,0xE5}};
        case SkinPreset::DeepBlue:
            return dark
                ? FangCunColors{{0xFF,0x17,0x28,0x3F}, {0xFF,0x2A,0x3D,0x58}, {0xFF,0x49,0x60,0x78}, {0xFF,0x34,0x4C,0x69}, {0xFF,0xF2,0xF6,0xFC}, {0xFF,0xAD,0xBF,0xD4}, {0xFF,0xFF,0xAD,0x9A}, {0xFF,0x33,0x4E,0x6A}}
                : FangCunColors{{0xFF,0xE9,0xEF,0xF6}, {0xFF,0xF9,0xFC,0xFF}, {0xFF,0xBE,0xCB,0xDC}, {0xFF,0xD5,0xDF,0xEC}, {0xFF,0x22,0x3B,0x58}, {0xFF,0x5F,0x75,0x8E}, {0xFF,0xAD,0x49,0x38}, {0xFF,0xF7,0xE4,0xDF}};
    }
    return skinColors(SkinPreset::NativeLight, false);
}

inline SkinMetrics skinMetrics(SkinPreset skin) {
    switch (skin) {
        case SkinPreset::NativeLight:
            return SkinMetrics{
                "native_light", "原生轻白", ThemeMode::Light,
                4.0f, 1.8f, 2.1f,
                {{
                    {0xFF,0x6B,0x9F,0xF0},
                    {0xFF,0x64,0xC9,0xDF},
                    {0xFF,0xA3,0x94,0xEB},
                    {0xFF,0x7A,0xB5,0xEF}
                }}
            };
        case SkinPreset::WarmGrid:
            return SkinMetrics{
                "warm_grid", "米白田字格", ThemeMode::Light,
                2.0f, 1.2f, 1.8f,
                {{
                    {0xFF,0xE9,0xAD,0x82},
                    {0xFF,0x88,0xBD,0xA3},
                    {0xFF,0xD4,0x9F,0xAD},
                    {0xFF,0xE2,0xC6,0x8C}
                }}
            };
        case SkinPreset::Mint:
            return SkinMetrics{
                "mint", "薄荷绿", ThemeMode::Light,
                4.0f, 1.6f, 2.1f,
                {{
                    {0xFF,0x64,0xCB,0xB7},
                    {0xFF,0x7D,0xD6,0xDF},
                    {0xFF,0xAB,0xAC,0xE8},
                    {0xFF,0x92,0xD1,0xA5}
                }}
            };
        case SkinPreset::DeepBlue:
            return SkinMetrics{
                "deep_blue", "深蓝珊瑚", ThemeMode::Dark,
                4.0f, 2.0f, 2.1f,
                {{
                    {0xFF,0x66,0x9B,0xEC},
                    {0xFF,0xFF,0x9A,0x87},
                    {0xFF,0x73,0xD9,0xC3},
                    {0xFF,0xB4,0x9A,0xDF}
                }}
            };
    }
    return skinMetrics(SkinPreset::NativeLight);
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

// Exact port of GlassSurface.kt glassKeySurface()
inline GlassKeyColors computeGlassKeyColors(
    SkinPreset skin,
    const FangCunColors& colors,
    float keyOpacity = 0.82f,
    bool pressed = false,
    bool functional = false,
    bool emphasized = false,
    bool selected = false) {
    const SkinMetrics metrics = skinMetrics(skin);
    const bool warmGrid = (skin == SkinPreset::WarmGrid);
    const bool dark = colors.isDarkKeyboard();

    const ColorArgb white{0xFF, 0xFF, 0xFF, 0xFF};
    const ColorArgb black{0xFF, 0x00, 0x00, 0x00};

    ColorArgb fill = colors.key;
    if (emphasized) fill = colors.accent;
    else if (pressed || selected) fill = colors.pressed;
    else if (functional) fill = colors.functionKey;

    float clampedOpacity = std::clamp(keyOpacity, 0.4f, 1.0f);
    float alpha = (emphasized || selected) ? 1.0f : clampedOpacity;

    ColorArgb top = emphasized
        ? fill
        : ColorArgb::lerp(fill, white, dark ? 0.055f : 0.045f).withAlpha(alpha);
    ColorArgb bottom = emphasized
        ? ColorArgb::lerp(fill, black, 0.045f)
        : ColorArgb::lerp(fill, colors.keyboard, dark ? 0.055f : 0.065f).withAlpha(alpha);
    ColorArgb ledge = ColorArgb::lerp(fill, black, dark ? 0.26f : (warmGrid ? 0.13f : 0.16f)).withAlpha(alpha);

    ColorArgb edge = emphasized
        ? colors.accent.withAlpha(0.7f)
        : ((pressed || selected)
            ? colors.accent.withAlpha(dark ? 0.65f : 0.5f)
            : colors.border.withAlpha(warmGrid ? 0.8f : (dark ? 0.7f : 0.55f)));

    ColorArgb highlight = white.withAlpha(emphasized ? 0.22f : (dark ? 0.19f : 0.72f));
    ColorArgb lowerEdge = black.withAlpha(dark ? 0.36f : (warmGrid ? 0.2f : 0.24f));

    return GlassKeyColors{
        top,
        bottom,
        ledge,
        edge,
        highlight,
        lowerEdge,
        metrics.cornerRadiusDp,
        metrics.ledgeDepthDp,
        pressed ? 0.8f : 0.0f,
        pressed ? 0.25f : metrics.restingShadowElevationDp,
        0.6f,
        0.7f,
        pressed ? 0.45f : 0.85f
    };
}

struct BackdropGradientColors {
    bool isFlat = false;
    ColorArgb top;
    ColorArgb middle;
    ColorArgb bottom;
};

// Exact port of GlassSurface.kt glassKeyboardBackground()
inline BackdropGradientColors computeBackdropGradient(SkinPreset skin, const FangCunColors& colors) {
    if (skin == SkinPreset::WarmGrid) {
        return BackdropGradientColors{true, colors.keyboard, colors.keyboard, colors.keyboard};
    }
    const bool dark = colors.isDarkKeyboard();
    const ColorArgb white{0xFF, 0xFF, 0xFF, 0xFF};
    const ColorArgb black{0xFF, 0x00, 0x00, 0x00};

    ColorArgb upper = ColorArgb::lerp(colors.keyboard, dark ? colors.key : white, dark ? 0.12f : 0.16f);
    ColorArgb lower = ColorArgb::lerp(colors.keyboard, dark ? black : colors.functionKey, dark ? 0.04f : 0.08f);
    return BackdropGradientColors{false, upper, colors.keyboard, lower};
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
    inline constexpr float kFirstBackgroundAlpha = 0.035f;
    inline constexpr float kFirstCornerDp = 4.0f;
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

