#pragma once

#include <windows.h>

namespace fangcun::tsf {

// Resolve at runtime so Windows 10 remains supported. Older DWM versions simply
// ignore the Windows 11 corner preference. This applies only to our own HWNDs.
inline void preferSquareWindow(HWND window) {
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const auto setAttribute = []() -> SetAttribute {
        HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<SetAttribute>(GetProcAddress(module, "DwmSetWindowAttribute")) : nullptr;
    }();
    if (window && setAttribute) {
        constexpr DWORD cornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
        constexpr DWORD doNotRound = 1;        // DWMWCP_DONOTROUND
        setAttribute(window, cornerPreference, &doNotRound, sizeof(doNotRound));
    }
}

} // namespace fangcun::tsf
