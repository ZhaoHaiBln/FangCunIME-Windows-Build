#include "CandidateWindow.hpp"
#include "DllMain.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fangcun::tsf {

namespace {

constexpr wchar_t kCandidateWndClass[] = L"FangCunCandidateWindowClass";
constexpr wchar_t kUiFontFamily[] = L"Microsoft YaHei UI";

template <typename T>
void safeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

D2D1_COLOR_F toD2DColor(ColorArgb c) {
    return D2D1::ColorF(
        static_cast<FLOAT>(c.r) / 255.0f,
        static_cast<FLOAT>(c.g) / 255.0f,
        static_cast<FLOAT>(c.b) / 255.0f,
        static_cast<FLOAT>(c.a) / 255.0f);
}

UINT queryDpiForWindow(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d > 0) return d;
    }
    HDC hdc = GetDC(hwnd);
    UINT d = hdc ? static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX)) : 96;
    if (hdc) ReleaseDC(hwnd, hdc);
    return d > 0 ? d : 96;
}

bool isClientAreaAnimationEnabled() {
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
        return enabled != FALSE;
    }
    return true;
}

int64_t monotonicNowNanos() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

float measureTextWidthDip(IDWriteFactory* factory, IDWriteTextFormat* format, const std::wstring& text) {
    if (!factory || !format || text.empty()) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            4096.0f,
            512.0f,
            &layout)) || !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics = {};
    float w = 0.0f;
    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
        w = metrics.widthIncludingTrailingWhitespace;
    }
    layout->Release();
    return w;
}

float measureTextHeightDip(IDWriteFactory* factory, IDWriteTextFormat* format, const std::wstring& text) {
    if (!factory || !format || text.empty()) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            4096.0f,
            512.0f,
            &layout)) || !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics = {};
    float h = 0.0f;
    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
        h = metrics.height;
    }
    layout->Release();
    return h;
}

} // namespace

CandidateWindow::CandidateWindow(
    SelectCallback onSelect,
    SettingsCallback onOpenSettings,
    PageCallback onPageChange)
    : onSelect_(std::move(onSelect)),
      onOpenSettings_(std::move(onOpenSettings)),
      onPageChange_(std::move(onPageChange)) {}

CandidateWindow::~CandidateWindow() {
    destroy();
}

bool CandidateWindow::ensureFactories() {
    if (!d2dFactory_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) {
            return false;
        }
    }
    if (!dwriteFactory_) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&dwriteFactory_)))) {
            return false;
        }
    }
    return true;
}

void CandidateWindow::releaseFactories() {
    safeRelease(codeFormat_);
    safeRelease(primaryFormat_);
    safeRelease(secondaryFormat_);
    safeRelease(indexFormat_);
    releaseDeviceResources();
    safeRelease(dwriteFactory_);
    safeRelease(d2dFactory_);
}

bool CandidateWindow::ensureDeviceResources() {
    if (!hwnd_ || !ensureFactories()) return false;
    if (renderTarget_) return true;

    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(std::max(1, width_)),
        static_cast<UINT32>(std::max(1, height_)));

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = static_cast<FLOAT>(dpi_);
    rtProps.dpiY = static_cast<FLOAT>(dpi_);

    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(hwnd_, size),
        &renderTarget_);
    if (SUCCEEDED(hr) && renderTarget_) {
        renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        return true;
    }
    return false;
}

void CandidateWindow::releaseDeviceResources() {
    safeRelease(renderTarget_);
}

bool CandidateWindow::create(HWND parentOwner) {
    if (hwnd_) return true;
    ensureFactories();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = &CandidateWindow::wndProc;
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kCandidateWndClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kCandidateWndClass,
        L"",
        WS_POPUP,
        0, 0, width_, height_,
        parentOwner,
        nullptr,
        g_hInstance,
        this);

    if (!hwnd_) return false;
    dpi_ = queryDpiForWindow(hwnd_);
    recomputeHeaderBudget();
    recalculateSize();
    return true;
}

void CandidateWindow::destroy() {
    if (hwnd_) {
        if (glowTimerActive_) {
            KillTimer(hwnd_, kGlowTimerId);
            glowTimerActive_ = false;
        }
        glowEngine_.clear();
        releaseFactories();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    } else {
        releaseFactories();
    }
}

int CandidateWindow::scaleForDpi(int px) const {
    return MulDiv(px, static_cast<int>(dpi_), 96);
}

float CandidateWindow::dpToPx(float dp) const {
    return dp * static_cast<float>(dpi_) / 96.0f;
}

void CandidateWindow::update(
    const std::string& composing,
    const std::vector<std::string>& candidates,
    int selectedIndex,
    bool pending,
    InputScheme scheme,
    SkinPreset skin,
    bool darkMode,
    int firstCandidateSize,
    bool typingGlow) {
    if (toastActive_) {
        toastActive_ = false;
        if (hwnd_) KillTimer(hwnd_, kToastTimerId);
    }
    bool composingChanged = (composing != lastGlowComposing_);
    lastGlowComposing_ = composing;

    composing_ = composing;
    candidates_ = candidates;
    selectedIndex_ = selectedIndex;
    pending_ = pending;
    scheme_ = scheme;
    skin_ = skin;
    darkMode_ = darkMode;
    firstCandidateSize_ = std::clamp(firstCandidateSize, HeaderMetricsSpec::kFirstCandidateMinSp, HeaderMetricsSpec::kFirstCandidateMaxSp);
    typingGlow_ = typingGlow;

    glowEngine_.setEnabled(typingGlow_);
    glowEngine_.setMotionAllowed(isClientAreaAnimationEnabled());

    if (!hwnd_) return;
    dpi_ = queryDpiForWindow(hwnd_);
    recomputeHeaderBudget();
    recalculateSize();

    if (composingChanged && !composing_.empty()) {
        glowEngine_.trigger(false, 0.0f, 0.0f);
        glowEngine_.advanceFrame(monotonicNowNanos());
    } else if (composing_.empty()) {
        glowEngine_.clear();
    }
    syncGlowTimer();

    if (hasCaretRect_) {
        moveNearCaret(lastCaretRect_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandidateWindow::showModeToast(bool englishMode, SkinPreset skin, bool darkMode) {
    if (!hwnd_) return;
    composing_.clear();
    candidates_.clear();
    toastActive_ = true;
    toastEnglish_ = englishMode;
    skin_ = skin;
    darkMode_ = darkMode;

    dpi_ = queryDpiForWindow(hwnd_);
    recomputeHeaderBudget();
    recalculateSize();

    if (!hasCaretRect_) {
        POINT pt{};
        if (GetCursorPos(&pt)) {
            lastCaretRect_ = RECT{pt.x, pt.y - 20, pt.x + 2, pt.y};
            hasCaretRect_ = true;
        }
    }
    if (hasCaretRect_) {
        moveNearCaret(lastCaretRect_);
    }
    show();
    SetTimer(hwnd_, kToastTimerId, kToastDurationMs, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

namespace {

bool isCraftedSkin(SkinPreset skin) {
    return skin == SkinPreset::GreenTerminal ||
           skin == SkinPreset::WoodType ||
           skin == SkinPreset::ClassicTypewriter;
}

bool showsCandidateIndex(SkinPreset skin) {
    return skin != SkinPreset::WoodType && skin != SkinPreset::ClassicTypewriter;
}

float skinFrameInsetDp(SkinPreset skin) {
    switch (skin) {
        case SkinPreset::GreenTerminal: return 6.0f;
        case SkinPreset::WoodType: return 6.0f;
        case SkinPreset::ClassicTypewriter: return 5.5f;
        default: return 0.0f;
    }
}

float skinTopChromeExtraDp(SkinPreset skin) {
    return (skin == SkinPreset::ClassicTypewriter) ? 10.0f : 0.0f;
}

float skinOuterCornerDp(SkinPreset skin) {
    switch (skin) {
        case SkinPreset::GreenTerminal: return 8.0f;
        case SkinPreset::WoodType: return 7.0f;
        case SkinPreset::ClassicTypewriter: return 9.0f;
        default: return 8.0f;
    }
}

} // namespace

void CandidateWindow::recomputeHeaderBudget() {
    if (!ensureFactories()) return;
    if (lastBudgetFirstSize_ == firstCandidateSize_ &&
        lastBudgetDpi_ == dpi_ &&
        lastBudgetSkin_ == skin_ &&
        primaryFormat_) {
        return;
    }

    safeRelease(codeFormat_);
    safeRelease(primaryFormat_);
    safeRelease(secondaryFormat_);
    safeRelease(indexFormat_);

    auto makeFormat = [this](const wchar_t* family, float sizeDip, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                family,
                nullptr,
                weight,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                sizeDip,
                L"",
                out)) && *out) {
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };

    if (skin_ == SkinPreset::GreenTerminal) {
        makeFormat(L"Consolas", 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &codeFormat_);
        makeFormat(kUiFontFamily, static_cast<float>(std::clamp(firstCandidateSize_ - 2, 15, 26)), DWRITE_FONT_WEIGHT_MEDIUM, &primaryFormat_);
        makeFormat(kUiFontFamily, static_cast<float>(std::clamp(firstCandidateSize_ - 3, 14, 24)), DWRITE_FONT_WEIGHT_NORMAL, &secondaryFormat_);
        makeFormat(L"Consolas", static_cast<float>(std::clamp(firstCandidateSize_ - 4, 13, 22)), DWRITE_FONT_WEIGHT_MEDIUM, &indexFormat_);
    } else if (skin_ == SkinPreset::WoodType) {
        makeFormat(L"Georgia", 14.0f, DWRITE_FONT_WEIGHT_NORMAL, &codeFormat_);
        makeFormat(L"SimSun", static_cast<float>(std::clamp(firstCandidateSize_ - 1, 16, 28)), DWRITE_FONT_WEIGHT_NORMAL, &primaryFormat_);
        makeFormat(L"SimSun", static_cast<float>(std::clamp(firstCandidateSize_ - 1, 16, 28)), DWRITE_FONT_WEIGHT_NORMAL, &secondaryFormat_);
        makeFormat(L"Georgia", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, &indexFormat_);
    } else if (skin_ == SkinPreset::ClassicTypewriter) {
        makeFormat(L"Courier New", 14.5f, DWRITE_FONT_WEIGHT_BOLD, &codeFormat_);
        makeFormat(L"SimSun", static_cast<float>(std::clamp(firstCandidateSize_ - 1, 16, 28)), DWRITE_FONT_WEIGHT_NORMAL, &primaryFormat_);
        makeFormat(L"SimSun", static_cast<float>(std::clamp(firstCandidateSize_ - 1, 16, 28)), DWRITE_FONT_WEIGHT_NORMAL, &secondaryFormat_);
        makeFormat(L"Courier New", 11.0f, DWRITE_FONT_WEIGHT_BOLD, &indexFormat_);
    } else {
        makeFormat(kUiFontFamily, static_cast<float>(HeaderMetricsSpec::kCodeSp + 1), DWRITE_FONT_WEIGHT_MEDIUM, &codeFormat_);
        makeFormat(kUiFontFamily, static_cast<float>(firstCandidateSize_), DWRITE_FONT_WEIGHT_MEDIUM, &primaryFormat_);
        makeFormat(kUiFontFamily, static_cast<float>(HeaderMetricsSpec::kOtherCandidateSp), DWRITE_FONT_WEIGHT_NORMAL, &secondaryFormat_);
        makeFormat(kUiFontFamily, static_cast<float>(HeaderMetricsSpec::kCandidateIndexSp), DWRITE_FONT_WEIGHT_NORMAL, &indexFormat_);
    }

    std::wstring probe = utf8ToWide(HeaderMetricsSpec::kMeasurementProbe);
    float hPrimary = measureTextHeightDip(dwriteFactory_, primaryFormat_, probe);
    float hSecondary = measureTextHeightDip(dwriteFactory_, secondaryFormat_, probe);
    float hIndex = measureTextHeightDip(dwriteFactory_, indexFormat_, L"123456789");
    float hCode = measureTextHeightDip(dwriteFactory_, codeFormat_, L"> Agjy0123456789'");

    candidateHeightDp_ = std::max(
        isCraftedSkin(skin_) ? 34.0f : HeaderMetricsSpec::kCandidateSlotMinimumDp,
        std::max({hPrimary, hSecondary, hIndex}) + 10.0f);
    codeHeightDp_ = std::max(22.0f, hCode + 6.0f);
    headerHeightDp_ = std::max(
        HeaderMetricsSpec::kHeaderMinimumDp,
        codeHeightDp_ + candidateHeightDp_ + 8.0f);

    lastBudgetFirstSize_ = firstCandidateSize_;
    lastBudgetDpi_ = dpi_;
    lastBudgetSkin_ = skin_;
}

void CandidateWindow::computePageBounds(
    size_t& pageStart,
    size_t& pageEnd,
    size_t& currentPage,
    size_t& totalPages) const {
    size_t totalCount = candidates_.size();
    if (totalCount == 0) {
        pageStart = 0;
        pageEnd = 0;
        currentPage = 0;
        totalPages = 0;
        return;
    }
    size_t ps = (pageSize_ >= 5 && pageSize_ <= 9) ? pageSize_ : kPageSize;
    totalPages = (totalCount + ps - 1) / ps;
    size_t safeSelected = (selectedIndex_ >= 0 && static_cast<size_t>(selectedIndex_) < totalCount)
        ? static_cast<size_t>(selectedIndex_)
        : 0;
    currentPage = safeSelected / ps;
    pageStart = currentPage * ps;
    pageEnd = std::min(totalCount, pageStart + ps);
}

void CandidateWindow::recalculateSize() {
    if (!hwnd_) return;
    ensureFactories();

    if (toastActive_ && composing_.empty()) {
        int newW = static_cast<int>(std::ceil(dpToPx(92.0f)));
        int newH = static_cast<int>(std::ceil(dpToPx(40.0f)));
        if (newW != width_ || newH != height_) {
            width_ = newW;
            height_ = newH;
            SetWindowPos(hwnd_, nullptr, 0, 0, width_, height_,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (renderTarget_) {
                renderTarget_->SetDpi(static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));
                renderTarget_->Resize(D2D1::SizeU(
                    static_cast<UINT32>(std::max(1, width_)),
                    static_cast<UINT32>(std::max(1, height_))));
            }
            updateWindowRegion();
        }
        return;
    }

    const bool crafted = isCraftedSkin(skin_);
    const bool showIdx = showsCandidateIndex(skin_);
    const float frameInset = skinFrameInsetDp(skin_);
    const float topExtra = skinTopChromeExtraDp(skin_);
    const float padXDp = crafted ? 12.0f : 12.0f;
    const float padYDp = crafted ? 6.0f : 5.0f;
    const float itemGapDp = crafted ? 10.0f : 6.0f;
    const float itemPadX = crafted ? 10.0f : HeaderMetricsSpec::kItemHorizontalPaddingDp;

    size_t pageStart = 0;
    size_t pageEnd = 0;
    size_t currentPage = 0;
    size_t totalPages = 0;
    computePageBounds(pageStart, pageEnd, currentPage, totalPages);

    std::wstring compDisplay = crafted ? (L"> " + utf8ToWide(composing_)) : utf8ToWide(composing_);
    float codeW = measureTextWidthDip(dwriteFactory_, codeFormat_, compDisplay);
    float topRightReserve = (skin_ == SkinPreset::GreenTerminal) ? 56.0f : (crafted ? 24.0f : 96.0f);
    float topLeftReserve = crafted ? 0.0f : 24.0f;
    float topRowWDp = frameInset * 2.0f + padXDp * 2.0f + topLeftReserve + codeW + topRightReserve;

    float contentWDp = frameInset * 2.0f + padXDp * 2.0f;
    float totalHDp = frameInset * 2.0f + topExtra + headerHeightDp_ + padYDp * 2.0f;

    if (layout_ == CandidateLayout::Vertical) {
        float maxRowW = 0.0f;
        for (size_t idx = pageStart; idx < pageEnd; ++idx) {
            size_t localIdx = idx - pageStart;
            std::wstring candStr = utf8ToWide(candidates_[idx]);
            float idxW = showIdx ? (measureTextWidthDip(dwriteFactory_, indexFormat_, std::to_wstring(localIdx + 1)) + 7.0f) : 0.0f;
            float txtW = measureTextWidthDip(
                dwriteFactory_,
                (localIdx == 0) ? primaryFormat_ : secondaryFormat_,
                candStr);
            float rowW = itemPadX * 2.0f + idxW + txtW + 18.0f;
            maxRowW = std::max(maxRowW, rowW);
        }
        contentWDp = std::max({196.0f, topRowWDp, frameInset * 2.0f + padXDp * 2.0f + maxRowW});
        size_t rowCount = (pageEnd > pageStart) ? (pageEnd - pageStart) : 0;
        float rowH = std::max(30.0f, candidateHeightDp_ - 4.0f);
        float pagerH = (totalPages > 1) ? 22.0f : 0.0f;
        totalHDp = frameInset * 2.0f + topExtra + padYDp * 2.0f + codeHeightDp_ + 8.0f +
                   static_cast<float>(rowCount) * rowH + pagerH;
        contentWDp = std::clamp(contentWDp, 196.0f, 360.0f);
    } else {
        if (pageEnd <= pageStart) {
            contentWDp = std::max(220.0f, topRowWDp);
        } else {
            for (size_t idx = pageStart; idx < pageEnd; ++idx) {
                size_t localIdx = idx - pageStart;
                std::wstring candStr = utf8ToWide(candidates_[idx]);
                float idxW = showIdx ? (measureTextWidthDip(dwriteFactory_, indexFormat_, std::to_wstring(localIdx + 1)) + (skin_ == SkinPreset::GreenTerminal ? 6.0f : 5.0f)) : 0.0f;
                float txtW = measureTextWidthDip(
                    dwriteFactory_,
                    (localIdx == 0) ? primaryFormat_ : secondaryFormat_,
                    candStr);
                float itemW = itemPadX * 2.0f + idxW + txtW;
                contentWDp += itemW + (idx + 1 < pageEnd ? itemGapDp : 0.0f);
            }
            if (totalPages > 1) {
                contentWDp += 44.0f;
            }
            contentWDp = std::max(contentWDp, topRowWDp);
        }
        contentWDp = std::clamp(contentWDp, 220.0f, 760.0f);
    }

    int newW = static_cast<int>(std::ceil(dpToPx(contentWDp)));
    int newH = static_cast<int>(std::ceil(dpToPx(totalHDp)));

    if (newW != width_ || newH != height_) {
        width_ = newW;
        height_ = newH;
        SetWindowPos(hwnd_, nullptr, 0, 0, width_, height_,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (renderTarget_) {
            renderTarget_->SetDpi(static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));
            renderTarget_->Resize(D2D1::SizeU(
                static_cast<UINT32>(std::max(1, width_)),
                static_cast<UINT32>(std::max(1, height_))));
        }
        updateWindowRegion();
    }
}

void CandidateWindow::updateWindowRegion() {
    if (!hwnd_) return;
    int cornerPx = std::max(6, static_cast<int>(std::lround(dpToPx(skinOuterCornerDp(skin_)))));
    HRGN rgn = CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, cornerPx * 2, cornerPx * 2);
    if (rgn) {
        SetWindowRgn(hwnd_, rgn, TRUE);
    }
}

void CandidateWindow::moveNearCaret(const RECT& caretRect) {
    HWND fg = GetForegroundWindow();
    auto isValidRect = [](const RECT& r) {
        if (r.left == 0 && r.top == 0 && r.right == 0 && r.bottom == 0) return false;
        if (r.left < -32000 || r.top < -32000 || r.right > 32000 || r.bottom > 32000) return false;
        return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
    };

    RECT resolved = caretRect;
    if (!isValidRect(resolved)) {
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(0, &gti) && gti.hwndCaret) {
            RECT rc = gti.rcCaret;
            POINT tl{rc.left, rc.top};
            POINT br{rc.right, rc.bottom};
            if (ClientToScreen(gti.hwndCaret, &tl) && ClientToScreen(gti.hwndCaret, &br)) {
                RECT candidate{tl.x, tl.y, br.x, br.y};
                if (isValidRect(candidate)) {
                    resolved = candidate;
                }
            }
        }
    }

    if (!isValidRect(resolved) && hasCaretRect_ && fg && lastCaretFgWnd_ == fg && isValidRect(lastCaretRect_)) {
        resolved = lastCaretRect_;
    }

    if (!isValidRect(resolved)) {
        RECT fgRect{};
        bool hasFgRect = fg && GetWindowRect(fg, &fgRect);
        POINT pt{};
        if (GetCursorPos(&pt) && (!hasFgRect || PtInRect(&fgRect, pt))) {
            resolved = RECT{pt.x, pt.y - scaleForDpi(20), pt.x + 2, pt.y};
        } else if (hasFgRect) {
            int cx = fgRect.left + std::max<int>(scaleForDpi(40), static_cast<int>(fgRect.right - fgRect.left) / 3);
            int cy = fgRect.bottom - scaleForDpi(120);
            resolved = RECT{cx, cy - scaleForDpi(20), cx + 2, cy};
        }
    }

    lastCaretRect_ = resolved;
    lastCaretFgWnd_ = fg;
    hasCaretRect_ = isValidRect(resolved);
    if (!hwnd_) return;

    int x = resolved.left;
    int y = resolved.bottom + scaleForDpi(6);

    POINT pt{resolved.left, resolved.bottom};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        RECT work = mi.rcWork;
        if (x + width_ > work.right) {
            x = std::max<int>(work.left, static_cast<int>(work.right) - width_ - scaleForDpi(8));
        }
        if (x < work.left) x = work.left;
        if (y + height_ > work.bottom) {
            y = resolved.top - height_ - scaleForDpi(6);
        }
        if (y + height_ > work.bottom) {
            y = static_cast<int>(work.bottom) - height_ - scaleForDpi(8);
        }
        if (y < work.top) y = work.top;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE);
}

void CandidateWindow::show() {
    if (!hwnd_) return;
    HWND fg = GetForegroundWindow();
    if (!hasCaretRect_ || (fg && lastCaretFgWnd_ != fg)) {
        moveNearCaret(RECT{});
    }
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    syncGlowTimer();
}

void CandidateWindow::hide() {
    if (toastActive_) {
        toastActive_ = false;
        if (hwnd_) KillTimer(hwnd_, kToastTimerId);
    }
    glowEngine_.clear();
    syncGlowTimer();
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

bool CandidateWindow::isVisible() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

void CandidateWindow::syncGlowTimer() {
    if (!hwnd_) return;
    bool needTimer = isVisible() && glowEngine_.isEnabled() && glowEngine_.isMotionAllowed() && glowEngine_.activeCount() > 0;
    if (needTimer && !glowTimerActive_) {
        SetTimer(hwnd_, kGlowTimerId, kGlowTimerIntervalMs, nullptr);
        glowTimerActive_ = true;
    } else if (!needTimer && glowTimerActive_) {
        KillTimer(hwnd_, kGlowTimerId);
        glowTimerActive_ = false;
    }
}

void CandidateWindow::onGlowTimer() {
    glowEngine_.advanceFrame(monotonicNowNanos());
    syncGlowTimer();
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK CandidateWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CandidateWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<CandidateWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        switch (msg) {
            case WM_PAINT:
                self->onPaint();
                return 0;
            case WM_TIMER:
                if (wp == kGlowTimerId) {
                    self->onGlowTimer();
                    return 0;
                }
                if (wp == kToastTimerId) {
                    KillTimer(hwnd, kToastTimerId);
                    self->toastActive_ = false;
                    if (self->composing_.empty()) {
                        self->hide();
                    }
                    return 0;
                }
                break;
            case WM_LBUTTONDOWN:
                self->onLButtonDown(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
                return 0;
            case WM_MOUSEWHEEL: {
                short delta = GET_WHEEL_DELTA_WPARAM(wp);
                if (self->wheelPaging_ && !self->pending_ && self->onPageChange_ && delta != 0) {
                    self->onPageChange_(delta < 0 ? 1 : -1);
                }
                return 0;
            }
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_DPICHANGED:
                self->dpi_ = HIWORD(wp);
                self->recomputeHeaderBudget();
                self->recalculateSize();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_SETTINGCHANGE:
                self->glowEngine_.setMotionAllowed(isClientAreaAnimationEnabled());
                self->syncGlowTimer();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CandidateWindow::onLButtonDown(int x, int y) {
    POINT pt{x, y};
    if (onOpenSettings_ && PtInRect(&settingsRect_, pt)) {
        onOpenSettings_();
        return;
    }
    if (pending_) return;
    if (onPageChange_ && PtInRect(&prevPageRect_, pt)) {
        onPageChange_(-1);
        return;
    }
    if (onPageChange_ && PtInRect(&nextPageRect_, pt)) {
        onPageChange_(1);
        return;
    }
    if (!onSelect_) return;
    for (size_t i = 0; i < candidateRects_.size() && i < candidateIndices_.size(); ++i) {
        if (PtInRect(&candidateRects_[i], pt)) {
            onSelect_(candidateIndices_[i]);
            return;
        }
    }
}

void CandidateWindow::drawTianzigeLogoD2D(float x, float y, float size) {
    if (!renderTarget_) return;
    ID2D1SolidColorBrush* brush = nullptr;
    ColorArgb tileColor = darkMode_ ? ColorArgb::fromHex(0xFFE07A5F) : ColorArgb::fromHex(0xFF2B5B3C);
    ColorArgb gridColor = darkMode_ ? ColorArgb::fromHex(0xFF1F2633) : ColorArgb::fromHex(0xFFFBF8F2);
    ColorArgb dotColor = darkMode_ ? ColorArgb::fromHex(0xFFFFF4E6) : ColorArgb::fromHex(0xFFE66E33);

    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(tileColor), &brush)) || !brush) {
        return;
    }

    float rx = std::max(2.5f, size * 0.22f);
    D2D1_ROUNDED_RECT tile = D2D1::RoundedRect(D2D1::RectF(x, y, x + size, y + size), rx, rx);
    renderTarget_->FillRoundedRectangle(tile, brush);

    float pad = std::max(2.0f, size * 0.20f);
    float stroke = std::max(1.1f, size * 0.085f);
    brush->SetColor(toD2DColor(gridColor));
    D2D1_RECT_F frameRect = D2D1::RectF(x + pad, y + pad, x + size - pad, y + size - pad);
    renderTarget_->DrawRectangle(frameRect, brush, stroke);

    float midX = x + size * 0.5f;
    float midY = y + size * 0.5f;
    renderTarget_->DrawLine(D2D1::Point2F(midX, y + pad), D2D1::Point2F(midX, y + size - pad), brush, stroke * 0.85f);
    renderTarget_->DrawLine(D2D1::Point2F(x + pad, midY), D2D1::Point2F(x + size - pad, midY), brush, stroke * 0.85f);

    // Warm orange accent dot in bottom-right cell
    brush->SetColor(toD2DColor(dotColor));
    float dotR = std::max(1.4f, size * 0.105f);
    float dotCx = x + size * 0.78f;
    float dotCy = y + size * 0.78f;
    renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotCx, dotCy), dotR, dotR), brush);
    brush->Release();
}

void CandidateWindow::drawGreenTerminalChrome(
    float wDip, float hDip,
    float innerL, float innerT, float innerR, float innerB,
    float dividerY) {
    if (!renderTarget_ || !d2dFactory_) return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(ColorArgb::fromHex(0xFFD4CCB9)), &brush)) || !brush) {
        return;
    }

    const float outerR = skinOuterCornerDp(SkinPreset::GreenTerminal);
    D2D1_ROUNDED_RECT outerRR = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, wDip, hDip), outerR, outerR);

    // 1. Retro beige CRT plastic bezel with vertical 3D bevel gradient
    D2D1_GRADIENT_STOP bezelStops[3] = {
        {0.0f, toD2DColor(ColorArgb::fromHex(0xFFEAE2D2))},
        {0.48f, toD2DColor(ColorArgb::fromHex(0xFFD4CCB9))},
        {1.0f, toD2DColor(ColorArgb::fromHex(0xFFB8AF99))},
    };
    ID2D1GradientStopCollection* stopCol = nullptr;
    if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(bezelStops, 3, &stopCol)) && stopCol) {
        ID2D1LinearGradientBrush* grad = nullptr;
        if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, hDip)),
                stopCol, &grad)) && grad) {
            renderTarget_->FillRoundedRectangle(outerRR, grad);
            grad->Release();
        }
        stopCol->Release();
    } else {
        renderTarget_->FillRoundedRectangle(outerRR, brush);
    }

    // Inner bevel highlight & outer frame stroke
    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFF6F1E5).withAlpha(0.75f)));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(1.5f, 1.5f, wDip - 1.5f, hDip - 1.5f), outerR - 1.0f, outerR - 1.0f),
        brush, 1.0f);
    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF8E8574)));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f), outerR, outerR),
        brush, 1.0f);

    // 2. Recessed CRT bezel rim + dark phosphor screen
    D2D1_RECT_F screenRc = D2D1::RectF(innerL, innerT, innerR, innerB);
    D2D1_ROUNDED_RECT screenRR = D2D1::RoundedRect(screenRc, 3.5f, 3.5f);

    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF6B6558)));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(innerL - 1.2f, innerT - 1.2f, innerR + 1.2f, innerB + 1.2f), 4.2f, 4.2f),
        brush, 1.4f);

    ID2D1RoundedRectangleGeometry* clipGeo = nullptr;
    if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(screenRR, &clipGeo)) && clipGeo) {
        renderTarget_->PushLayer(D2D1::LayerParameters(screenRc, clipGeo), nullptr);

        D2D1_GRADIENT_STOP crtStops[3] = {
            {0.0f, toD2DColor(ColorArgb::fromHex(0xFF102013))},
            {0.5f, toD2DColor(ColorArgb::fromHex(0xFF142717))},
            {1.0f, toD2DColor(ColorArgb::fromHex(0xFF0C190E))},
        };
        ID2D1GradientStopCollection* crtCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(crtStops, 3, &crtCol)) && crtCol) {
            ID2D1LinearGradientBrush* crtGrad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, innerT), D2D1::Point2F(0.0f, innerB)),
                    crtCol, &crtGrad)) && crtGrad) {
                renderTarget_->FillRectangle(screenRc, crtGrad);
                crtGrad->Release();
            }
            crtCol->Release();
        } else {
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF0F1E12)));
            renderTarget_->FillRectangle(screenRc, brush);
        }

        // Subtle CRT horizontal scanlines
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF000000).withAlpha(0.20f)));
        for (float sy = innerT + 2.0f; sy < innerB; sy += 2.6f) {
            renderTarget_->DrawLine(D2D1::Point2F(innerL, sy), D2D1::Point2F(innerR, sy), brush, 0.7f);
        }

        // Subtle inner edge vignette
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF060E07).withAlpha(0.75f)));
        renderTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(innerL + 0.5f, innerT + 0.5f, innerR - 0.5f, innerB - 0.5f), 3.0f, 3.0f),
            brush, 1.5f);

        // Phosphor divider line between composing row and candidates
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF214526)));
        renderTarget_->DrawLine(
            D2D1::Point2F(innerL + 6.0f, dividerY),
            D2D1::Point2F(innerR - 6.0f, dividerY),
            brush, 0.9f);

        // Top-right three status LEDs (two dim olive, rightmost glowing phosphor green)
        float ledCy = innerT + 6.0f + codeHeightDp_ * 0.5f;
        float led3X = innerR - 12.0f;
        float led2X = led3X - 10.5f;
        float led1X = led2X - 10.5f;

        for (float lx : {led1X, led2X}) {
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF475E49)));
            renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lx, ledCy), 2.5f, 2.5f), brush);
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF6B886E).withAlpha(0.6f)));
            renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lx - 0.6f, ledCy - 0.6f), 0.9f, 0.9f), brush);
        }

        // Glowing active green LED
        D2D1_GRADIENT_STOP glowStops[2] = {
            {0.0f, toD2DColor(ColorArgb::fromHex(0xFF7BFB62).withAlpha(0.62f))},
            {1.0f, toD2DColor(ColorArgb::fromHex(0xFF7BFB62).withAlpha(0.0f))},
        };
        ID2D1GradientStopCollection* glowCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(glowStops, 2, &glowCol)) && glowCol) {
            ID2D1RadialGradientBrush* rad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(D2D1::Point2F(led3X, ledCy), D2D1::Point2F(0.0f, 0.0f), 8.0f, 8.0f),
                    glowCol, &rad)) && rad) {
                renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(led3X, ledCy), 8.0f, 8.0f), rad);
                rad->Release();
            }
            glowCol->Release();
        }
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF7BFB62)));
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(led3X, ledCy), 2.7f, 2.7f), brush);
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFDFFFD8)));
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(led3X - 0.6f, ledCy - 0.6f), 1.0f, 1.0f), brush);

        renderTarget_->PopLayer();
        clipGeo->Release();
    }

    brush->Release();
}

void CandidateWindow::drawWoodTypeChrome(
    float wDip, float hDip,
    float innerL, float innerT, float innerR, float innerB,
    float dividerY) {
    if (!renderTarget_ || !d2dFactory_) return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(ColorArgb::fromHex(0xFF684127)), &brush)) || !brush) {
        return;
    }

    const float outerR = skinOuterCornerDp(SkinPreset::WoodType);
    D2D1_ROUNDED_RECT outerRR = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, wDip, hDip), outerR, outerR);

    // 1. Rich walnut wood frame with beveled gradient
    D2D1_GRADIENT_STOP woodStops[4] = {
        {0.0f, toD2DColor(ColorArgb::fromHex(0xFF8C5F41))},
        {0.25f, toD2DColor(ColorArgb::fromHex(0xFF6E462B))},
        {0.75f, toD2DColor(ColorArgb::fromHex(0xFF5D3920))},
        {1.0f, toD2DColor(ColorArgb::fromHex(0xFF472A16))},
    };
    ID2D1GradientStopCollection* stopCol = nullptr;
    if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(woodStops, 4, &stopCol)) && stopCol) {
        ID2D1LinearGradientBrush* grad = nullptr;
        if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, hDip)),
                stopCol, &grad)) && grad) {
            renderTarget_->FillRoundedRectangle(outerRR, grad);
            grad->Release();
        }
        stopCol->Release();
    } else {
        renderTarget_->FillRoundedRectangle(outerRR, brush);
    }

    // Subtle horizontal wood grain streaks on top & bottom rails
    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF3E2210).withAlpha(0.35f)));
    renderTarget_->DrawLine(D2D1::Point2F(4.0f, 2.2f), D2D1::Point2F(wDip - 4.0f, 2.2f), brush, 0.7f);
    renderTarget_->DrawLine(D2D1::Point2F(4.0f, hDip - 2.2f), D2D1::Point2F(wDip - 4.0f, hDip - 2.2f), brush, 0.7f);
    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFA07454).withAlpha(0.28f)));
    renderTarget_->DrawLine(D2D1::Point2F(4.0f, 3.8f), D2D1::Point2F(wDip - 4.0f, 3.8f), brush, 0.6f);

    // Outer dark walnut rim
    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF3D2211)));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f), outerR, outerR),
        brush, 1.0f);

    // 2. Xuan paper interior
    D2D1_RECT_F paperRc = D2D1::RectF(innerL, innerT, innerR, innerB);
    D2D1_ROUNDED_RECT paperRR = D2D1::RoundedRect(paperRc, 2.5f, 2.5f);

    ID2D1RoundedRectangleGeometry* clipGeo = nullptr;
    if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(paperRR, &clipGeo)) && clipGeo) {
        renderTarget_->PushLayer(D2D1::LayerParameters(paperRc, clipGeo), nullptr);

        D2D1_GRADIENT_STOP paperStops[3] = {
            {0.0f, toD2DColor(ColorArgb::fromHex(0xFFEFDFC6))},
            {0.5f, toD2DColor(ColorArgb::fromHex(0xFFF4E6CF))},
            {1.0f, toD2DColor(ColorArgb::fromHex(0xFFECDBC0))},
        };
        ID2D1GradientStopCollection* pCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(paperStops, 3, &pCol)) && pCol) {
            ID2D1LinearGradientBrush* pGrad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, innerT), D2D1::Point2F(0.0f, innerB)),
                    pCol, &pGrad)) && pGrad) {
                renderTarget_->FillRectangle(paperRc, pGrad);
                pGrad->Release();
            }
            pCol->Release();
        } else {
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFF3E4CC)));
            renderTarget_->FillRectangle(paperRc, brush);
        }

        // Recessed inner wood shadow on paper perimeter
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF4A2D18).withAlpha(0.24f)));
        renderTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(innerL + 0.5f, innerT + 0.5f, innerR - 0.5f, innerB - 0.5f), 2.0f, 2.0f),
            brush, 1.8f);

        // Crisp ink divider line between composing row and candidates
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFAA9A81)));
        renderTarget_->DrawLine(
            D2D1::Point2F(innerL + 10.0f, dividerY),
            D2D1::Point2F(innerR - 10.0f, dividerY),
            brush, 0.85f);

        renderTarget_->PopLayer();
        clipGeo->Release();
    }

    brush->Release();
}

void CandidateWindow::drawClassicTypewriterChrome(
    float wDip, float hDip,
    float innerL, float innerT, float innerR, float innerB) {
    if (!renderTarget_ || !d2dFactory_) return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(ColorArgb::fromHex(0xFF8F8476)), &brush)) || !brush) {
        return;
    }

    const float outerR = skinOuterCornerDp(SkinPreset::ClassicTypewriter);
    D2D1_ROUNDED_RECT outerRR = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, wDip, hDip), outerR, outerR);

    // 1. Brushed vintage pewter/steel metallic outer bezel
    D2D1_GRADIENT_STOP metalStops[4] = {
        {0.0f, toD2DColor(ColorArgb::fromHex(0xFFD3C9BC))},
        {0.35f, toD2DColor(ColorArgb::fromHex(0xFF8C8173))},
        {0.72f, toD2DColor(ColorArgb::fromHex(0xFF564F47))},
        {1.0f, toD2DColor(ColorArgb::fromHex(0xFFB5ABA0))},
    };
    ID2D1GradientStopCollection* mCol = nullptr;
    if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(metalStops, 4, &mCol)) && mCol) {
        ID2D1LinearGradientBrush* mGrad = nullptr;
        if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, hDip)),
                mCol, &mGrad)) && mGrad) {
            renderTarget_->FillRoundedRectangle(outerRR, mGrad);
            mGrad->Release();
        }
        mCol->Release();
    } else {
        renderTarget_->FillRoundedRectangle(outerRR, brush);
    }

    // Metallic bevel highlight & dark inner groove
    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFE5DDD2).withAlpha(0.75f)));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(1.4f, 1.4f, wDip - 1.4f, hDip - 1.4f), outerR - 1.0f, outerR - 1.0f),
        brush, 0.9f);
    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF3A3530)));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f), outerR, outerR),
        brush, 1.0f);

    const float frameInset = skinFrameInsetDp(SkinPreset::ClassicTypewriter);
    D2D1_RECT_F cavityRc = D2D1::RectF(frameInset, frameInset, wDip - frameInset, hDip - frameInset);
    D2D1_ROUNDED_RECT cavityRR = D2D1::RoundedRect(cavityRc, 4.0f, 4.0f);

    brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF1E1A18)));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(frameInset - 1.0f, frameInset - 1.0f, wDip - frameInset + 1.0f, hDip - frameInset + 1.0f), 4.6f, 4.6f),
        brush, 1.8f);

    // 2. Cream typewriter paper + signature top platen roller bar
    ID2D1RoundedRectangleGeometry* clipGeo = nullptr;
    if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(cavityRR, &clipGeo)) && clipGeo) {
        renderTarget_->PushLayer(D2D1::LayerParameters(cavityRc, clipGeo), nullptr);

        D2D1_GRADIENT_STOP paperStops[3] = {
            {0.0f, toD2DColor(ColorArgb::fromHex(0xFFEEDDC2))},
            {0.45f, toD2DColor(ColorArgb::fromHex(0xFFF6E8D0))},
            {1.0f, toD2DColor(ColorArgb::fromHex(0xFFECDBC0))},
        };
        ID2D1GradientStopCollection* pCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(paperStops, 3, &pCol)) && pCol) {
            ID2D1LinearGradientBrush* pGrad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, frameInset), D2D1::Point2F(0.0f, innerB)),
                    pCol, &pGrad)) && pGrad) {
                renderTarget_->FillRectangle(cavityRc, pGrad);
                pGrad->Release();
            }
            pCol->Release();
        } else {
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFF6E8D0)));
            renderTarget_->FillRectangle(cavityRc, brush);
        }

        // Cast shadow from platen roller onto paper
        float rollerTop = frameInset + 1.5f;
        float rollerH = 7.5f;
        float rollerBot = rollerTop + rollerH;
        D2D1_GRADIENT_STOP shStops[2] = {
            {0.0f, toD2DColor(ColorArgb::fromHex(0xFF1A1510).withAlpha(0.28f))},
            {1.0f, toD2DColor(ColorArgb::fromHex(0xFF1A1510).withAlpha(0.0f))},
        };
        ID2D1GradientStopCollection* shCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(shStops, 2, &shCol)) && shCol) {
            ID2D1LinearGradientBrush* shGrad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, rollerBot), D2D1::Point2F(0.0f, rollerBot + 6.0f)),
                    shCol, &shGrad)) && shGrad) {
                renderTarget_->FillRectangle(D2D1::RectF(innerL, rollerBot, innerR, rollerBot + 6.0f), shGrad);
                shGrad->Release();
            }
            shCol->Release();
        }

        // Horizontal black cylindrical rubber platen roller
        D2D1_RECT_F rollerRc = D2D1::RectF(innerL + 2.0f, rollerTop, innerR - 2.0f, rollerBot);
        D2D1_GRADIENT_STOP rStops[4] = {
            {0.0f, toD2DColor(ColorArgb::fromHex(0xFF141311))},
            {0.28f, toD2DColor(ColorArgb::fromHex(0xFF494641))},
            {0.62f, toD2DColor(ColorArgb::fromHex(0xFF161513))},
            {1.0f, toD2DColor(ColorArgb::fromHex(0xFF0A0908))},
        };
        ID2D1GradientStopCollection* rCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(rStops, 4, &rCol)) && rCol) {
            ID2D1LinearGradientBrush* rGrad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, rollerTop), D2D1::Point2F(0.0f, rollerBot)),
                    rCol, &rGrad)) && rGrad) {
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rollerRc, 2.0f, 2.0f), rGrad);
                rGrad->Release();
            }
            rCol->Release();
        } else {
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF12110F)));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rollerRc, 2.0f, 2.0f), brush);
        }

        // Left & right metallic knurled roller end-caps
        auto drawCap = [&](float cx) {
            D2D1_RECT_F capRc = D2D1::RectF(cx, rollerTop - 1.0f, cx + 11.0f, rollerBot + 1.0f);
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFFAFA598)));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(capRc, 2.0f, 2.0f), brush);
            brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF423C35)));
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(capRc, 2.0f, 2.0f), brush, 0.9f);
            for (float kx = cx + 3.0f; kx <= cx + 8.5f; kx += 2.6f) {
                renderTarget_->DrawLine(
                    D2D1::Point2F(kx, rollerTop),
                    D2D1::Point2F(kx, rollerBot),
                    brush, 0.9f);
            }
        };
        drawCap(innerL + 4.0f);
        drawCap(innerR - 15.0f);

        // Subtle inner edge shadow on paper
        brush->SetColor(toD2DColor(ColorArgb::fromHex(0xFF3A3025).withAlpha(0.18f)));
        renderTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(innerL + 0.5f, innerT - 9.0f, innerR - 0.5f, innerB - 0.5f), 3.0f, 3.0f),
            brush, 1.2f);

        renderTarget_->PopLayer();
        clipGeo->Release();
    }

    brush->Release();
}

void CandidateWindow::onPaint() {
    PAINTSTRUCT ps = {};
    BeginPaint(hwnd_, &ps);

    if (!ensureDeviceResources()) {
        EndPaint(hwnd_, &ps);
        return;
    }

    const SkinMetrics metrics = skinMetrics(skin_);
    const bool crafted = isCraftedSkin(skin_);
    const bool showIdx = showsCandidateIndex(skin_);
    const float frameInset = skinFrameInsetDp(skin_);
    const float topExtra = skinTopChromeExtraDp(skin_);

    const float wDip = static_cast<float>(width_) * 96.0f / static_cast<float>(dpi_);
    const float hDip = static_cast<float>(height_) * 96.0f / static_cast<float>(dpi_);
    const float cornerDp = skinOuterCornerDp(skin_);

    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, wDip, hDip);
    D2D1_ROUNDED_RECT roundedBounds = D2D1::RoundedRect(fullRect, cornerDp, cornerDp);

    // Resolve palette per skin
    ColorArgb topBg = darkMode_ ? ColorArgb::fromHex(0xFF1F2633) : ColorArgb::fromHex(0xFFF6F1E6);
    ColorArgb bottomBandBg = darkMode_ ? ColorArgb::fromHex(0xFF19202B) : ColorArgb::fromHex(0xFFEFE8D9);
    ColorArgb pillAccent = darkMode_ ? ColorArgb::fromHex(0xFFE07A5F) : ColorArgb::fromHex(0xFF2B5B3C);
    ColorArgb pillText = darkMode_ ? ColorArgb::fromHex(0xFF1B222E) : ColorArgb::fromHex(0xFFFFFFFF);
    ColorArgb composingText = darkMode_ ? ColorArgb::fromHex(0xFFE88D73) : ColorArgb::fromHex(0xFF23201C);
    ColorArgb mainText = darkMode_ ? ColorArgb::fromHex(0xFFE88D73) : ColorArgb::fromHex(0xFF23201C);
    ColorArgb subText = darkMode_ ? ColorArgb::fromHex(0xFFB48372) : ColorArgb::fromHex(0xFF6E6659);
    ColorArgb borderCol = darkMode_ ? ColorArgb::fromHex(0xFF333D4F) : ColorArgb::fromHex(0xFFDDD4C0);
    ColorArgb caretCol = mainText.withAlpha(0.75f);

    if (skin_ == SkinPreset::NativeLight && !darkMode_) {
        topBg = ColorArgb::fromHex(0xFFFCFDFB);
        bottomBandBg = ColorArgb::fromHex(0xFFF2F5F2);
        pillAccent = ColorArgb::fromHex(0xFF28593A);
        borderCol = ColorArgb::fromHex(0xFFD6DFD8);
    } else if (skin_ == SkinPreset::Mint && !darkMode_) {
        topBg = ColorArgb::fromHex(0xFFF1F7F4);
        bottomBandBg = ColorArgb::fromHex(0xFFE5F0EB);
        pillAccent = ColorArgb::fromHex(0xFF1F8A68);
        borderCol = ColorArgb::fromHex(0xFFC8E0D6);
    } else if (skin_ == SkinPreset::DeepBlue) {
        topBg = ColorArgb::fromHex(0xFF18243A);
        bottomBandBg = ColorArgb::fromHex(0xFF131D30);
        pillAccent = ColorArgb::fromHex(0xFFF28B66);
        pillText = ColorArgb::fromHex(0xFF131D30);
        composingText = ColorArgb::fromHex(0xFFEEF3FC);
        mainText = ColorArgb::fromHex(0xFFEEF3FC);
        subText = ColorArgb::fromHex(0xFF94A6C8);
        borderCol = ColorArgb::fromHex(0xFF2C3F63);
    } else if (skin_ == SkinPreset::GreenTerminal) {
        topBg = ColorArgb::fromHex(0xFF0F1E12);
        bottomBandBg = ColorArgb::fromHex(0xFF0F1E12);
        pillAccent = ColorArgb::fromHex(0xFF78F867);
        pillText = ColorArgb::fromHex(0xFF0B1A0D);
        composingText = ColorArgb::fromHex(0xFFF4E667);
        mainText = ColorArgb::fromHex(0xFF8DEE6F);
        subText = ColorArgb::fromHex(0xFF8DEE6F);
        borderCol = ColorArgb::fromHex(0xFFD4CCB9);
    } else if (skin_ == SkinPreset::WoodType) {
        topBg = ColorArgb::fromHex(0xFFF3E4CC);
        bottomBandBg = ColorArgb::fromHex(0xFFF3E4CC);
        pillAccent = ColorArgb::fromHex(0xFFB54632);
        pillText = ColorArgb::fromHex(0xFFB03E2B);
        composingText = ColorArgb::fromHex(0xFF292015);
        mainText = ColorArgb::fromHex(0xFF241C14);
        subText = ColorArgb::fromHex(0xFF7B6A54);
        borderCol = ColorArgb::fromHex(0xFF684127);
        caretCol = ColorArgb::fromHex(0xFF3A2E22);
    } else if (skin_ == SkinPreset::ClassicTypewriter) {
        topBg = ColorArgb::fromHex(0xFFF6E8D0);
        bottomBandBg = ColorArgb::fromHex(0xFFF6E8D0);
        pillAccent = ColorArgb::fromHex(0xFF2B2723);
        pillText = ColorArgb::fromHex(0xFFF7EEDB);
        composingText = ColorArgb::fromHex(0xFF221E1A);
        mainText = ColorArgb::fromHex(0xFF221E1A);
        subText = ColorArgb::fromHex(0xFF6E6558);
        borderCol = ColorArgb::fromHex(0xFF8F8476);
        caretCol = ColorArgb::fromHex(0xFF237218);
    }

    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(toD2DColor(topBg), &brush);

    // Mode-switch floating toast (Shift toggle)
    if (toastActive_ && composing_.empty() && brush) {
        if (skin_ == SkinPreset::GreenTerminal) {
            drawGreenTerminalChrome(wDip, hDip, frameInset, frameInset, wDip - frameInset, hDip - frameInset, hDip + 10.0f);
        } else if (skin_ == SkinPreset::WoodType) {
            drawWoodTypeChrome(wDip, hDip, frameInset, frameInset, wDip - frameInset, hDip - frameInset, hDip + 10.0f);
        } else if (skin_ == SkinPreset::ClassicTypewriter) {
            drawClassicTypewriterChrome(wDip, hDip, frameInset, frameInset + 8.0f, wDip - frameInset, hDip - frameInset);
        } else {
            renderTarget_->FillRoundedRectangle(roundedBounds, brush);
        }

        float logoSize = 18.0f;
        float logoX = frameInset + 12.0f;
        float logoY = (hDip - logoSize) * 0.5f;
        if (!crafted) {
            drawTianzigeLogoD2D(logoX, logoY, logoSize);
        }

        if (primaryFormat_) {
            std::wstring modeStr = toastEnglish_ ? L"英" : L"中";
            D2D1_RECT_F txtRc = crafted
                ? D2D1::RectF(frameInset, 0.0f, wDip - frameInset, hDip)
                : D2D1::RectF(logoX + logoSize + 10.0f, 0.0f, wDip - 10.0f, hDip);
            if (crafted) primaryFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(toD2DColor(crafted ? composingText : mainText));
            renderTarget_->DrawText(modeStr.c_str(), static_cast<UINT32>(modeStr.size()), primaryFormat_, txtRc, brush);
            if (crafted) primaryFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        if (!crafted) {
            brush->SetColor(toD2DColor(borderCol));
            D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f), cornerDp, cornerDp);
            renderTarget_->DrawRoundedRectangle(borderRect, brush, 1.0f);
        }
        brush->Release();

        HRESULT hr = renderTarget_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) releaseDeviceResources();
        EndPaint(hwnd_, &ps);
        return;
    }

    const float padXDip = 12.0f;
    const float padYDip = crafted ? 6.0f : 5.0f;
    const float innerL = frameInset;
    const float innerT = frameInset + topExtra;
    const float innerR = wDip - frameInset;
    const float innerB = hDip - frameInset;

    const float codeTopDip = innerT + padYDip;
    const float dividerY = codeTopDip + codeHeightDp_ + 2.0f;
    const float candTopDip = dividerY + (crafted ? 4.0f : 1.0f);
    const float candBottomDip = candTopDip + candidateHeightDp_;

    // Background & physical skin chrome
    if (skin_ == SkinPreset::GreenTerminal) {
        drawGreenTerminalChrome(wDip, hDip, innerL, innerT, innerR, innerB, dividerY);
    } else if (skin_ == SkinPreset::WoodType) {
        drawWoodTypeChrome(wDip, hDip, innerL, innerT, innerR, innerB, dividerY);
    } else if (skin_ == SkinPreset::ClassicTypewriter) {
        drawClassicTypewriterChrome(wDip, hDip, innerL, innerT, innerR, innerB);
    } else if (brush) {
        renderTarget_->FillRoundedRectangle(roundedBounds, brush);
        brush->SetColor(toD2DColor(bottomBandBg));
        D2D1_RECT_F bandRect = D2D1::RectF(0.0f, candTopDip - 1.0f, wDip, hDip);
        renderTarget_->FillRectangle(bandRect, brush);
    }

    // TypingGlow layer (subtle on legacy skins; skip over opaque frames)
    if (!crafted) {
        auto glowCircles = glowEngine_.buildRenderCircles(
            wDip, hDip, 1.0f, darkMode_, metrics.glowColors);
        for (const auto& circle : glowCircles) {
            D2D1_GRADIENT_STOP gStops[3] = {};
            UINT32 count = circle.isTrailing ? 2 : 3;
            if (circle.isTrailing) {
                gStops[0] = {0.0f, toD2DColor(circle.innerColor)};
                gStops[1] = {1.0f, toD2DColor(circle.midColor)};
            } else {
                gStops[0] = {0.0f, toD2DColor(circle.innerColor)};
                gStops[1] = {0.55f, toD2DColor(circle.midColor)};
                gStops[2] = {1.0f, toD2DColor(circle.midColor.withAlpha(0.0f))};
            }
            ID2D1GradientStopCollection* stopCol = nullptr;
            if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(gStops, count, &stopCol)) && stopCol) {
                ID2D1RadialGradientBrush* radBrush = nullptr;
                if (SUCCEEDED(renderTarget_->CreateRadialGradientBrush(
                        D2D1::RadialGradientBrushProperties(
                            D2D1::Point2F(circle.cx, circle.cy),
                            D2D1::Point2F(0.0f, 0.0f),
                            circle.radius,
                            circle.radius),
                        stopCol,
                        &radBrush)) && radBrush) {
                    renderTarget_->FillEllipse(
                        D2D1::Ellipse(D2D1::Point2F(circle.cx, circle.cy), circle.radius, circle.radius),
                        radBrush);
                    radBrush->Release();
                }
                stopCol->Release();
            }
        }
    }

    // Top row: composing text
    float codeStartX = innerL + padXDip;
    if (!crafted) {
        const float logoSizeDip = 16.0f;
        float logoX = innerL + padXDip;
        float logoY = codeTopDip + (codeHeightDp_ - logoSizeDip) * 0.5f;
        drawTianzigeLogoD2D(logoX, logoY, logoSizeDip);
        settingsRect_ = RECT{
            static_cast<LONG>(std::floor(dpToPx(logoX - 4.0f))),
            static_cast<LONG>(std::floor(dpToPx(codeTopDip))),
            static_cast<LONG>(std::ceil(dpToPx(logoX + logoSizeDip + 4.0f))),
            static_cast<LONG>(std::ceil(dpToPx(codeTopDip + codeHeightDp_ + 2.0f)))
        };
        codeStartX = logoX + logoSizeDip + 7.0f;
    } else {
        // Clicking the "> " prompt opens settings
        settingsRect_ = RECT{
            static_cast<LONG>(std::floor(dpToPx(innerL + 4.0f))),
            static_cast<LONG>(std::floor(dpToPx(codeTopDip))),
            static_cast<LONG>(std::ceil(dpToPx(innerL + padXDip + 16.0f))),
            static_cast<LONG>(std::ceil(dpToPx(codeTopDip + codeHeightDp_)))
        };
    }

    if (brush && codeFormat_) {
        std::wstring compWide = crafted ? (L"> " + utf8ToWide(composing_)) : utf8ToWide(composing_);
        float compW = measureTextWidthDip(dwriteFactory_, codeFormat_, compWide);
        float maxCodeRight = innerR - padXDip - (skin_ == SkinPreset::GreenTerminal ? 42.0f : 6.0f);
        D2D1_RECT_F compRc = D2D1::RectF(
            codeStartX,
            codeTopDip,
            std::max(codeStartX + 20.0f, maxCodeRight),
            codeTopDip + codeHeightDp_);
        brush->SetColor(toD2DColor(composingText));
        renderTarget_->DrawText(
            compWide.c_str(),
            static_cast<UINT32>(compWide.size()),
            codeFormat_,
            compRc,
            brush);

        // Caret bar styling per skin:
        // - GreenTerminal: no caret (exact match to final design)
        // - WoodType: crisp thin vertical ink bar |
        // - ClassicTypewriter: signature emerald green vertical block caret (#237218)
        if (skin_ != SkinPreset::GreenTerminal) {
            float caretX = std::min(maxCodeRight - 2.0f, codeStartX + compW + (skin_ == SkinPreset::ClassicTypewriter ? 5.0f : 4.0f));
            float caretTop = codeTopDip + 3.0f;
            float caretBot = codeTopDip + codeHeightDp_ - 3.0f;
            brush->SetColor(toD2DColor(caretCol));
            if (skin_ == SkinPreset::ClassicTypewriter) {
                renderTarget_->FillRectangle(
                    D2D1::RectF(caretX, caretTop, caretX + 2.6f, caretBot),
                    brush);
            } else {
                renderTarget_->DrawLine(
                    D2D1::Point2F(caretX, caretTop),
                    D2D1::Point2F(caretX, caretBot),
                    brush,
                    skin_ == SkinPreset::WoodType ? 1.15f : 1.4f);
            }
        }
    }

    // Candidates (Horizontal or Vertical)
    candidateRects_.clear();
    candidateIndices_.clear();
    prevPageRect_ = RECT{0, 0, 0, 0};
    nextPageRect_ = RECT{0, 0, 0, 0};

    size_t pageStart = 0;
    size_t pageEnd = 0;
    size_t currentPage = 0;
    size_t totalPages = 0;
    computePageBounds(pageStart, pageEnd, currentPage, totalPages);

    const float itemPadX = crafted ? 10.0f : HeaderMetricsSpec::kItemHorizontalPaddingDp;
    const float itemGap = crafted ? 10.0f : 6.0f;

    auto drawSelectionBg = [&](const D2D1_RECT_F& itemRc) {
        if (!brush) return;
        if (skin_ == SkinPreset::WoodType) {
            // Vermilion red-seal outline box (朱砂印章边框)
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(itemRc, 3.5f, 3.5f);
            brush->SetColor(toD2DColor(pillAccent.withAlpha(0.045f)));
            renderTarget_->FillRoundedRectangle(rr, brush);
            brush->SetColor(toD2DColor(pillAccent));
            renderTarget_->DrawRoundedRectangle(rr, brush, 1.35f);
        } else if (skin_ == SkinPreset::GreenTerminal) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(itemRc, 3.5f, 3.5f);
            brush->SetColor(toD2DColor(pillAccent));
            renderTarget_->FillRoundedRectangle(rr, brush);
        } else if (skin_ == SkinPreset::ClassicTypewriter) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(itemRc, 4.0f, 4.0f);
            brush->SetColor(toD2DColor(pillAccent));
            renderTarget_->FillRoundedRectangle(rr, brush);
        } else {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(itemRc, 5.5f, 5.5f);
            brush->SetColor(toD2DColor(pillAccent));
            renderTarget_->FillRoundedRectangle(rr, brush);
        }
    };

    if (layout_ == CandidateLayout::Vertical) {
        float rowH = std::max(30.0f, candidateHeightDp_ - 4.0f);
        float curY = candTopDip + 1.0f;
        float rowL = innerL + padXDip;
        float rowR = innerR - padXDip;

        for (size_t idx = pageStart; idx < pageEnd; ++idx) {
            size_t localIdx = idx - pageStart;
            std::wstring idxWide = std::to_wstring(localIdx + 1);
            std::wstring candWide = utf8ToWide(candidates_[idx]);
            IDWriteTextFormat* wordFormat = (localIdx == 0) ? primaryFormat_ : secondaryFormat_;
            bool isSelected = (static_cast<int>(idx) == selectedIndex_);

            D2D1_RECT_F itemRc = D2D1::RectF(rowL, curY + 2.0f, rowR, curY + rowH - 2.0f);
            if (isSelected && !pending_) {
                drawSelectionBg(itemRc);
            }

            if (brush && wordFormat) {
                ColorArgb idxColor = pending_
                    ? subText.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                    : (isSelected ? pillText : subText);
                ColorArgb wordColor = pending_
                    ? subText.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                    : (isSelected ? pillText : mainText);

                float textX = rowL + itemPadX;
                if (showIdx && indexFormat_) {
                    float idxWDip = measureTextWidthDip(dwriteFactory_, indexFormat_, idxWide);
                    D2D1_RECT_F idxRc = D2D1::RectF(textX, curY, textX + idxWDip + 2.0f, curY + rowH);
                    brush->SetColor(toD2DColor(idxColor));
                    renderTarget_->DrawText(
                        idxWide.c_str(),
                        static_cast<UINT32>(idxWide.size()),
                        indexFormat_,
                        idxRc,
                        brush);
                    textX += idxWDip + (skin_ == SkinPreset::GreenTerminal ? 8.0f : 6.0f);
                }

                D2D1_RECT_F wordRc = D2D1::RectF(textX, curY, rowR - itemPadX, curY + rowH);
                brush->SetColor(toD2DColor(wordColor));
                renderTarget_->DrawText(
                    candWide.c_str(),
                    static_cast<UINT32>(candWide.size()),
                    wordFormat,
                    wordRc,
                    brush);
            }

            RECT hitRc{
                static_cast<LONG>(std::floor(dpToPx(rowL))),
                static_cast<LONG>(std::floor(dpToPx(curY))),
                static_cast<LONG>(std::ceil(dpToPx(rowR))),
                static_cast<LONG>(std::ceil(dpToPx(curY + rowH)))
            };
            candidateRects_.push_back(hitRc);
            candidateIndices_.push_back(idx);
            curY += rowH;
        }

        if (totalPages > 1 && brush && codeFormat_) {
            const float pagerBtnW = 18.0f;
            float nextL = rowR - pagerBtnW;
            float prevL = nextL - 6.0f - pagerBtnW;
            bool canPrev = (currentPage > 0) && !pending_;
            bool canNext = (currentPage + 1 < totalPages) && !pending_;
            D2D1_RECT_F prevRc = D2D1::RectF(prevL, curY, prevL + pagerBtnW, curY + 20.0f);
            D2D1_RECT_F nextRc = D2D1::RectF(nextL, curY, nextL + pagerBtnW, curY + 20.0f);
            codeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(toD2DColor(canPrev ? mainText : subText.withAlpha(0.35f)));
            renderTarget_->DrawText(L"‹", 1, codeFormat_, prevRc, brush);
            brush->SetColor(toD2DColor(canNext ? mainText : subText.withAlpha(0.35f)));
            renderTarget_->DrawText(L"›", 1, codeFormat_, nextRc, brush);
            codeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            if (canPrev) {
                prevPageRect_ = RECT{
                    static_cast<LONG>(std::floor(dpToPx(prevRc.left))),
                    static_cast<LONG>(std::floor(dpToPx(prevRc.top))),
                    static_cast<LONG>(std::ceil(dpToPx(prevRc.right))),
                    static_cast<LONG>(std::ceil(dpToPx(prevRc.bottom)))
                };
            }
            if (canNext) {
                nextPageRect_ = RECT{
                    static_cast<LONG>(std::floor(dpToPx(nextRc.left))),
                    static_cast<LONG>(std::floor(dpToPx(nextRc.top))),
                    static_cast<LONG>(std::ceil(dpToPx(nextRc.right))),
                    static_cast<LONG>(std::ceil(dpToPx(nextRc.bottom)))
                };
            }
        }
    } else {
        // Horizontal layout
        float curXDip = innerL + padXDip;
        float pagerTotalDip = 0.0f;
        const float pagerBtnWDip = 16.0f;
        if (totalPages > 1) {
            pagerTotalDip = 8.0f + pagerBtnWDip + 4.0f + pagerBtnWDip;
        }
        float maxCandRightDip = innerR - padXDip - pagerTotalDip;

        for (size_t idx = pageStart; idx < pageEnd; ++idx) {
            size_t localIdx = idx - pageStart;
            std::wstring idxWide = std::to_wstring(localIdx + 1);
            std::wstring candWide = utf8ToWide(candidates_[idx]);
            IDWriteTextFormat* wordFormat = (localIdx == 0) ? primaryFormat_ : secondaryFormat_;

            float idxWDip = showIdx ? measureTextWidthDip(dwriteFactory_, indexFormat_, idxWide) : 0.0f;
            float idxGap = showIdx ? (skin_ == SkinPreset::GreenTerminal ? 6.0f : 5.0f) : 0.0f;
            float wordWDip = measureTextWidthDip(dwriteFactory_, wordFormat, candWide);
            float itemWDip = itemPadX * 2.0f + idxWDip + idxGap + wordWDip;

            if (curXDip + itemWDip > maxCandRightDip && localIdx > 0) break;

            float pillTop = candTopDip + 3.0f;
            float pillBottom = candBottomDip - 3.0f;
            D2D1_RECT_F itemRc = D2D1::RectF(curXDip, pillTop, curXDip + itemWDip, pillBottom);
            bool isSelected = (static_cast<int>(idx) == selectedIndex_);

            if (isSelected && !pending_) {
                drawSelectionBg(itemRc);
            }

            if (brush && wordFormat) {
                ColorArgb idxColor = pending_
                    ? subText.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                    : (isSelected ? pillText : subText);
                ColorArgb wordColor = pending_
                    ? subText.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                    : (isSelected ? pillText : mainText);

                float textX = curXDip + itemPadX;
                if (showIdx && indexFormat_) {
                    D2D1_RECT_F idxRc = D2D1::RectF(textX, candTopDip, textX + idxWDip + 2.0f, candBottomDip);
                    brush->SetColor(toD2DColor(idxColor));
                    renderTarget_->DrawText(
                        idxWide.c_str(),
                        static_cast<UINT32>(idxWide.size()),
                        indexFormat_,
                        idxRc,
                        brush);
                    textX += idxWDip + idxGap;
                }

                D2D1_RECT_F wordRc = D2D1::RectF(textX, candTopDip, textX + wordWDip + 2.0f, candBottomDip);
                brush->SetColor(toD2DColor(wordColor));
                renderTarget_->DrawText(
                    candWide.c_str(),
                    static_cast<UINT32>(candWide.size()),
                    wordFormat,
                    wordRc,
                    brush);
            }

            RECT hitRc{
                static_cast<LONG>(std::floor(dpToPx(itemRc.left))),
                static_cast<LONG>(std::floor(dpToPx(candTopDip))),
                static_cast<LONG>(std::ceil(dpToPx(itemRc.right))),
                static_cast<LONG>(std::ceil(dpToPx(candBottomDip)))
            };
            candidateRects_.push_back(hitRc);
            candidateIndices_.push_back(idx);
            curXDip += itemWDip + itemGap;
        }

        if (totalPages > 1 && brush && codeFormat_) {
            float pagerRightDip = innerR - padXDip;
            float nextLeftDip = pagerRightDip - pagerBtnWDip;
            float prevLeftDip = nextLeftDip - 4.0f - pagerBtnWDip;

            bool canPrev = (currentPage > 0) && !pending_;
            bool canNext = (currentPage + 1 < totalPages) && !pending_;

            D2D1_RECT_F prevRc = D2D1::RectF(prevLeftDip, candTopDip, prevLeftDip + pagerBtnWDip, candBottomDip);
            D2D1_RECT_F nextRc = D2D1::RectF(nextLeftDip, candTopDip, nextLeftDip + pagerBtnWDip, candBottomDip);

            codeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(toD2DColor(canPrev ? mainText : subText.withAlpha(0.35f)));
            renderTarget_->DrawText(L"‹", 1, codeFormat_, prevRc, brush);

            brush->SetColor(toD2DColor(canNext ? mainText : subText.withAlpha(0.35f)));
            renderTarget_->DrawText(L"›", 1, codeFormat_, nextRc, brush);
            codeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

            if (canPrev) {
                prevPageRect_ = RECT{
                    static_cast<LONG>(std::floor(dpToPx(prevRc.left))),
                    static_cast<LONG>(std::floor(dpToPx(prevRc.top))),
                    static_cast<LONG>(std::ceil(dpToPx(prevRc.right))),
                    static_cast<LONG>(std::ceil(dpToPx(prevRc.bottom)))
                };
            }
            if (canNext) {
                nextPageRect_ = RECT{
                    static_cast<LONG>(std::floor(dpToPx(nextRc.left))),
                    static_cast<LONG>(std::floor(dpToPx(nextRc.top))),
                    static_cast<LONG>(std::ceil(dpToPx(nextRc.right))),
                    static_cast<LONG>(std::ceil(dpToPx(nextRc.bottom)))
                };
            }
        }
    }

    // Outer border for legacy skins (crafted skins already drew their full physical frame)
    if (brush) {
        if (!crafted) {
            brush->SetColor(toD2DColor(borderCol));
            D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f),
                cornerDp,
                cornerDp);
            renderTarget_->DrawRoundedRectangle(borderRect, brush, 1.0f);
        }
        brush->Release();
    }

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        releaseDeviceResources();
    }

    EndPaint(hwnd_, &ps);
}

} // namespace fangcun::tsf
