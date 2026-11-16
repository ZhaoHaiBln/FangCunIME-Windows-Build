#pragma once

#include "fangcun/InputScheme.hpp"
#include "fangcun/ThemeConfig.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace fangcun::tsf {

/**
 * Caret-following, non-activating floating candidate window.
 * Rendered with Direct2D + DirectWrite matching Android 1.5.1 visual tokens:
 * - 4 skins x light/dark mode
 * - Probe-measured stable header height (no layout jumping)
 * - First candidate accent text + 3.5% alpha rounded highlight (4dp corner)
 * - Tianzige brand mark + skin-matched TypingGlow behind header
 */
class CandidateWindow {
public:
    static constexpr size_t kPageSize = 9;

    using SelectCallback = std::function<void(size_t index)>;
    using SettingsCallback = std::function<void()>;
    using PageCallback = std::function<void(int delta)>;

    explicit CandidateWindow(
        SelectCallback onSelect,
        SettingsCallback onOpenSettings = nullptr,
        PageCallback onPageChange = nullptr);
    ~CandidateWindow();

    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;

    bool create(HWND parentOwner);
    void destroy();

    void update(
        const std::string& composing,
        const std::vector<std::string>& candidates,
        int selectedIndex,
        bool pending,
        InputScheme scheme,
        SkinPreset skin,
        bool darkMode,
        int firstCandidateSize,
        bool typingGlow = true);

    void moveNearCaret(const RECT& caretRect);
    void show();
    void hide();
    bool isVisible() const;
    void setPageSize(size_t size) { pageSize_ = (size >= 5 && size <= 9) ? size : kPageSize; }
    void setWheelPaging(bool enabled) { wheelPaging_ = enabled; }
    void showModeToast(bool englishMode, SkinPreset skin, bool darkMode);

private:
    static constexpr UINT_PTR kGlowTimerId = 1;
    static constexpr UINT kGlowTimerIntervalMs = 16;
    static constexpr UINT_PTR kToastTimerId = 2;
    static constexpr UINT kToastDurationMs = 900;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void onPaint();
    void onLButtonDown(int x, int y);
    void onGlowTimer();
    void syncGlowTimer();
    int scaleForDpi(int px) const;
    float dpToPx(float dp) const;
    void recomputeHeaderBudget();
    void recalculateSize();
    void updateWindowRegion();
    void computePageBounds(size_t& pageStart, size_t& pageEnd, size_t& currentPage, size_t& totalPages) const;

    bool ensureFactories();
    void releaseFactories();
    bool ensureDeviceResources();
    void releaseDeviceResources();
    void drawTianzigeLogoD2D(float x, float y, float size);

    HWND hwnd_ = nullptr;
    SelectCallback onSelect_;
    SettingsCallback onOpenSettings_;
    PageCallback onPageChange_;

    std::string composing_;
    std::string lastGlowComposing_;
    std::vector<std::string> candidates_;
    std::vector<RECT> candidateRects_;
    std::vector<size_t> candidateIndices_;
    RECT prevPageRect_{0, 0, 0, 0};
    RECT nextPageRect_{0, 0, 0, 0};
    RECT settingsRect_{0, 0, 0, 0};
    int selectedIndex_ = 0;
    bool pending_ = false;
    InputScheme scheme_ = InputScheme::Flypy;
    SkinPreset skin_ = SkinPreset::NativeLight;
    bool darkMode_ = false;
    int firstCandidateSize_ = 20;
    bool typingGlow_ = true;
    size_t pageSize_ = kPageSize;
    bool wheelPaging_ = true;
    bool toastActive_ = false;
    bool toastEnglish_ = false;

    UINT dpi_ = 96;
    int width_ = 360;
    int height_ = 68;
    float headerHeightDp_ = 64.0f;
    float codeHeightDp_ = 19.0f;
    float candidateHeightDp_ = 40.0f;
    int lastBudgetFirstSize_ = -1;
    UINT lastBudgetDpi_ = 0;

    RECT lastCaretRect_{0, 0, 0, 0};
    bool hasCaretRect_ = false;

    TypingGlowEngine glowEngine_;
    bool glowTimerActive_ = false;

    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;
    IDWriteTextFormat* codeFormat_ = nullptr;
    IDWriteTextFormat* primaryFormat_ = nullptr;
    IDWriteTextFormat* secondaryFormat_ = nullptr;
    IDWriteTextFormat* indexFormat_ = nullptr;
};

} // namespace fangcun::tsf
