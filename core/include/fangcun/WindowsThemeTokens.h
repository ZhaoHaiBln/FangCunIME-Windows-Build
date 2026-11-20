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
    0xFFF0DFC3, 0xFFE8D6BA, 0xFF493629, 0xFFB09270, 0xFF241B14, 0xFF604935,
    0xFF963E2C, 0xFFFFF5DE, 0xFFF4DBB9, 0xFF8B1A10, 0x00000000,
};
inline constexpr WinPalette TYPEWRITER_PALETTE{
    0xFFF1DFC0, 0xFFEBDDCA, 0xFF34302B, 0xFF9D825F, 0xFF211B14, 0xFF64503B,
    0xFF745633, 0xFFFFEFCF, 0xFF29251D, 0xFFFFEFCF, 0x00000000,
};
inline constexpr WinPalette TERMINAL_PALETTE{
    0xFF152219, 0xFF17251A, 0xFFC4BAA2, 0xFF344838, 0xFFA2F484, 0xFF85BD74,
    0xFFEAD17B, 0xFF22231A, 0xFF83F064, 0xFF142411, 0x00000000,
};

inline constexpr WinThemeSpec THEME_WOOD_TYPE{
    "wood_type", {7, 0, false, 3.0f}, WOOD_PALETTE,
    {0xFF3C2D24, 0xFF3C2D24, 0xFF584031, 0xFF775A44, 0xFFF5E3CC, 0xFFCDB69B,
        0xFFA84834, 0xFFFFF5DE, 0xFF56352A, 0xFFFFB294, 0x00000000},
};
inline constexpr WinThemeSpec THEME_TYPEWRITER{
    "typewriter", {7, 0, false, 4.0f}, TYPEWRITER_PALETTE,
    {0xFF302923, 0xFF302923, 0xFF191615, 0xFF7F705F, 0xFFF6E8D1, 0xFFCBBA9B,
        0xFF795936, 0xFFFFEECF, 0xFF201C19, 0xFFFFEECF, 0x00000000},
};
inline constexpr WinThemeSpec THEME_TERMINAL{
    "terminal", {7, 0, true, 3.0f}, TERMINAL_PALETTE,
    {0xFF0E180F, 0xFF152218, 0xFF6E7265, 0xFF304637, 0xFF9CE787, 0xFF83BC75,
        0xFFE2C773, 0xFF282416, 0xFF75DF5B, 0xFF142411, 0x00000000},
};

inline constexpr WinThemeSpec THEME_BRUTAL = {
    "brutal",
    {2, 4, true, 0.0f},
    /* Light */ { 0xFFFFFDF8, 0xFFEFEAE0, 0xFF121212, 0xFF121212, 0xFF121212, 0xFF5E5950, 0xFFB8F500, 0xFF121212, 0xFFB8F500, 0xFF121212, 0xFF121212 },
    /* Dark  */ { 0xFF141A17, 0xFF0F1412, 0xFF00F078, 0xFF46574F, 0xFFE6F4EC, 0xFF6F877B, 0xFF00F078, 0xFF08120C, 0xFF00F078, 0xFF08120C, 0xFF000000 }
};

} // namespace SharpIme
