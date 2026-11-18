#pragma once
#include <stdint.h>

/**
 * SharpIME Pure Right-Angle (0px Corner Radius) Theme Definitions for Windows
 * Suitable for Win32 / Direct2D / GDI+ / TSF IME (e.g., Weasel-like) or adaptable to C# WPF / WinUI 3.
 * Note: On Windows 11, explicitly disable rounded window corners via:
 *   DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_DONOTROUND;
 *   DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
 */

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
    int cornerRadiusPx;            // ALWAYS 0
    int outerBorderWidthPx;        // 1 or 2
    int innerDividerWidthPx;       // 1
    int metroLeftBarWidthPx;       // 4 for Horizontal, 3 for Vertical (Metro only, else 0)
    int hardShadowOffsetPx;        // 4 for Brutal window, else 0
    bool useMonospaceCode;         // true for Brutal
    bool collapseCandidateCells;   // true for Swiss
};

struct WinThemeSpec {
    const char* id;
    WinMetrics metrics;
    WinPalette light;
    WinPalette dark;
};

static const WinThemeSpec THEME_SWISS = {
    "swiss",
    { 0, 1, 1, 0, 0, false, true },
    /* Light */ { 0xFFFAFAF8, 0xFFF1F1ED, 0xFF181817, 0xFFC8C8C2, 0xFF141413, 0xFF767670, 0xFFEA3800, 0xFFFFFFFF, 0xFF141413, 0xFFFAFAF8, 0x00000000 },
    /* Dark  */ { 0xFF181818, 0xFF121212, 0xFF454545, 0xFF2E2E2E, 0xFFEFEFEC, 0xFF787873, 0xFFFF4D15, 0xFFFFFFFF, 0xFFEFEFEC, 0xFF121212, 0x00000000 }
};

static const WinThemeSpec THEME_METRO = {
    "metro",
    { 0, 1, 1, 4, 0, false, false },
    /* Light */ { 0xFFFFFFFF, 0xFFF4F7FB, 0xFF0067C0, 0xFFBFC8D4, 0xFF0F1722, 0xFF5C6B7E, 0xFF0067C0, 0xFFFFFFFF, 0xFFE5F1FB, 0xFF004E92, 0x00000000 },
    /* Dark  */ { 0xFF1B2029, 0xFF151921, 0xFF3AA0FF, 0xFF2F3745, 0xFFF2F6FC, 0xFF8492A6, 0xFF3AA0FF, 0xFF09111A, 0xFF192E44, 0xFF70BFFF, 0x00000000 }
};

static const WinThemeSpec THEME_BRUTAL = {
    "brutal",
    { 0, 2, 1, 0, 4, true, false },
    /* Light */ { 0xFFFFFDF8, 0xFFEFEAE0, 0xFF121212, 0xFF121212, 0xFF121212, 0xFF5E5950, 0xFFB8F500, 0xFF121212, 0xFFB8F500, 0xFF121212, 0xFF121212 },
    /* Dark  */ { 0xFF141A17, 0xFF0F1412, 0xFF00F078, 0xFF46574F, 0xFFE6F4EC, 0xFF6F877B, 0xFF00F078, 0xFF08120C, 0xFF00F078, 0xFF08120C, 0xFF000000 }
};

static const WinThemeSpec THEME_EINK = {
    "eink",
    { 0, 2, 1, 0, 0, false, false },
    /* Light */ { 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF333333, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0x00000000 },
    /* Dark  */ { 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0x00000000 }
};

} // namespace SharpIme
