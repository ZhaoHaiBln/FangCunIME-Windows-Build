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

void CandidateWindow::recomputeHeaderBudget() {
    if (!ensureFactories()) return;
    if (lastBudgetFirstSize_ == firstCandidateSize_ && lastBudgetDpi_ == dpi_ && primaryFormat_) {
        return;
    }

    safeRelease(codeFormat_);
    safeRelease(primaryFormat_);
    safeRelease(secondaryFormat_);
    safeRelease(indexFormat_);

    auto makeFormat = [this](float sizeDip, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                kUiFontFamily,
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

    makeFormat(static_cast<float>(HeaderMetricsSpec::kCodeSp + 1), DWRITE_FONT_WEIGHT_MEDIUM, &codeFormat_);
    makeFormat(static_cast<float>(firstCandidateSize_), DWRITE_FONT_WEIGHT_MEDIUM, &primaryFormat_);
    makeFormat(static_cast<float>(HeaderMetricsSpec::kOtherCandidateSp), DWRITE_FONT_WEIGHT_NORMAL, &secondaryFormat_);
    makeFormat(static_cast<float>(HeaderMetricsSpec::kCandidateIndexSp), DWRITE_FONT_WEIGHT_NORMAL, &indexFormat_);

    std::wstring probe = utf8ToWide(HeaderMetricsSpec::kMeasurementProbe);
    float hPrimary = measureTextHeightDip(dwriteFactory_, primaryFormat_, probe);
    float hSecondary = measureTextHeightDip(dwriteFactory_, secondaryFormat_, probe);
    float hIndex = measureTextHeightDip(dwriteFactory_, indexFormat_, L"123456789");
    float hCode = measureTextHeightDip(dwriteFactory_, codeFormat_, L"Agjy0123456789'");

    candidateHeightDp_ = std::max(
        HeaderMetricsSpec::kCandidateSlotMinimumDp,
        std::max({hPrimary, hSecondary, hIndex}) + 12.0f);
    codeHeightDp_ = std::max(20.0f, hCode + 6.0f);
    headerHeightDp_ = std::max(
        HeaderMetricsSpec::kHeaderMinimumDp,
        codeHeightDp_ + candidateHeightDp_ + 8.0f);

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
            updateWindowRegion();
        }
        return;
    }

    const float padXDp = 12.0f;
    const float padYDp = 5.0f;
    const float itemGapDp = 6.0f;

    size_t pageStart = 0;
    size_t pageEnd = 0;
    size_t currentPage = 0;
    size_t totalPages = 0;
    computePageBounds(pageStart, pageEnd, currentPage, totalPages);

    float codeW = measureTextWidthDip(dwriteFactory_, codeFormat_, utf8ToWide(composing_));
    float topRowWDp = padXDp * 2.0f + 24.0f + codeW + 96.0f;

    float contentWDp = padXDp * 2.0f;
    if (pageEnd <= pageStart) {
        contentWDp = std::max(220.0f, topRowWDp);
    } else {
        for (size_t idx = pageStart; idx < pageEnd; ++idx) {
            size_t localIdx = idx - pageStart;
            std::wstring idxStr = std::to_wstring(localIdx + 1);
            std::wstring candStr = utf8ToWide(candidates_[idx]);
            float idxW = measureTextWidthDip(dwriteFactory_, indexFormat_, idxStr);
            float txtW = measureTextWidthDip(
                dwriteFactory_,
                (localIdx == 0) ? primaryFormat_ : secondaryFormat_,
                candStr);
            float itemW = HeaderMetricsSpec::kItemHorizontalPaddingDp * 2.0f + idxW + 5.0f + txtW;
            contentWDp += itemW + (idx + 1 < pageEnd ? itemGapDp : 0.0f);
        }
        if (totalPages > 1) {
            std::wstring pageLabel = std::to_wstring(currentPage + 1) + L"/" + std::to_wstring(totalPages);
            float labelW = measureTextWidthDip(dwriteFactory_, indexFormat_, pageLabel);
            float pagerW = 8.0f + 16.0f + 2.0f + labelW + 2.0f + 16.0f;
            contentWDp += pagerW;
        }
        contentWDp = std::max(contentWDp, topRowWDp);
    }

    float totalHDp = headerHeightDp_ + padYDp * 2.0f;
    float clampedWDp = std::clamp(contentWDp, 220.0f, 760.0f);

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
        updateWindowRegion();
    }
}

void CandidateWindow::updateWindowRegion() {
    if (!hwnd_) return;
    int cornerPx = std::max(6, static_cast<int>(std::lround(dpToPx(8.0f))));
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

void CandidateWindow::onPaint() {
    PAINTSTRUCT ps = {};
    BeginPaint(hwnd_, &ps);

    if (!ensureDeviceResources()) {
        EndPaint(hwnd_, &ps);
        return;
    }

    const SkinMetrics metrics = skinMetrics(skin_);

    const float wDip = static_cast<float>(width_) * 96.0f / static_cast<float>(dpi_);
    const float hDip = static_cast<float>(height_) * 96.0f / static_cast<float>(dpi_);
    const float cornerDp = 8.0f;

    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, wDip, hDip);
    D2D1_ROUNDED_RECT roundedBounds = D2D1::RoundedRect(fullRect, cornerDp, cornerDp);

    ColorArgb topBg = darkMode_ ? ColorArgb::fromHex(0xFF1F2633) : ColorArgb::fromHex(0xFFF6F1E6);
    ColorArgb bottomBandBg = darkMode_ ? ColorArgb::fromHex(0xFF19202B) : ColorArgb::fromHex(0xFFEFE8D9);
    ColorArgb pillAccent = darkMode_ ? ColorArgb::fromHex(0xFFE07A5F) : ColorArgb::fromHex(0xFF2B5B3C);
    ColorArgb pillText = darkMode_ ? ColorArgb::fromHex(0xFF1B222E) : ColorArgb::fromHex(0xFFFFFFFF);
    ColorArgb mainText = darkMode_ ? ColorArgb::fromHex(0xFFE88D73) : ColorArgb::fromHex(0xFF23201C);
    ColorArgb subText = darkMode_ ? ColorArgb::fromHex(0xFFB48372) : ColorArgb::fromHex(0xFF6E6659);
    ColorArgb borderCol = darkMode_ ? ColorArgb::fromHex(0xFF333D4F) : ColorArgb::fromHex(0xFFDDD4C0);

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
        mainText = ColorArgb::fromHex(0xFFEEF3FC);
        subText = ColorArgb::fromHex(0xFF94A6C8);
        borderCol = ColorArgb::fromHex(0xFF2C3F63);
    }

    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(toD2DColor(topBg), &brush);
    if (brush) {
        renderTarget_->FillRoundedRectangle(roundedBounds, brush);
    }

    // Mode-switch floating toast (Shift toggle)
    if (toastActive_ && composing_.empty() && brush) {
        float logoSize = 20.0f;
        float logoX = 14.0f;
        float logoY = (hDip - logoSize) * 0.5f;
        drawTianzigeLogoD2D(logoX, logoY, logoSize);

        if (primaryFormat_) {
            std::wstring modeStr = toastEnglish_ ? L"英" : L"中";
            D2D1_RECT_F txtRc = D2D1::RectF(logoX + logoSize + 10.0f, 0.0f, wDip - 10.0f, hDip);
            brush->SetColor(toD2DColor(mainText));
            renderTarget_->DrawText(modeStr.c_str(), static_cast<UINT32>(modeStr.size()), primaryFormat_, txtRc, brush);
        }

        brush->SetColor(toD2DColor(borderCol));
        D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f), cornerDp, cornerDp);
        renderTarget_->DrawRoundedRectangle(borderRect, brush, 1.0f);
        brush->Release();

        HRESULT hr = renderTarget_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) releaseDeviceResources();
        EndPaint(hwnd_, &ps);
        return;
    }

    const float padXDip = 12.0f;
    const float padYDip = 5.0f;
    float candTopDip = padYDip + codeHeightDp_ + 3.0f;
    float candBottomDip = candTopDip + candidateHeightDp_;

    // Subtle two-tone bottom strip for candidates
    if (brush) {
        brush->SetColor(toD2DColor(bottomBandBg));
        D2D1_RECT_F bandRect = D2D1::RectF(0.0f, candTopDip - 1.0f, wDip, hDip);
        renderTarget_->FillRectangle(bandRect, brush);
    }

    // TypingGlow layer
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

    // Top row: Tianzige icon on left + composing text + cursor bar |
    const float logoSizeDip = 16.0f;
    float logoX = padXDip;
    float logoY = padYDip + (codeHeightDp_ - logoSizeDip) * 0.5f;
    drawTianzigeLogoD2D(logoX, logoY, logoSizeDip);

    settingsRect_ = RECT{
        static_cast<LONG>(std::floor(dpToPx(logoX - 4.0f))),
        static_cast<LONG>(std::floor(dpToPx(padYDip))),
        static_cast<LONG>(std::ceil(dpToPx(logoX + logoSizeDip + 4.0f))),
        static_cast<LONG>(std::ceil(dpToPx(padYDip + codeHeightDp_ + 2.0f)))
    };

    if (brush && codeFormat_) {
        std::wstring compWide = utf8ToWide(composing_);
        float codeX = logoX + logoSizeDip + 7.0f;
        float compW = measureTextWidthDip(dwriteFactory_, codeFormat_, compWide);
        D2D1_RECT_F compRc = D2D1::RectF(
            codeX,
            padYDip,
            std::max(codeX + 20.0f, wDip - padXDip),
            padYDip + codeHeightDp_);
        brush->SetColor(toD2DColor(mainText));
        renderTarget_->DrawText(
            compWide.c_str(),
            static_cast<UINT32>(compWide.size()),
            codeFormat_,
            compRc,
            brush);

        // Vertical caret bar at end of composing text
        float caretX = std::min(wDip - padXDip - 4.0f, codeX + compW + 2.0f);
        float caretTop = padYDip + 3.0f;
        float caretBot = padYDip + codeHeightDp_ - 3.0f;
        brush->SetColor(toD2DColor(mainText.withAlpha(0.75f)));
        renderTarget_->DrawLine(
            D2D1::Point2F(caretX, caretTop),
            D2D1::Point2F(caretX, caretBot),
            brush,
            1.4f);
    }

    // Candidate row
    candidateRects_.clear();
    candidateIndices_.clear();
    prevPageRect_ = RECT{0, 0, 0, 0};
    nextPageRect_ = RECT{0, 0, 0, 0};

    float curXDip = padXDip;
    size_t pageStart = 0;
    size_t pageEnd = 0;
    size_t currentPage = 0;
    size_t totalPages = 0;
    computePageBounds(pageStart, pageEnd, currentPage, totalPages);

    float pagerTotalDip = 0.0f;
    const float pagerBtnWDip = 16.0f;
    if (totalPages > 1) {
        pagerTotalDip = 8.0f + pagerBtnWDip + 4.0f + pagerBtnWDip;
    }

    float maxCandRightDip = wDip - padXDip - pagerTotalDip;

    for (size_t idx = pageStart; idx < pageEnd; ++idx) {
        size_t localIdx = idx - pageStart;
        std::wstring idxWide = std::to_wstring(localIdx + 1);
        std::wstring candWide = utf8ToWide(candidates_[idx]);
        IDWriteTextFormat* wordFormat = (localIdx == 0) ? primaryFormat_ : secondaryFormat_;

        float idxWDip = measureTextWidthDip(dwriteFactory_, indexFormat_, idxWide);
        float wordWDip = measureTextWidthDip(dwriteFactory_, wordFormat, candWide);
        float itemWDip = HeaderMetricsSpec::kItemHorizontalPaddingDp * 2.0f + idxWDip + 5.0f + wordWDip;

        if (curXDip + itemWDip > maxCandRightDip && localIdx > 0) break;

        float pillTop = candTopDip + 3.0f;
        float pillBottom = candBottomDip - 3.0f;
        D2D1_RECT_F itemRc = D2D1::RectF(curXDip, pillTop, curXDip + itemWDip, pillBottom);
        bool isSelected = (static_cast<int>(idx) == selectedIndex_);

        if (isSelected && !pending_ && brush) {
            D2D1_ROUNDED_RECT selPill = D2D1::RoundedRect(itemRc, 5.5f, 5.5f);
            brush->SetColor(toD2DColor(pillAccent));
            renderTarget_->FillRoundedRectangle(selPill, brush);
        }

        if (brush && indexFormat_ && wordFormat) {
            ColorArgb idxColor = pending_
                ? subText.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                : (isSelected ? pillText.withAlpha(0.92f) : subText);
            ColorArgb wordColor = pending_
                ? subText.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                : (isSelected ? pillText : mainText);

            float textX = curXDip + HeaderMetricsSpec::kItemHorizontalPaddingDp;
            D2D1_RECT_F idxRc = D2D1::RectF(textX, candTopDip, textX + idxWDip + 2.0f, candBottomDip);
            brush->SetColor(toD2DColor(idxColor));
            renderTarget_->DrawText(
                idxWide.c_str(),
                static_cast<UINT32>(idxWide.size()),
                indexFormat_,
                idxRc,
                brush);

            float wordX = textX + idxWDip + 5.0f;
            D2D1_RECT_F wordRc = D2D1::RectF(wordX, candTopDip, wordX + wordWDip + 2.0f, candBottomDip);
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
        curXDip += itemWDip + 6.0f;
    }

    if (totalPages > 1 && brush && codeFormat_) {
        float pagerRightDip = wDip - padXDip;
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

    // Outer border
    if (brush) {
        brush->SetColor(toD2DColor(borderCol));
        D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f),
            cornerDp,
            cornerDp);
        renderTarget_->DrawRoundedRectangle(borderRect, brush, 1.0f);
        brush->Release();
    }

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        releaseDeviceResources();
    }

    EndPaint(hwnd_, &ps);
}

} // namespace fangcun::tsf
