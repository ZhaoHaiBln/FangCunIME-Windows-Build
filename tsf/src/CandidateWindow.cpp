#include "CandidateWindow.hpp"
#include "DllMain.hpp"
#include "SquareWindow.hpp"
#include "fangcun/CandidateCellLayout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fangcun::tsf {

namespace {

constexpr wchar_t kCandidateWndClass[] = L"FangCunCandidateWindowClass";
constexpr wchar_t kUiFontFamily[] = L"Microsoft YaHei UI";
constexpr float kCandidateTrailingPadding = 6.0f;
constexpr float kAuxiliaryCodeWidth = 74.0f;
constexpr float kPagerWidth = 40.0f;

constexpr float kCandidateIndexInset = 6.0f;

float candidateIndexWidth(const SharpIme::WinMetrics& metrics) {
    return metrics.hardShadowOffsetPx > 0 ? 23.0f : 14.0f;
}

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
    wc.style = CS_HREDRAW | CS_VREDRAW;
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
    preferSquareWindow(hwnd_);
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

void CandidateWindow::recomputeHeaderBudget() {
    if (!ensureFactories()) return;
    const bool mono = sharpTheme(skin_).metrics.useMonospaceCode;
    if (lastBudgetSkin_ == skin_ && lastBudgetFirstSize_ == firstCandidateSize_ && lastBudgetDpi_ == dpi_ && primaryFormat_) {
        return;
    }

    safeRelease(codeFormat_);
    safeRelease(primaryFormat_);
    safeRelease(secondaryFormat_);
    safeRelease(indexFormat_);

    auto makeFormat = [this](float sizeDip, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out, const wchar_t* family = kUiFontFamily) {
        const auto create = [&](const wchar_t* name) {
            return dwriteFactory_->CreateTextFormat(name, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizeDip, L"", out);
        };
        if (FAILED(create(family)) && family != kUiFontFamily) create(kUiFontFamily);
        if (*out) {
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };

    const bool serif = skin_ == SkinPreset::WoodType || skin_ == SkinPreset::Typewriter;
    const wchar_t* candidateFont = serif ? L"SimSun" : kUiFontFamily;
    const wchar_t* codeFont = mono ? L"Consolas" : serif ? L"Georgia" : kUiFontFamily;
    makeFormat(static_cast<float>(HeaderMetricsSpec::kCodeSp + 1), DWRITE_FONT_WEIGHT_MEDIUM, &codeFormat_, codeFont);
    makeFormat(static_cast<float>(firstCandidateSize_), DWRITE_FONT_WEIGHT_MEDIUM, &primaryFormat_, candidateFont);
    makeFormat(static_cast<float>(HeaderMetricsSpec::kOtherCandidateSp), DWRITE_FONT_WEIGHT_NORMAL, &secondaryFormat_, candidateFont);
    makeFormat(static_cast<float>(HeaderMetricsSpec::kCandidateIndexSp), DWRITE_FONT_WEIGHT_NORMAL, &indexFormat_, mono ? L"Consolas" : kUiFontFamily);

    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    for (auto* format : {primaryFormat_, secondaryFormat_}) {
        IDWriteInlineObject* ellipsis = nullptr;
        if (format && SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format, &ellipsis))) {
            format->SetTrimming(&trimming, ellipsis);
            safeRelease(ellipsis);
        }
    }

    std::wstring probe = utf8ToWide(HeaderMetricsSpec::kMeasurementProbe);
    float hPrimary = measureTextHeightDip(dwriteFactory_, primaryFormat_, probe);
    float hSecondary = measureTextHeightDip(dwriteFactory_, secondaryFormat_, probe);
    float hIndex = measureTextHeightDip(dwriteFactory_, indexFormat_, L"123456789");
    float hCode = measureTextHeightDip(dwriteFactory_, codeFormat_, L"Agjy0123456789'");

    candidateHeightDp_ = std::max(
        32.0f,
        std::max({hPrimary, hSecondary, hIndex}) + 4.0f);
    codeHeightDp_ = std::max(28.0f, hCode + 6.0f);
    headerHeightDp_ = std::max(
        HeaderMetricsSpec::kHeaderMinimumDp,
        codeHeightDp_ + candidateHeightDp_ + 8.0f);

    lastBudgetSkin_ = skin_;
    lastBudgetFirstSize_ = firstCandidateSize_;
    lastBudgetDpi_ = dpi_;
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
        }
        updateWindowRegion();
        return;
    }

    const auto& style = sharpTheme(skin_).metrics;
    const bool vertical = layout_ == CandidateLayout::Vertical;
    const bool single = layout_ == CandidateLayout::SingleRow;
    const float rowHeight = std::max(layout_ == CandidateLayout::DoubleRow ? 36.0f : 32.0f, candidateHeightDp_);
    size_t start = 0, end = 0, page = 0, pages = 0;
    computePageBounds(start, end, page, pages);

    const auto code = (skin_ == SkinPreset::Brutal ? L"" : L"> ") + utf8ToWide(composing_);
    const float codeWidth = measureTextWidthDip(dwriteFactory_, codeFormat_, code);
    singleCodeWidth_ = std::clamp(codeWidth + 25.0f, 82.0f, 190.0f);
    const float pagerWidth = !vertical && pages > 1 ? kPagerWidth : 0.0f;
    const float candidatePadding = kCandidateIndexInset + candidateIndexWidth(style) + kCandidateTrailingPadding;
    float contentWidth = pagerWidth;
    horizontalCellWidths_.clear();
    if (!vertical) horizontalCellWidths_.reserve(end - start);
    for (size_t index = start; index < end; ++index) {
        const float wordWidth = measureTextWidthDip(dwriteFactory_, index == start ? primaryFormat_ : secondaryFormat_,
            utf8ToWide(candidates_[index]));
        const float itemWidth = std::ceil(wordWidth) + candidatePadding;
        if (vertical) contentWidth = std::max(contentWidth, itemWidth + kAuxiliaryCodeWidth);
        else {
            horizontalCellWidths_.push_back(itemWidth);
            contentWidth += itemWidth;
        }
    }
    const float border = static_cast<float>(style.outerBorderWidthPx);
    const float shadow = static_cast<float>(style.hardShadowOffsetPx);
    float totalHeight = single ? rowHeight : codeHeightDp_ + rowHeight;
    if (vertical) totalHeight = codeHeightDp_ + rowHeight * std::max<size_t>(1, end - start) + 24.0f;
    if (single) contentWidth += singleCodeWidth_;
    else contentWidth = std::max(contentWidth, codeWidth + 64.0f);
    float totalHDp = totalHeight + border * 2.0f + shadow;
    const float fittedWidth = std::clamp(contentWidth, vertical ? 228.0f : 220.0f, vertical ? 440.0f : 760.0f);
    if (single) singleCodeWidth_ = std::min(singleCodeWidth_, fittedWidth - pagerWidth - 70.0f);
    if (!vertical) {
        fitHorizontalCandidateWidths(horizontalCellWidths_, fittedWidth - pagerWidth - (single ? singleCodeWidth_ : 0.0f));
    }
    const float clampedWDp = fittedWidth + border * 2.0f + shadow;

    int newW = static_cast<int>(std::ceil(dpToPx(clampedWDp)));
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
    }
    updateWindowRegion();
}

void CandidateWindow::updateWindowRegion() {
    if (!hwnd_) return;
    const int shadow = toastActive_ ? 0 : scaleForDpi(sharpTheme(skin_).metrics.hardShadowOffsetPx);
    if (shadow > 0 && width_ > shadow && height_ > shadow) {
        HRGN face = CreateRectRgn(0, 0, width_ - shadow, height_ - shadow);
        HRGN offset = CreateRectRgn(shadow, shadow, width_, height_);
        if (face && offset && CombineRgn(face, face, offset, RGN_OR) != ERROR &&
            SetWindowRgn(hwnd_, face, TRUE)) {
            face = nullptr; // Windows owns the region after a successful call.
        }
        if (face) DeleteObject(face);
        if (offset) DeleteObject(offset);
    } else if (skin_ != SkinPreset::Brutal && width_ > 0 && height_ > 0) {
        const int diameter = scaleForDpi(10);
        HRGN face = CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, diameter, diameter);
        if (!face || !SetWindowRgn(hwnd_, face, TRUE)) {
            if (face) DeleteObject(face);
            SetWindowRgn(hwnd_, nullptr, TRUE);
        }
    } else {
        SetWindowRgn(hwnd_, nullptr, TRUE);
    }
    preferSquareWindow(hwnd_);
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
    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(BrandMark::CreamBackground), &brush)) || !brush) return;
    renderTarget_->FillRectangle(D2D1::RectF(x, y, x + size, y + size), brush);
    const float pad = std::max(2.0f, size * 0.16f);
    const float stroke = std::max(1.0f, size * 0.06f);
    brush->SetColor(toD2DColor(BrandMark::FrameColor));
    renderTarget_->DrawRectangle(D2D1::RectF(x + pad, y + pad, x + size - pad, y + size - pad), brush, stroke);
    brush->SetColor(toD2DColor(BrandMark::FrameColor.withAlpha(0.45f)));
    const float midX = x + size * 0.5f;
    const float midY = y + size * 0.5f;
    renderTarget_->DrawLine(D2D1::Point2F(midX, y + pad), D2D1::Point2F(midX, y + size - pad), brush, 0.8f);
    renderTarget_->DrawLine(D2D1::Point2F(x + pad, midY), D2D1::Point2F(x + size - pad, midY), brush, 0.8f);
    brush->SetColor(toD2DColor(BrandMark::CenterTerracotta));
    renderTarget_->FillRectangle(D2D1::RectF(x + size * 0.4f, y + size * 0.4f, x + size * 0.6f, y + size * 0.6f), brush);
    brush->Release();
}

void CandidateWindow::onPaint() {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    if (!ensureDeviceResources()) {
        EndPaint(hwnd_, &ps);
        return;
    }

    const auto& metrics = sharpTheme(skin_).metrics;
    const auto& palette = sharpPalette(skin_, darkMode_);
    const bool vertical = layout_ == CandidateLayout::Vertical;
    const bool single = layout_ == CandidateLayout::SingleRow;
    const float shadow = toastActive_ ? 0.0f : static_cast<float>(metrics.hardShadowOffsetPx);
    const float w = width_ * 96.0f / dpi_ - shadow;
    const float h = height_ * 96.0f / dpi_ - shadow;
    const float edge = static_cast<float>(metrics.outerBorderWidthPx);
    const float left = edge;
    const float right = w - edge;
    const float bottom = h - edge;
    const float headerBottom = single ? bottom : edge + codeHeightDp_;
    const float rowHeight = std::max(layout_ == CandidateLayout::DoubleRow ? 36.0f : 32.0f, candidateHeightDp_);
    const auto rect = [](float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); };
    const auto color = ColorArgb::fromHex;
    const auto chrome = skinColors(skin_, darkMode_);

    candidateRects_.clear();
    candidateIndices_.clear();
    prevPageRect_ = nextPageRect_ = settingsRect_ = RECT{};

    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(toD2DColor(color(palette.windowBg)), &brush);
    if (brush) {
        auto fill = [&](D2D1_RECT_F bounds, uint32_t argb) {
            brush->SetColor(toD2DColor(color(argb)));
            renderTarget_->FillRectangle(bounds, brush);
        };
        auto argb = [](ColorArgb c) -> uint32_t {
            return (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.r) << 16) |
                (static_cast<uint32_t>(c.g) << 8) | c.b;
        };
        auto gradient = [&](D2D1_RECT_F bounds, uint32_t top, uint32_t middle, uint32_t bottomColor) {
            if (bounds.bottom <= bounds.top || bounds.right <= bounds.left) return;
            const D2D1_GRADIENT_STOP stops[] = {
                {0.0f, toD2DColor(color(top))}, {0.5f, toD2DColor(color(middle))},
                {1.0f, toD2DColor(color(bottomColor))},
            };
            ID2D1GradientStopCollection* collection = nullptr;
            if (FAILED(renderTarget_->CreateGradientStopCollection(stops, 3, &collection))) return;
            ID2D1LinearGradientBrush* surface = nullptr;
            const auto axis = D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(bounds.left, bounds.top), D2D1::Point2F(bounds.left, bounds.bottom));
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(axis, collection, &surface))) {
                renderTarget_->FillRectangle(bounds, surface);
                surface->Release();
            }
            collection->Release();
        };
        auto text = [&](const std::wstring& label, IDWriteTextFormat* format, D2D1_RECT_F bounds,
                        uint32_t argb, bool alignRight = false) {
            if (!format || bounds.right <= bounds.left || label.empty()) return;
            brush->SetColor(toD2DColor(color(argb)));
            if (alignRight) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            renderTarget_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), format, bounds,
                brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (alignRight) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        };
        auto hitRect = [&](D2D1_RECT_F r) -> RECT {
            return {std::lround(dpToPx(r.left)), std::lround(dpToPx(r.top)),
                std::lround(dpToPx(r.right)), std::lround(dpToPx(r.bottom))};
        };

        if (shadow > 0) fill(rect(shadow, shadow, w + shadow, h + shadow), palette.hardShadowColor);
        fill(rect(0, 0, w, h), palette.windowOuterBorder);
        if (skin_ == SkinPreset::WoodType) {
            gradient(rect(1, 1, w - 1, h - 1), darkMode_ ? 0xFF665041 : 0xFF9A7356,
                darkMode_ ? 0xFF382820 : 0xFF684A35, darkMode_ ? 0xFF241A16 : 0xFF3F291C);
            for (float line = 1.5f; line < edge; line += 1.15f) {
                fill(rect(1, line, w - 1, line + .35f), 0x5FA98B6F);
                fill(rect(1, h - line - .35f, w - 1, h - line), 0x57342116);
            }
        } else if (skin_ == SkinPreset::Typewriter) {
            gradient(rect(1, 1, w - 1, h - 1), 0xFFDCD1B8, 0xFF716858, 0xFF171614);
            gradient(rect(2.5f, 2.5f, w - 2.5f, h - 2.5f), 0xFF302F2B, 0xFF1C1B18, 0xFF070706);
        } else if (skin_ == SkinPreset::Terminal) {
            gradient(rect(1, 1, w - 1, h - 1), darkMode_ ? 0xFF8E9081 : 0xFFEEE7D3,
                darkMode_ ? 0xFF6B6D61 : 0xFFC5BCA5, darkMode_ ? 0xFF414437 : 0xFF8E846C);
        }
        if (skin_ != SkinPreset::Brutal) fill(rect(edge - 1, edge - 1, right + 1, bottom + 1), 0xB0221D16);
        fill(rect(edge, edge, right, bottom), palette.windowBg);

        if (toastActive_ && composing_.empty()) {
            drawTianzigeLogoD2D(left + 8, (h - 20) * .5f, 20);
            text(toastEnglish_ ? L"EN" : L"中", primaryFormat_, rect(left + 36, edge, right, bottom), palette.textPrimary);
        } else {
            size_t start = 0, end = 0, page = 0, pages = 0;
            computePageBounds(start, end, page, pages);
            const std::wstring pageLabel = std::to_wstring(page + 1) + L"/" + std::to_wstring(std::max<size_t>(1, pages));
            const float codeRight = single ? left + singleCodeWidth_ : right;
            const auto candidateArea = rect(left, edge, right, bottom);
            const auto material = color(palette.windowBg);
            if (skin_ == SkinPreset::Terminal) {
                gradient(candidateArea, argb(ColorArgb::lerp(material, color(0xFF000000), .35f)),
                    palette.windowBg, argb(ColorArgb::lerp(material, color(0xFF000000), .35f)));
                for (float y = edge + .75f; y < bottom; y += 2.5f) {
                    fill(rect(left, y, right, std::min(y + .55f, bottom)), 0x49000000);
                }
            } else if (skin_ != SkinPreset::Brutal) {
                gradient(candidateArea, argb(ColorArgb::lerp(material, color(0xFF4D3020), .08f)),
                    argb(ColorArgb::lerp(material, color(0xFFFFF6DF), darkMode_ ? .035f : .30f)),
                    palette.windowBg);
                for (unsigned i = 1; i <= 150; ++i) {
                    const float x = left + (right - left) * ((i * 83u) % 157u) / 157.0f;
                    const float y = edge + (bottom - edge) * ((i * 47u) % 149u) / 149.0f;
                    fill(rect(x, y, std::min(x + .65f, right), std::min(y + .65f, bottom)), 0x19805C36);
                }
                if (skin_ == SkinPreset::Typewriter) {
                    gradient(rect(4, 2, w - 4, edge), 0xFF918B7C, 0xFF2E2B24, 0xFF0D0C0A);
                    for (const float x : {7.0f, w - 13.0f}) {
                        gradient(rect(x, 1.5f, x + 6, edge), 0xFFE2D7BC, 0xFF827968, 0xFF4B4032);
                    }
                }
            }
            if (skin_ == SkinPreset::Brutal) fill(rect(left, edge, codeRight, headerBottom), palette.windowHeaderBg);

            const auto pulses = glowEngine_.buildRenderCircles(w, h, 1.0f, darkMode_, skinMetrics(skin_).glowColors);
            if (!pulses.empty()) {
                const auto surface = rect(left, edge, right, bottom);
                renderTarget_->PushAxisAlignedClip(surface, D2D1_ANTIALIAS_MODE_ALIASED);
                for (const auto& pulse : pulses) {
                    // Paint under the glyphs and selection, and keep the wood / metal frame clear.
                    const auto center = D2D1::Point2F(pulse.cx, pulse.cy);
                    const D2D1_GRADIENT_STOP stops[] = {
                        {0.0f, toD2DColor(pulse.innerColor.withAlpha(pulse.innerColor.a / 390.0f))},
                        {0.46f, toD2DColor(pulse.midColor.withAlpha(pulse.midColor.a / 390.0f))},
                        {1.0f, toD2DColor(pulse.midColor.withAlpha(0.0f))},
                    };
                    ID2D1GradientStopCollection* colors = nullptr;
                    if (FAILED(renderTarget_->CreateGradientStopCollection(stops, 3, &colors))) continue;
                    ID2D1RadialGradientBrush* glow = nullptr;
                    const auto geometry = D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), pulse.radius, pulse.radius);
                    if (SUCCEEDED(renderTarget_->CreateRadialGradientBrush(geometry, colors, &glow))) {
                        renderTarget_->FillRectangle(surface, glow);
                        glow->Release();
                    }
                    colors->Release();
                }
                renderTarget_->PopAxisAlignedClip();
            }
            if (single) fill(rect(codeRight - 1.5f, edge, codeRight, bottom), palette.accent);
            else fill(rect(left, headerBottom - 1, right, headerBottom), palette.innerDivider);

            const bool terminalLeds = skin_ == SkinPreset::Terminal && !single && w > 275.0f;
            const float ledWidth = terminalLeds ? 28.0f : 0.0f;
            const float labelRight = codeRight - (single ? 9 : 43 + ledWidth);
            const float codeX = left + 9;
            const std::wstring preedit = utf8ToWide(composing_);
            const std::wstring displayCode = skin_ == SkinPreset::Brutal ? preedit : L"> " + preedit;
            const auto codeInk = skin_ == SkinPreset::Terminal ? palette.accent : argb(chrome.text);
            const auto secondaryInk = skin_ == SkinPreset::Terminal ? palette.textSecondary : argb(chrome.secondary);
            text(displayCode, codeFormat_, rect(codeX, edge, labelRight, headerBottom), codeInk);
            const float caret = std::min(labelRight, codeX + measureTextWidthDip(dwriteFactory_, codeFormat_, displayCode) + 2);
            fill(rect(caret, (headerBottom + edge - 16) / 2, caret + 2, (headerBottom + edge + 16) / 2), palette.accent);
            if (!single) text(pageLabel, indexFormat_, rect(labelRight + 5, edge, right - 8 - ledWidth, headerBottom), secondaryInk, true);
            if (terminalLeds) {
                const bool indicators[] = {pending_, !pending_ && candidates_.empty(), !pending_ && !candidates_.empty()};
                for (int i = 0; i < 3; ++i) {
                    brush->SetColor(toD2DColor(color(indicators[i] ? palette.candidateActiveBg : 0xFF475C44)));
                    renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(right - 24 + i * 8, (edge + headerBottom) * .5f), 2.3f, 2.3f), brush);
                }
            }

            const float footerTop = vertical ? bottom - 24 : (single ? edge : headerBottom);
            const float pagerLeft = right - kPagerWidth;
            float x = single ? codeRight : left;
            for (size_t index = start; index < end; ++index) {
                const size_t local = index - start;
                const std::wstring word = utf8ToWide(candidates_[index]);
                auto* format = local == 0 ? primaryFormat_ : secondaryFormat_;
                const float width = vertical ? right - left : horizontalCellWidths_[local];
                const float y = vertical ? headerBottom + local * rowHeight : (single ? edge : headerBottom);
                const auto cell = rect(vertical ? left : x, y, (vertical ? left : x) + width, std::min(bottom, y + rowHeight));
                const bool active = !pending_ && static_cast<int>(index) == selectedIndex_;
                if (active && skin_ == SkinPreset::Brutal) {
                    fill(cell, palette.candidateActiveBg);
                } else if (active && cell.right - cell.left > 7.0f) {
                    const auto inset = rect(cell.left + 2.5f, cell.top + 3.0f,
                        cell.right - 2.5f, cell.bottom - 3.0f);
                    const float radius = metrics.candidateRadiusDp;
                    const auto cap = D2D1::RoundedRect(inset, radius, radius);
                    brush->SetColor(toD2DColor(color(palette.candidateActiveBg)));
                    renderTarget_->FillRoundedRectangle(cap, brush);
                    if (skin_ == SkinPreset::WoodType) {
                        brush->SetColor(toD2DColor(color(palette.candidateActiveText)));
                        renderTarget_->DrawRoundedRectangle(cap, brush, 1.0f);
                    }
                }
                if (vertical) fill(rect(cell.left, cell.bottom - 1, cell.right, cell.bottom), palette.innerDivider);
                std::wstring number = std::to_wstring(local + 1);
                if (skin_ == SkinPreset::Brutal) number = L"[" + number + L"]";
                const float indexX = cell.left + kCandidateIndexInset;
                const float wordX = indexX + candidateIndexWidth(metrics);
                const bool hasCode = vertical && active && !preedit.empty();
                const float wordRight = cell.right - kCandidateTrailingPadding - (hasCode ? kAuxiliaryCodeWidth : 0.0f);
                text(number, indexFormat_, rect(indexX, y, wordX, cell.bottom),
                    active ? palette.candidateActiveText : palette.textSecondary);
                text(word, format, rect(wordX, y, wordRight, cell.bottom),
                    active ? palette.candidateActiveText : pending_ ? palette.textSecondary : palette.textPrimary);
                if (hasCode) text(L"[" + preedit + L"]", indexFormat_,
                    rect(wordRight + 3, y, cell.right - 7, cell.bottom), palette.candidateActiveText, true);
                candidateRects_.push_back(hitRect(cell));
                candidateIndices_.push_back(index);
                x += width;
            }

            if (vertical) {
                fill(rect(left, footerTop, right, bottom), palette.windowHeaderBg);
                text(pageLabel, indexFormat_, rect(left + 9, footerTop, pagerLeft, bottom), secondaryInk);
            }
            if (pages > 1) {
                for (int direction : {-1, 1}) {
                    const bool enabled = !pending_ && (direction < 0 ? page > 0 : page + 1 < pages);
                    const float startX = pagerLeft + (direction > 0 ? 20 : 0);
                    const auto cell = rect(startX, footerTop, startX + 20, bottom);
                    fill(rect(startX, footerTop, startX + 1, bottom), palette.innerDivider);
                    // Geometry of the pack's chevron-left/right icons, centered in the page cell.
                    const float cx = startX + 10, cy = (footerTop + bottom) * .5f;
                    brush->SetColor(toD2DColor(color(enabled ? (vertical ? argb(chrome.text) : palette.textPrimary) : palette.innerDivider)));
                    renderTarget_->DrawLine(D2D1::Point2F(cx - direction * 2, cy - 4), D2D1::Point2F(cx + direction * 2, cy), brush, 1.5f);
                    renderTarget_->DrawLine(D2D1::Point2F(cx + direction * 2, cy), D2D1::Point2F(cx - direction * 2, cy + 4), brush, 1.5f);
                    if (enabled) (direction < 0 ? prevPageRect_ : nextPageRect_) = hitRect(cell);
                }
            }
        }
        brush->Release();
    }

    if (renderTarget_->EndDraw() == D2DERR_RECREATE_TARGET) releaseDeviceResources();
    EndPaint(hwnd_, &ps);
}

} // namespace fangcun::tsf
