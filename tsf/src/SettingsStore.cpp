#include "SettingsStore.hpp"
#include "DllMain.hpp"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fangcun::tsf {

std::filesystem::path SettingsStore::getDllDirectory() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hInstance, path, MAX_PATH) > 0) {
        return std::filesystem::path(path).parent_path();
    }
    return std::filesystem::current_path();
}

std::filesystem::path SettingsStore::getAssetDirectory() {
    std::filesystem::path dllDir = getDllDirectory();
    if (std::filesystem::exists(dllDir / "assets" / "dict.db")) {
        return dllDir / "assets";
    }
    if (std::filesystem::exists(dllDir / "dict.db")) {
        return dllDir;
    }
    // Fallback to %LOCALAPPDATA%\FangCunIME\assets
    return getDataDirectory() / "assets";
}

std::filesystem::path SettingsStore::getDataDirectory() {
    PWSTR localAppData = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
        base = std::filesystem::path(localAppData);
        CoTaskMemFree(localAppData);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    std::filesystem::path dir = base / "FangCunIME";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool SettingsStore::isSystemDarkMode() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0,
            KEY_READ,
            &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    bool dark = false;
    if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        dark = (value == 0);
    }
    RegCloseKey(hKey);
    return dark;
}

bool SettingsStore::effectiveDarkMode(const AppSettings& settings) {
    return resolveEffectiveDarkMode(settings.themeMode, settings.skin, isSystemDarkMode());
}

AppSettings SettingsStore::load() {
    AppSettings s;
    std::filesystem::path cfg = getDataDirectory() / "settings.ini";
    std::ifstream in(cfg);
    if (!in.is_open()) {
        s.darkMode = effectiveDarkMode(s);
        return s;
    }

    bool hasThemeMode = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "scheme") {
            if (val == "pinyin") s.scheme = InputScheme::Pinyin;
            else if (val == "ninekey") s.scheme = InputScheme::NineKey;
            else s.scheme = InputScheme::Flypy;
        } else if (key == "mixed_input") {
            s.mixedInput = (val != "0");
        } else if (key == "chinese_punctuation") {
            s.chinesePunctuation = (val != "0");
        } else if (key == "skin") {
            s.skin = parseSkinPreset(val);
        } else if (key == "theme_mode") {
            hasThemeMode = true;
            if (val == "light") s.themeMode = ThemeMode::Light;
            else if (val == "dark") s.themeMode = ThemeMode::Dark;
            else s.themeMode = ThemeMode::System;
        } else if (key == "dark" || key == "dark_mode") {
            s.darkMode = (val == "1" || val == "true");
            if (!hasThemeMode) {
                s.themeMode = s.darkMode ? ThemeMode::Dark : ThemeMode::Light;
            }
        } else if (key == "first_candidate_size") {
            try {
                s.firstCandidateSize = std::clamp(std::stoi(val), 16, 30);
            } catch (...) {}
        } else if (key == "key_opacity") {
            try {
                s.keyOpacity = std::clamp(std::stof(val), 0.4f, 1.0f);
            } catch (...) {}
        } else if (key == "typing_glow") {
            s.typingGlow = (val != "0");
        } else if (key == "key_letter_size") {
            try {
                s.keyLetterSize = std::clamp(std::stoi(val), 14, 30);
            } catch (...) {}
        } else if (key == "key_letter_bold") {
            s.keyLetterBold = (val == "1");
        } else if (key == "shift_toggle_english") {
            s.shiftToggleEnglish = (val != "0");
        } else if (key == "capslock_bypass") {
            s.capsLockBypass = (val != "0");
        } else if (key == "page_size") {
            try {
                s.pageSize = std::clamp(std::stoi(val), 5, 9);
            } catch (...) {}
        } else if (key == "wheel_paging") {
            s.wheelPaging = (val != "0");
        } else if (key == "clipboard_enabled") {
            s.clipboardEnabled = (val != "0" && val != "false");
        }
    }
    s.darkMode = effectiveDarkMode(s);
    return s;
}

void SettingsStore::save(const AppSettings& s) {
    std::filesystem::path cfg = getDataDirectory() / "settings.ini";
    std::ofstream out(cfg, std::ios::trunc);
    if (!out.is_open()) return;

    const char* schemeStr =
        (s.scheme == InputScheme::Pinyin) ? "pinyin" :
        (s.scheme == InputScheme::NineKey) ? "ninekey" : "flypy";
    const char* skinStr = skinMetrics(s.skin).id;
    const char* modeStr =
        (s.themeMode == ThemeMode::Light) ? "light" :
        (s.themeMode == ThemeMode::Dark) ? "dark" : "system";
    const char* darkBit = effectiveDarkMode(s) ? "1" : "0";

    out << "scheme=" << schemeStr << "\n"
        << "mixed_input=" << (s.mixedInput ? "1" : "0") << "\n"
        << "chinese_punctuation=" << (s.chinesePunctuation ? "1" : "0") << "\n"
        << "skin=" << skinStr << "\n"
        << "theme_mode=" << modeStr << "\n"
        << "dark=" << darkBit << "\n"
        << "dark_mode=" << darkBit << "\n"
        << "first_candidate_size=" << std::clamp(s.firstCandidateSize, 16, 30) << "\n"
        << "key_opacity=" << std::clamp(s.keyOpacity, 0.4f, 1.0f) << "\n"
        << "typing_glow=" << (s.typingGlow ? "1" : "0") << "\n"
        << "key_letter_size=" << std::clamp(s.keyLetterSize, 14, 30) << "\n"
        << "key_letter_bold=" << (s.keyLetterBold ? "1" : "0") << "\n"
        << "shift_toggle_english=" << (s.shiftToggleEnglish ? "1" : "0") << "\n"
        << "capslock_bypass=" << (s.capsLockBypass ? "1" : "0") << "\n"
        << "page_size=" << std::clamp(s.pageSize, 5, 9) << "\n"
        << "wheel_paging=" << (s.wheelPaging ? "1" : "0") << "\n"
        << "clipboard_enabled=" << (s.clipboardEnabled ? "1" : "0") << "\n";
}

} // namespace fangcun::tsf
