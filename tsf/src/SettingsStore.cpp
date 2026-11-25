#include "SettingsStore.hpp"
#include "DllMain.hpp"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <locale>
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

namespace {
constexpr std::size_t kMaximumSettingsBytes = 64 * 1024;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct WriteLock {
    Handle file;
    OVERLAPPED overlap{};
    bool held = false;
    explicit WriteLock(const std::filesystem::path& path)
        : file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr)) {
        // Native settings writes can run on the input thread: never sleep here.
        held = file.value != INVALID_HANDLE_VALUE && LockFileEx(file.value,
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap);
    }
    ~WriteLock() { if (held) UnlockFileEx(file.value, 0, 1, 0, &overlap); }
};

enum class ReadResult { Missing, Success, Failure };
ReadResult readSnapshot(const std::filesystem::path& path, std::string& content) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ReadResult::Missing : ReadResult::Failure;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 || size.QuadPart > kMaximumSettingsBytes) return ReadResult::Failure;
    content.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < content.size()) {
        DWORD read = 0;
        if (!ReadFile(file.value, content.data() + offset, static_cast<DWORD>(content.size() - offset), &read, nullptr) || read == 0)
            return ReadResult::Failure;
        offset += read;
    }
    char extra = 0;
    DWORD read = 0;
    if (!ReadFile(file.value, &extra, 1, &read, nullptr) || read != 0 || content.find('\0') != std::string::npos) return ReadResult::Failure;
    if (!content.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), static_cast<int>(content.size()), nullptr, 0) == 0)
        return ReadResult::Failure;
    return ReadResult::Success;
}

fangcun::settings::Values knownValues(const AppSettings& s) {
    const auto bit = [](bool value) { return value ? "1" : "0"; };
    const char* scheme = s.scheme == InputScheme::Pinyin ? "pinyin" : s.scheme == InputScheme::NineKey ? "ninekey" : "flypy";
    const char* mode = s.themeMode == ThemeMode::Light ? "light" : s.themeMode == ThemeMode::Dark ? "dark" : "system";
    std::ostringstream opacity;
    opacity.imbue(std::locale::classic());
    opacity << std::fixed << std::setprecision(2) << std::clamp(s.keyOpacity, .4f, 1.f);
    return {
        {"scheme", scheme}, {"mixed_input", bit(s.mixedInput)}, {"chinese_punctuation", bit(s.chinesePunctuation)},
        {"skin", skinMetrics(s.skin).id}, {"theme_mode", mode},
        {"dark", bit(SettingsStore::effectiveDarkMode(s))}, {"dark_mode", bit(SettingsStore::effectiveDarkMode(s))},
        {"first_candidate_size", std::to_string(std::clamp(s.firstCandidateSize, 16, 30))}, {"key_opacity", opacity.str()},
        {"typing_glow", bit(s.typingGlow)}, {"key_letter_size", std::to_string(std::clamp(s.keyLetterSize, 14, 30))},
        {"key_letter_bold", bit(s.keyLetterBold)}, {"shift_toggle_english", bit(s.shiftToggleEnglish)},
        {"capslock_bypass", bit(s.capsLockBypass)}, {"page_size", std::to_string(std::clamp(s.pageSize, 5, 9))},
        {"wheel_paging", bit(s.wheelPaging)}, {"candidate_layout", s.candidateLayout}, {"full_width", bit(s.fullWidth)},
        {"traditional", bit(s.traditional)}, {"clipboard_enabled", bit(s.clipboardEnabled)}
    };
}

AppSettings fromValues(const fangcun::settings::Values& values) {
    AppSettings s;
    bool hasThemeMode = values.contains("theme_mode");
    const auto boolean = [](const std::string& value, bool fallback) {
        if (value == "1" || value == "true" || value == "TRUE") return true;
        if (value == "0" || value == "false" || value == "FALSE") return false;
        return fallback;
    };
    for (const auto& [key, val] : values) {
        try {
            if (key == "scheme") s.scheme = val == "pinyin" ? InputScheme::Pinyin : val == "ninekey" ? InputScheme::NineKey : InputScheme::Flypy;
            else if (key == "mixed_input") s.mixedInput = boolean(val, s.mixedInput);
            else if (key == "chinese_punctuation") s.chinesePunctuation = boolean(val, s.chinesePunctuation);
            else if (key == "skin") s.skin = parseSkinPreset(val);
            else if (key == "theme_mode") s.themeMode = val == "light" ? ThemeMode::Light : val == "dark" ? ThemeMode::Dark : ThemeMode::System;
            else if (!hasThemeMode && (key == "dark" || key == "dark_mode")) s.themeMode = boolean(val, false) ? ThemeMode::Dark : ThemeMode::Light;
            else if (key == "first_candidate_size") s.firstCandidateSize = std::clamp(std::stoi(val), 16, 30);
            else if (key == "key_opacity") { const auto value = std::stof(val); if (std::isfinite(value)) s.keyOpacity = std::clamp(value, .4f, 1.f); }
            else if (key == "typing_glow") s.typingGlow = boolean(val, s.typingGlow);
            else if (key == "key_letter_size") s.keyLetterSize = std::clamp(std::stoi(val), 14, 30);
            else if (key == "key_letter_bold") s.keyLetterBold = boolean(val, s.keyLetterBold);
            else if (key == "shift_toggle_english") s.shiftToggleEnglish = boolean(val, s.shiftToggleEnglish);
            else if (key == "capslock_bypass") s.capsLockBypass = boolean(val, s.capsLockBypass);
            else if (key == "page_size") s.pageSize = std::clamp(std::stoi(val), 5, 9);
            else if (key == "wheel_paging") s.wheelPaging = boolean(val, s.wheelPaging);
            else if (key == "candidate_layout" && (val == "double" || val == "single" || val == "vertical")) s.candidateLayout = val;
            else if (key == "full_width") s.fullWidth = boolean(val, s.fullWidth);
            else if (key == "traditional") s.traditional = boolean(val, s.traditional);
            else if (key == "clipboard_enabled") s.clipboardEnabled = boolean(val, s.clipboardEnabled);
        } catch (...) { /* Keep the bounded legacy default for an invalid known value. */ }
    }
    s.darkMode = SettingsStore::effectiveDarkMode(s);
    s.savedValues = knownValues(s);
    return s;
}

bool replaceAtomically(const std::filesystem::path& path, const std::string& content) {
    if (content.size() > kMaximumSettingsBytes) return false;
    static std::atomic<unsigned long long> next{0};
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(next.fetch_add(1));
    bool result = false;
    {
        Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) return false;
        std::size_t offset = 0;
        while (offset < content.size()) {
            DWORD written = 0;
            if (!WriteFile(file.value, content.data() + offset, static_cast<DWORD>(content.size() - offset), &written, nullptr) || written == 0) break;
            offset += written;
        }
        result = offset == content.size() && FlushFileBuffers(file.value);
    }
    if (result) result = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!result) DeleteFileW(temporary.c_str());
    return result;
}
} // namespace

AppSettings SettingsStore::load() {
    std::string content;
    const auto result = readSnapshot(getDataDirectory() / "settings.ini", content);
    auto settings = fromValues(fangcun::settings::parse(result == ReadResult::Failure ? std::string{} : content));
    settings.readSucceeded = result != ReadResult::Failure;
    return settings;
}

AppSettings SettingsStore::confirmed(const AppSettings& settings) {
    auto original = fromValues(settings.savedValues);
    original.readSucceeded = settings.readSucceeded;
    return original;
}

bool SettingsStore::save(AppSettings& desired) {
    if (!desired.readSucceeded || !std::isfinite(desired.keyOpacity)) return false;
    const auto path = getDataDirectory() / "settings.ini";
    auto lockPath = path;
    lockPath += L".lock";
    WriteLock lock(lockPath);
    if (!lock.held) return false;
    std::string original;
    const auto status = readSnapshot(path, original);
    if (status == ReadResult::Failure) return false;
    try {
        const auto target = knownValues(desired);
        auto updates = desired.savedValues.empty() ? target : fangcun::settings::changed(desired.savedValues, target);
        updates.erase("dark"); updates.erase("dark_mode");
        auto merged = fromValues(fangcun::settings::parse(fangcun::settings::merge(original, updates)));
        const auto actual = knownValues(merged);
        updates["theme_mode"] = actual.at("theme_mode");
        updates["dark"] = actual.at("dark"); updates["dark_mode"] = actual.at("dark_mode");
        const auto body = fangcun::settings::merge(original, updates);
        if (!replaceAtomically(path, body)) return false;
        desired = fromValues(fangcun::settings::parse(body));
        return true;
    } catch (...) { return false; }
}

} // namespace fangcun::tsf
