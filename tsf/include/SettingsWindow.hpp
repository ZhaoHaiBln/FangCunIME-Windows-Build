#pragma once

#include "SettingsStore.hpp"
#include "fangcun/ThemeConfig.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace fangcun::tsf {

/**
 * Native Direct2D + DirectWrite Settings Window for FangCun IME.
 * Shares the exact ThemeConfig.hpp palette, 3D glass keycap surfaces,
 * Tianzige brand mark, and TypingGlow with CandidateWindow.
 */
class SettingsWindow {
public:
    using ChangeCallback = std::function<void(const AppSettings&)>;

    explicit SettingsWindow(ChangeCallback onChanged = nullptr);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    void showOrActivate(HWND owner = nullptr);
    void destroy();
    bool isOpen() const;

    static UINT settingsChangedMessage();
    static void broadcastSettingsChanged();

private:
    enum class CategoryTab {
        All = 0,
        Input,
        Theme,
        Typography,
    };

    enum class ActionId {
        None = 0,
        TabAll,
        TabInput,
        TabTheme,
        TabTypography,
        SchemeFlypy,
        SchemePinyin,
        SchemeNineKey,
        ToggleMixedInput,
        ToggleChinesePunct,
        SkinNativeLight,
        SkinWarmGrid,
        SkinMint,
        SkinDeepBlue,
        ModeSystem,
        ModeLight,
        ModeDark,
        ToggleTypingGlow,
        ToggleKeyBold,
        FirstSizeMinus,
        FirstSizePlus,
        LetterSizeMinus,
        LetterSizePlus,
        OpacityMinus,
        OpacityPlus,
        PreviewGlowTrigger,
        ResetDefaults,
        CloseWindow,
    };

    struct HitControl {
        D2D1_RECT_F rectDip{};
        ActionId action = ActionId::None;
    };

    static constexpr UINT_PTR kGlowTimerId = 2;
    static constexpr UINT kGlowTimerIntervalMs = 16;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void onPaint();
    void onLButtonDown(int x, int y);
    void onMouseMove(int x, int y);
    void onGlowTimer();
    void syncGlowTimer();
    void applyAction(ActionId action);
    void saveAndNotify();

    float dpToPx(float dp) const;
    bool ensureFactories();
    void releaseFactories();
    bool ensureDeviceResources();
    void releaseDeviceResources();
    void createTianzigeIcons();
    void destroyTianzigeIcons();

    void drawTianzigeLogo(float x, float y, float size);
    void drawGlassButton(
        const D2D1_RECT_F& rect,
        const std::wstring& label,
        SkinPreset skin,
        const FangCunColors& colors,
        bool selected,
        bool functional,
        bool emphasized,
        bool hovered,
        float fontSizeDip = 13.0f,
        bool bold = false);

    HWND hwnd_ = nullptr;
    ChangeCallback onChanged_;
    AppSettings settings_;
    CategoryTab activeTab_ = CategoryTab::All;
    UINT dpi_ = 96;
    int width_ = 820;
    int height_ = 650;

    HICON smallIcon_ = nullptr;
    HICON bigIcon_ = nullptr;

    std::vector<HitControl> hitControls_;
    ActionId hoveredAction_ = ActionId::None;

    TypingGlowEngine glowEngine_;
    bool glowTimerActive_ = false;

    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* sectionFormat_ = nullptr;
    IDWriteTextFormat* bodyFormat_ = nullptr;
    IDWriteTextFormat* smallFormat_ = nullptr;
    IDWriteTextFormat* monoFormat_ = nullptr;
};

} // namespace fangcun::tsf
