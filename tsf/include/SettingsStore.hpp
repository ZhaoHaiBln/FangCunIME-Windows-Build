#pragma once

#include "fangcun/InputScheme.hpp"
#include "fangcun/ThemeConfig.hpp"

#include <filesystem>
#include <string>

namespace fangcun::tsf {

struct AppSettings {
    InputScheme scheme = InputScheme::Flypy;
    bool mixedInput = true;
    bool chinesePunctuation = true;
    SkinPreset skin = SkinPreset::WarmGrid;
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
    CandidateLayout candidateLayout = CandidateLayout::Horizontal;
};

class SettingsStore {
public:
    static std::filesystem::path getDllDirectory();
    static std::filesystem::path getAssetDirectory();
    static std::filesystem::path getDataDirectory();
    static bool isSystemDarkMode();
    static bool effectiveDarkMode(const AppSettings& settings);
    static AppSettings load();
    static void save(const AppSettings& settings);
};

} // namespace fangcun::tsf
