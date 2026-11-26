#pragma once

#include "fangcun/InputScheme.hpp"
#include "fangcun/ThemeConfig.hpp"
#include "CustomSkinCache.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fangcun::tsf {

enum class CandidateLayout { DoubleRow, SingleRow, Vertical };

/**
 * Caret-following, non-activating floating candidate window.
 * Rendered with Direct2D + DirectWrite matching Android 1.5.1 visual tokens:
 * - 4 skins x light/dark mode
 * - Probe-measured stable header height (no layout jumping)
 * - Solid theme-accent first candidate with contrast-safe text and square corners
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

    void setCustomSkin(const std::filesystem::path& dataRoot, const std::string& id, const std::string& hash, bool dark);
    void setBuiltinStyle(const std::filesystem::path& dataRoot, const std::string& builtin, const std::string& hash, bool dark);
    void moveNearCaret(const RECT& caretRect);
    void show();
    void hide();
    bool isVisible() const;
    void setPageSize(size_t size) { pageSize_ = (size >= 5 && size <= 9) ? size : kPageSize; }
    void setLayout(CandidateLayout layout) { layout_ = layout; }
    void setWheelPaging(bool enabled) { wheelPaging_ = enabled; }
    void showModeToast(bool englishMode, SkinPreset skin, bool darkMode);

private:
    friend struct CandidateWindowTestAccess;
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
    void consumeCustomSkin();
    const LoadedBuiltinStyle* activeBuiltinStyle() const;
    void rebuildCustomBitmaps();
    void releaseCustomBitmaps();
    void drawCustomImage(skin::ImageRole role, D2D1_RECT_F bounds);

    HWND hwnd_ = nullptr;
    std::unique_ptr<CustomSkinCache> customCache_;
    std::shared_ptr<const LoadedCustomSkin> customSkin_;
    std::shared_ptr<const LoadedBuiltinStyle> builtinSkin_;
    std::array<ID2D1Bitmap*,8> customBitmaps_{};
    std::filesystem::path customDataRoot_;
    std::string customId_, customHash_;
    bool customDark_ = false;
    std::filesystem::path builtinDataRoot_;
    std::string builtinId_, builtinHash_;
    bool builtinDark_ = false;
    SelectCallback onSelect_;
    SettingsCallback onOpenSettings_;
    PageCallback onPageChange_;

    std::string composing_;
    std::string lastGlowComposing_;
    // Derived during layout updates; glow-only paints do not remeasure text.
    std::wstring visibleCode_;
    float visibleCodeWidthDp_ = 0.0f;
    std::vector<std::string> candidates_;
    std::vector<float> horizontalCellWidths_;
    std::vector<RECT> candidateRects_;
    std::vector<size_t> candidateIndices_;
    RECT prevPageRect_{0, 0, 0, 0};
    RECT nextPageRect_{0, 0, 0, 0};
    RECT settingsRect_{0, 0, 0, 0};
    int selectedIndex_ = 0;
    bool pending_ = false;
    InputScheme scheme_ = InputScheme::Flypy;
    SkinPreset skin_ = SkinPreset::SystemDefault;
    bool darkMode_ = false;
    int firstCandidateSize_ = 20;
    bool typingGlow_ = true;
    size_t pageSize_ = kPageSize;
    bool wheelPaging_ = true;
    CandidateLayout layout_ = CandidateLayout::DoubleRow;
    SkinPreset lastBudgetSkin_ = SkinPreset::SystemDefault;
    float singleCodeWidth_ = 110.0f;
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
    // Static font-family views are part of the measured budget identity. A
    // profile can become effective on the first dark-mode key after loading.
    std::array<std::wstring_view, 3> lastBudgetFonts_{};

    RECT lastCaretRect_{0, 0, 0, 0};
    HWND lastCaretFgWnd_ = nullptr;
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
