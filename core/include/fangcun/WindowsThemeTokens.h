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

// The three reference materials use the same palette in both system modes.
inline constexpr WinPalette WOOD_PALETTE{
    0xFFF0DFC3, 0xFFECDCBE, 0xFF493629, 0xFFB09270, 0xFF241B14, 0xFF604935,
    0xFF963E2C, 0xFFFFF5DE, 0xFFF4DBB9, 0xFF8B1A10, 0x00000000,
};
inline constexpr WinPalette TYPEWRITER_PALETTE{
    0xFFF1DFC0, 0xFF191816, 0xFF1B1915, 0xFF9D825F, 0xFF211B14, 0xFF64503B,
    0xFF745633, 0xFFFFEFCF, 0xFF29251D, 0xFFFFEFCF, 0x00000000,
};
inline constexpr WinPalette TERMINAL_PALETTE{
    0xFF151E15, 0xFFD3C9B7, 0xFFB3A892, 0xFF40543B, 0xFFA5F492, 0xFF91BD86,
    0xFF303535, 0xFFF5ECDD, 0xFF161F16, 0xFFAAFC98, 0x00000000,
};

inline constexpr WinThemeSpec THEME_WOOD_TYPE{
    "wood_type", {3, 0, false, 3.0f}, WOOD_PALETTE, WOOD_PALETTE,
};
inline constexpr WinThemeSpec THEME_TYPEWRITER{
    "typewriter", {3, 0, false, 6.0f}, TYPEWRITER_PALETTE, TYPEWRITER_PALETTE,
};
inline constexpr WinThemeSpec THEME_TERMINAL{
    "terminal", {3, 0, true, 0.0f}, TERMINAL_PALETTE, TERMINAL_PALETTE,
};

inline constexpr WinThemeSpec THEME_BRUTAL = {
    "brutal",
    {2, 4, true, 0.0f},
    /* Light */ { 0xFFFFFDF8, 0xFFEFEAE0, 0xFF121212, 0xFF121212, 0xFF121212, 0xFF5E5950, 0xFFB8F500, 0xFF121212, 0xFFB8F500, 0xFF121212, 0xFF121212 },
    /* Dark  */ { 0xFF141A17, 0xFF0F1412, 0xFF00F078, 0xFF46574F, 0xFFE6F4EC, 0xFF6F877B, 0xFF00F078, 0xFF08120C, 0xFF00F078, 0xFF08120C, 0xFF000000 }
};

} // namespace SharpIme
