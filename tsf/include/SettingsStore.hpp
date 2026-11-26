#pragma once

#include "fangcun/InputScheme.hpp"
#include "fangcun/ThemeConfig.hpp"
#include "fangcun/SettingsIni.hpp"

#include <filesystem>
#include <string>

namespace fangcun::tsf {

struct AppSettings {
    InputScheme scheme = InputScheme::Flypy;
    bool mixedInput = true;
    bool chinesePunctuation = true;
    SkinPreset skin = SkinPreset::SystemDefault;
    std::string customSkinId;
    std::string customSkinHash;
    // Read-only reference owned by the builtin editor, not fallback saves.
    std::string builtinStyleHash;
    ThemeMode themeMode = ThemeMode::System;
    bool darkMode = false; // legacy / explicit override when themeMode != System
    int firstCandidateSize = 20;
    float keyOpacity = 0.82f;
    bool typingGlow = true;
    int keyLetterSize = 18;
    bool keyLetterBold = false;
    bool shiftToggleEnglish = true;
    bool capsLockBypass = true;
    int pageSize = 7;
    bool wheelPaging = true;
    std::string candidateLayout = "double";
    bool fullWidth = false;
    bool traditional = false;
    bool clipboardEnabled = true; // Preserve the companion listener preference on fallback edits.
    // Loaded snapshot metadata stays in memory; never serialized as preferences.
    fangcun::settings::Values savedValues;
    bool readSucceeded = true;
};

class SettingsStore {
public:
    static std::filesystem::path getDllDirectory();
    static std::filesystem::path getAssetDirectory();
    static std::filesystem::path getDataDirectory();
    static bool isSystemDarkMode();
    static bool effectiveDarkMode(const AppSettings& settings);
    static AppSettings load();
    static bool save(AppSettings& settings);
    static AppSettings confirmed(const AppSettings& settings);
};

} // namespace fangcun::tsf
