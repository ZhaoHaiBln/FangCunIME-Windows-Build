#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string_view>

namespace fangcun {

enum class SkinPreset {
    SquareCream,
    SquareAluminum,
    SquareAmber,
    SquareJade,
    // Source aliases remain valid; persisted INI aliases decode below.
    NativeLight = SquareAluminum,
    WarmGrid = SquareCream,
    Mint = SquareJade,
    DeepBlue = SquareAmber,
};

inline SkinPreset parseSkinPreset(std::string_view id) {
    if (id == "square_aluminum" || id == "native_light") return SkinPreset::SquareAluminum;
    if (id == "square_amber" || id == "deep_blue") return SkinPreset::SquareAmber;
    if (id == "square_jade" || id == "mint") return SkinPreset::SquareJade;
    return SkinPreset::SquareCream;
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

// Shared four-skin palettes, kept byte-for-byte with Android Theme.kt.
inline FangCunColors skinColors(SkinPreset skin, bool dark) {
    switch (skin) {
        case SkinPreset::SquareCream:
            return dark ? FangCunColors{ColorArgb::fromHex(0xFF242723), ColorArgb::fromHex(0xFF383D34), ColorArgb::fromHex(0xFF5F6959), ColorArgb::fromHex(0xFF454B3F), ColorArgb::fromHex(0xFFF5F3EA), ColorArgb::fromHex(0xFFB8C0AD), ColorArgb::fromHex(0xFFB4D49B), ColorArgb::fromHex(0xFF4D5C44)}
                : FangCunColors{ColorArgb::fromHex(0xFFF0EDE4), ColorArgb::fromHex(0xFFFFFEFA), ColorArgb::fromHex(0xFFD2CFC4), ColorArgb::fromHex(0xFFE7E4DB), ColorArgb::fromHex(0xFF292D29), ColorArgb::fromHex(0xFF62665D), ColorArgb::fromHex(0xFF315F37), ColorArgb::fromHex(0xFFE0E8D9)};
        case SkinPreset::SquareAluminum:
            return dark ? FangCunColors{ColorArgb::fromHex(0xFF232A32), ColorArgb::fromHex(0xFF3A4551), ColorArgb::fromHex(0xFF657588), ColorArgb::fromHex(0xFF465463), ColorArgb::fromHex(0xFFF2F6FA), ColorArgb::fromHex(0xFFB0BECE), ColorArgb::fromHex(0xFF9BC8FA), ColorArgb::fromHex(0xFF3D5D7C)}
                : FangCunColors{ColorArgb::fromHex(0xFFCFD4DA), ColorArgb::fromHex(0xFFEFF2F5), ColorArgb::fromHex(0xFF929DA9), ColorArgb::fromHex(0xFFD4D9E0), ColorArgb::fromHex(0xFF232C36), ColorArgb::fromHex(0xFF4F5C69), ColorArgb::fromHex(0xFF075EAC), ColorArgb::fromHex(0xFFD1E4F3)};
        case SkinPreset::SquareAmber:
            return dark ? FangCunColors{ColorArgb::fromHex(0xFF20211F), ColorArgb::fromHex(0xFF383A35), ColorArgb::fromHex(0xFF68695E), ColorArgb::fromHex(0xFF45473F), ColorArgb::fromHex(0xFFF8F4E8), ColorArgb::fromHex(0xFFC0BDAD), ColorArgb::fromHex(0xFFF1AD3E), ColorArgb::fromHex(0xFF5C4B31)}
                : FangCunColors{ColorArgb::fromHex(0xFFE5E0D7), ColorArgb::fromHex(0xFFF9F5EC), ColorArgb::fromHex(0xFFB7AA93), ColorArgb::fromHex(0xFFD8D0C0), ColorArgb::fromHex(0xFF302C24), ColorArgb::fromHex(0xFF695C48), ColorArgb::fromHex(0xFF955200), ColorArgb::fromHex(0xFFF1DFC1)};
        case SkinPreset::SquareJade:
            return dark ? FangCunColors{ColorArgb::fromHex(0xFF1D2D27), ColorArgb::fromHex(0xFF304D3E), ColorArgb::fromHex(0xFF607F6C), ColorArgb::fromHex(0xFF3D5B4A), ColorArgb::fromHex(0xFFF0F8EE), ColorArgb::fromHex(0xFFB4CCBA), ColorArgb::fromHex(0xFFA0D9B6), ColorArgb::fromHex(0xFF416850)}
                : FangCunColors{ColorArgb::fromHex(0xFFDCECE4), ColorArgb::fromHex(0xFFF0F8F1), ColorArgb::fromHex(0xFFB0CBBE), ColorArgb::fromHex(0xFFDBEAE0), ColorArgb::fromHex(0xFF233E32), ColorArgb::fromHex(0xFF526C5D), ColorArgb::fromHex(0xFF156448), ColorArgb::fromHex(0xFFC7E4D4)};
    }
    return skinColors(SkinPreset::SquareCream, false);
}

inline ColorArgb accentForeground(const FangCunColors& colors) {
    const float luminance = colors.accent.relativeLuminance();
    const float whiteContrast = 1.05f / (luminance + 0.05f);
    const float blackContrast = (luminance + 0.05f) / 0.05f;
    return whiteContrast >= blackContrast
        ? ColorArgb::fromHex(0xFFFFFFFF) : ColorArgb::fromHex(0xFF000000);
}

inline SkinMetrics skinMetrics(SkinPreset skin) {
    switch (skin) {
        case SkinPreset::SquareCream:
            return SkinMetrics{"square_cream", "奶油方格", ThemeMode::Light,
                0.0f, 1.4f, 2.2f,
                {{ColorArgb::fromHex(0xFFACCAA2), ColorArgb::fromHex(0xFFD7C5A6), ColorArgb::fromHex(0xFFC0D3B2), ColorArgb::fromHex(0xFFD9CFB5)}}};
        case SkinPreset::SquareAluminum:
            return SkinMetrics{"square_aluminum", "铝银机械", ThemeMode::Light,
                0.0f, 1.6f, 2.6f,
                {{ColorArgb::fromHex(0xFF92B8DF), ColorArgb::fromHex(0xFF83C9D0), ColorArgb::fromHex(0xFFB7BCE2), ColorArgb::fromHex(0xFFA5BDCF)}}};
        case SkinPreset::SquareAmber:
            return SkinMetrics{"square_amber", "墨黑琥珀", ThemeMode::Dark,
                0.0f, 1.6f, 2.6f,
                {{ColorArgb::fromHex(0xFFDDAA51), ColorArgb::fromHex(0xFFCDBA85), ColorArgb::fromHex(0xFFE4C187), ColorArgb::fromHex(0xFFB1A488)}}};
        case SkinPreset::SquareJade:
            return SkinMetrics{"square_jade", "翡翠磨砂", ThemeMode::Light,
                0.0f, 1.1f, 1.9f,
                {{ColorArgb::fromHex(0xFF91CDB0), ColorArgb::fromHex(0xFFB9DDC7), ColorArgb::fromHex(0xFFA5CEBF), ColorArgb::fromHex(0xFFCADDB5)}}};
    }
    return skinMetrics(SkinPreset::SquareCream);
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

