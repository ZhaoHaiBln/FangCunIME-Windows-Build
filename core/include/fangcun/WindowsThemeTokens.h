#pragma once
#include <stdint.h>

namespace SharpIme {

struct WinPalette {
    uint32_t windowBg;             // 0xAARRGGBB
    uint32_t windowHeaderBg;       // Preedit / status header strip
    uint32_t windowOuterBorder;    // Outer 1px-2px frame
    uint32_t innerDivider;         // Internal 1px grid / row divider
    uint32_t textPrimary;          // Candidate & preedit text
    uint32_t textSecondary;        // Index numbers, page count, aux hints
    uint32_t accent;               // Caret, indicator bar, brand block
    uint32_t accentText;           // Text on accent block
    uint32_t candidateActiveBg;    // Highlighted #1 / selected candidate row/cell
    uint32_t candidateActiveText;  // Highlighted candidate text
    uint32_t hardShadowColor;      // Used when hardShadowOffsetPx > 0 (Brutal theme)
};

struct WinMetrics {
    int outerBorderWidthPx;
    int hardShadowOffsetPx;
    bool useMonospaceCode;
    float candidateRadiusDp;
};

struct WinThemeSpec {
    const char* id;
    WinMetrics metrics;
    WinPalette light;
    WinPalette dark;
};

// The application has a neutral emergency appearance only. Named skins and
// their palettes are supplied by validated .fcskin files.
inline constexpr WinPalette SYSTEM_LIGHT{
    0xFFF3F3F3, 0xFFE9E9E9, 0xFFB8B8B8, 0xFFB8B8B8, 0xFF202020, 0xFF606060,
    0xFF404040, 0xFFFFFFFF, 0xFF404040, 0xFFFFFFFF, 0x00000000,
};
inline constexpr WinPalette SYSTEM_DARK{
    0xFF181818, 0xFF303030, 0xFF626262, 0xFF626262, 0xFFF0F0F0, 0xFFB0B0B0,
    0xFFD0D0D0, 0xFF181818, 0xFFD0D0D0, 0xFF181818, 0x00000000,
};
inline constexpr WinThemeSpec THEME_SYSTEM_DEFAULT{
    "system_default", {1, 0, false, 0.0f}, SYSTEM_LIGHT, SYSTEM_DARK,
};
// The four legacy IDs are layout bases in .fcskin v1, not selectable skins.
// Until a local file finishes loading, every base uses neutral colors.
inline constexpr WinThemeSpec THEME_WOOD_TYPE{
    "wood_type", {1, 0, false, 0.0f}, SYSTEM_LIGHT, SYSTEM_DARK,
};
inline constexpr WinThemeSpec THEME_TYPEWRITER{
    "typewriter", {1, 0, false, 0.0f}, SYSTEM_LIGHT, SYSTEM_DARK,
};
inline constexpr WinThemeSpec THEME_TERMINAL{
    "terminal", {1, 0, false, 0.0f}, SYSTEM_LIGHT, SYSTEM_DARK,
};
inline constexpr WinThemeSpec THEME_BRUTAL{
    "brutal", {2, 4, true, 0.0f}, SYSTEM_LIGHT, SYSTEM_DARK,
};

} // namespace SharpIme
