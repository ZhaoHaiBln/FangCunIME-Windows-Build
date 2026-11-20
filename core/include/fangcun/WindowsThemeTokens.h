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

inline constexpr WinPalette WOOD_PALETTE{
    0xFFF3E4CC, 0xFFEAD9C7, 0xFF472A16, 0xFFAA9A81, 0xFF241C14, 0xFF6A5744,
    0xFF3A2E22, 0xFFFFF5DE, 0xFFF3E4CC, 0xFF9E2F1E, 0x00000000,
};
inline constexpr WinPalette TYPEWRITER_PALETTE{
    0xFFF6E8D0, 0xFFE6DACA, 0xFF3A3530, 0x00000000, 0xFF221E1A, 0xFF64503B,
    0xFF237218, 0xFFF7EEDB, 0xFF2B2723, 0xFFF7EEDB, 0x00000000,
};
inline constexpr WinPalette TERMINAL_PALETTE{
    0xFF0F1E12, 0xFF102013, 0xFFD4CCB9, 0xFF214526, 0xFF8DEE6F, 0xFF8DEE6F,
    0xFFF4E667, 0xFF0B1A0D, 0xFF78F867, 0xFF0B1A0D, 0x00000000,
};

inline constexpr WinThemeSpec THEME_WOOD_TYPE{
    "wood_type", {6, 0, false, 3.5f}, WOOD_PALETTE,
    {0xFF3C2D24, 0xFF3C2D24, 0xFF584031, 0xFF775A44, 0xFFF5E3CC, 0xFFCDB69B,
        0xFFA84834, 0xFFFFF5DE, 0xFF56352A, 0xFFFFB294, 0x00000000},
};
inline constexpr WinThemeSpec THEME_TYPEWRITER{
    "typewriter", {6, 0, true, 4.0f}, TYPEWRITER_PALETTE,
    {0xFF302923, 0xFF302923, 0xFF191615, 0x00000000, 0xFFF6E8D1, 0xFFCBBA9B,
        0xFF237218, 0xFFF7EEDB, 0xFF201C19, 0xFFF7EEDB, 0x00000000},
};
inline constexpr WinThemeSpec THEME_TERMINAL{
    "terminal", {6, 0, true, 3.5f}, TERMINAL_PALETTE,
    {0xFF0A140C, 0xFF0E1B11, 0xFF6E7265, 0xFF214526, 0xFF8DEE6F, 0xFF8DEE6F,
        0xFFF4E667, 0xFF0B1A0D, 0xFF78F867, 0xFF0B1A0D, 0x00000000},
};

inline constexpr WinThemeSpec THEME_BRUTAL = {
    "brutal",
    {2, 4, true, 0.0f},
    /* Light */ { 0xFFFFFDF8, 0xFFEFEAE0, 0xFF121212, 0xFF121212, 0xFF121212, 0xFF5E5950, 0xFFB8F500, 0xFF121212, 0xFFB8F500, 0xFF121212, 0xFF121212 },
    /* Dark  */ { 0xFF141A17, 0xFF0F1412, 0xFF00F078, 0xFF46574F, 0xFFE6F4EC, 0xFF6F877B, 0xFF00F078, 0xFF08120C, 0xFF00F078, 0xFF08120C, 0xFF000000 }
};

} // namespace SharpIme
