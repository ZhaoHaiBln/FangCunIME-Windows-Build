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

CandidateWindow::CandidateWindow(SelectCallback onSelect, SettingsCallback onOpenSettings)
    : onSelect_(std::move(onSelect)), onOpenSettings_(std::move(onOpenSettings)) {}

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

    makeFormat(static_cast<float>(HeaderMetricsSpec::kCodeSp), DWRITE_FONT_WEIGHT_NORMAL, &codeFormat_);
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
    codeHeightDp_ = std::max(14.0f, hCode + 5.0f);
    headerHeightDp_ = std::max(
        HeaderMetricsSpec::kHeaderMinimumDp,
        codeHeightDp_ + candidateHeightDp_ + 6.0f);

    lastBudgetFirstSize_ = firstCandidateSize_;
    lastBudgetDpi_ = dpi_;
}

void CandidateWindow::recalculateSize() {
    if (!hwnd_) return;
    ensureFactories();

    const float padXDp = 10.0f;
    const float padYDp = 4.0f;
    const float itemGapDp = 4.0f;

    float contentWDp = padXDp * 2.0f;
    size_t maxDisplay = std::min<size_t>(candidates_.size(), 9);
    if (maxDisplay == 0) {
        float codeW = measureTextWidthDip(dwriteFactory_, codeFormat_, utf8ToWide(composing_));
        contentWDp = std::max(220.0f, codeW + 96.0f);
    } else {
        for (size_t i = 0; i < maxDisplay; ++i) {
            std::wstring idxStr = std::to_wstring(i + 1);
            std::wstring candStr = utf8ToWide(candidates_[i]);
            float idxW = measureTextWidthDip(dwriteFactory_, indexFormat_, idxStr);
            float txtW = measureTextWidthDip(
                dwriteFactory_,
                (i == 0) ? primaryFormat_ : secondaryFormat_,
                candStr);
            float itemW = HeaderMetricsSpec::kItemHorizontalPaddingDp * 2.0f + idxW + 4.0f + txtW;
            contentWDp += itemW + (i + 1 < maxDisplay ? itemGapDp : 0.0f);
        }
    }

    float totalHDp = headerHeightDp_ + padYDp * 2.0f;
    float clampedWDp = std::clamp(contentWDp, 220.0f, 680.0f);

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
    int cornerPx = std::max(2, static_cast<int>(std::lround(dpToPx(skinMetrics(skin_).cornerRadiusDp))));
    HRGN rgn = CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, cornerPx * 2, cornerPx * 2);
    if (rgn) {
        SetWindowRgn(hwnd_, rgn, TRUE);
    }
}

void CandidateWindow::moveNearCaret(const RECT& caretRect) {
    lastCaretRect_ = caretRect;
    hasCaretRect_ = true;
    if (!hwnd_) return;

    int x = caretRect.left;
    int y = caretRect.bottom + scaleForDpi(6);

    POINT pt{caretRect.left, caretRect.bottom};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        RECT work = mi.rcWork;
        if (x + width_ > work.right) {
            x = std::max<int>(work.left, work.right - width_ - scaleForDpi(8));
        }
        if (x < work.left) x = work.left;
        if (y + height_ > work.bottom) {
            y = std::max<int>(work.top, caretRect.top - height_ - scaleForDpi(6));
        }
        if (y < work.top) y = work.top;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE);
}

void CandidateWindow::show() {
    if (hwnd_ && !IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    syncGlowTimer();
}

void CandidateWindow::hide() {
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
                break;
            case WM_LBUTTONDOWN:
                self->onLButtonDown(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
                return 0;
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
    if (pending_ || !onSelect_) return;
    for (size_t i = 0; i < candidateRects_.size(); ++i) {
        if (PtInRect(&candidateRects_[i], pt)) {
            onSelect_(i);
            return;
        }
    }
}

void CandidateWindow::drawTianzigeLogoD2D(float x, float y, float size) {
    if (!renderTarget_) return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(BrandMark::CreamBackground), &brush)) || !brush) {
        return;
    }

    D2D1_ROUNDED_RECT tile = D2D1::RoundedRect(D2D1::RectF(x, y, x + size, y + size), 2.0f, 2.0f);
    renderTarget_->FillRoundedRectangle(tile, brush);

    float pad = std::max(1.5f, size * 0.16f);
    float stroke = std::max(0.8f, size * 0.06f);
    brush->SetColor(toD2DColor(BrandMark::FrameColor));
    D2D1_RECT_F frameRect = D2D1::RectF(x + pad, y + pad, x + size - pad, y + size - pad);
    renderTarget_->DrawRectangle(frameRect, brush, stroke);

    // Subtle center guides
    brush->SetColor(toD2DColor(BrandMark::FrameColor.withAlpha(0.45f)));
    float midX = x + size * 0.5f;
    float midY = y + size * 0.5f;
    renderTarget_->DrawLine(D2D1::Point2F(midX, y + pad + stroke), D2D1::Point2F(midX, y + size - pad - stroke), brush, 0.7f);
    renderTarget_->DrawLine(D2D1::Point2F(x + pad + stroke, midY), D2D1::Point2F(x + size - pad - stroke, midY), brush, 0.7f);

    // Terracotta center square (40..60 in 100x100)
    brush->SetColor(toD2DColor(BrandMark::CenterTerracotta));
    D2D1_RECT_F centerRect = D2D1::RectF(
        x + size * 0.40f,
        y + size * 0.40f,
        x + size * 0.60f,
        y + size * 0.60f);
    renderTarget_->FillRectangle(centerRect, brush);
    brush->Release();
}

void CandidateWindow::onPaint() {
    PAINTSTRUCT ps = {};
    BeginPaint(hwnd_, &ps);

    if (!ensureDeviceResources()) {
        EndPaint(hwnd_, &ps);
        return;
    }

    const FangCunColors colors = skinColors(skin_, darkMode_);
    const SkinMetrics metrics = skinMetrics(skin_);
    const BackdropGradientColors backdrop = computeBackdropGradient(skin_, colors);

    const float wDip = static_cast<float>(width_) * 96.0f / static_cast<float>(dpi_);
    const float hDip = static_cast<float>(height_) * 96.0f / static_cast<float>(dpi_);

    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, wDip, hDip);
    D2D1_ROUNDED_RECT roundedBounds = D2D1::RoundedRect(fullRect, metrics.cornerRadiusDp, metrics.cornerRadiusDp);

    // 1. Static keyboard backdrop (flat for WarmGrid, 3-stop vertical gradient for others)
    if (backdrop.isFlat) {
        ID2D1SolidColorBrush* bgBrush = nullptr;
        if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(toD2DColor(backdrop.middle), &bgBrush)) && bgBrush) {
            renderTarget_->FillRoundedRectangle(roundedBounds, bgBrush);
            bgBrush->Release();
        }
    } else {
        D2D1_GRADIENT_STOP stops[3] = {
            {0.0f, toD2DColor(backdrop.top)},
            {0.5f, toD2DColor(backdrop.middle)},
            {1.0f, toD2DColor(backdrop.bottom)},
        };
        ID2D1GradientStopCollection* stopCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(stops, 3, &stopCol)) && stopCol) {
            ID2D1LinearGradientBrush* gradBrush = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, hDip)),
                    stopCol,
                    &gradBrush)) && gradBrush) {
                renderTarget_->FillRoundedRectangle(roundedBounds, gradBrush);
                gradBrush->Release();
            }
            stopCol->Release();
        }
    }

    // 2. TypingGlow layer behind header & candidates
    auto glowCircles = glowEngine_.buildRenderCircles(
        wDip, hDip, 1.0f, colors.isDarkKeyboard(), metrics.glowColors);
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

    ID2D1SolidColorBrush* textBrush = nullptr;
    renderTarget_->CreateSolidColorBrush(toD2DColor(colors.text), &textBrush);

    // 3. Top row: composing code on left, Tianzige logo + scheme badge on right
    const float padXDip = 10.0f;
    const float padYDip = 4.0f;
    const float logoSizeDip = 14.0f;

    std::wstring schemeWide = utf8ToWide(schemeLabel(scheme_));
    float schemeWDip = measureTextWidthDip(dwriteFactory_, codeFormat_, schemeWide);

    if (textBrush && codeFormat_) {
        std::wstring compWide = utf8ToWide(composing_);
        float rightReserved = logoSizeDip + 6.0f + schemeWDip + 8.0f;
        D2D1_RECT_F compRc = D2D1::RectF(
            padXDip,
            padYDip,
            std::max(padXDip + 20.0f, wDip - padXDip - rightReserved),
            padYDip + codeHeightDp_);
        textBrush->SetColor(toD2DColor(colors.secondary));
        renderTarget_->DrawText(
            compWide.c_str(),
            static_cast<UINT32>(compWide.size()),
            codeFormat_,
            compRc,
            textBrush);

        float badgeX = wDip - padXDip - schemeWDip;
        D2D1_RECT_F schemeRc = D2D1::RectF(badgeX, padYDip, wDip - padXDip, padYDip + codeHeightDp_);
        renderTarget_->DrawText(
            schemeWide.c_str(),
            static_cast<UINT32>(schemeWide.size()),
            codeFormat_,
            schemeRc,
            textBrush);

        float logoX = badgeX - logoSizeDip - 5.0f;
        float logoY = padYDip + (codeHeightDp_ - logoSizeDip) * 0.5f;
        drawTianzigeLogoD2D(logoX, logoY, logoSizeDip);

        settingsRect_ = RECT{
            static_cast<LONG>(std::floor(dpToPx(logoX - 4.0f))),
            static_cast<LONG>(std::floor(dpToPx(padYDip))),
            static_cast<LONG>(std::ceil(dpToPx(wDip - padXDip + 4.0f))),
            static_cast<LONG>(std::ceil(dpToPx(padYDip + codeHeightDp_ + 2.0f)))
        };
    }

    // 4. Candidate row (stable candidateHeightDp_ slot)
    candidateRects_.clear();
    float curXDip = padXDip;
    float candTopDip = padYDip + codeHeightDp_ + 2.0f;
    float candBottomDip = candTopDip + candidateHeightDp_;
    size_t maxDisplay = std::min<size_t>(candidates_.size(), 9);

    for (size_t i = 0; i < maxDisplay; ++i) {
        std::wstring idxWide = std::to_wstring(i + 1);
        std::wstring candWide = utf8ToWide(candidates_[i]);
        IDWriteTextFormat* wordFormat = (i == 0) ? primaryFormat_ : secondaryFormat_;

        float idxWDip = measureTextWidthDip(dwriteFactory_, indexFormat_, idxWide);
        float wordWDip = measureTextWidthDip(dwriteFactory_, wordFormat, candWide);
        float itemWDip = HeaderMetricsSpec::kItemHorizontalPaddingDp * 2.0f + idxWDip + 4.0f + wordWDip;

        if (curXDip + itemWDip > wDip - padXDip && i > 0) break;

        D2D1_RECT_F itemRc = D2D1::RectF(curXDip, candTopDip, curXDip + itemWDip, candBottomDip);

        // First candidate: accent text + accent @ 3.5% alpha background + 4dp corner (NOT solid fill!)
        if (i == 0 && !pending_ && textBrush) {
            D2D1_ROUNDED_RECT firstBg = D2D1::RoundedRect(
                itemRc,
                HeaderMetricsSpec::kFirstCornerDp,
                HeaderMetricsSpec::kFirstCornerDp);
            textBrush->SetColor(toD2DColor(colors.accent.withAlpha(HeaderMetricsSpec::kFirstBackgroundAlpha)));
            renderTarget_->FillRoundedRectangle(firstBg, textBrush);
        } else if (static_cast<int>(i) == selectedIndex_ && i > 0 && !pending_ && textBrush) {
            D2D1_ROUNDED_RECT selBg = D2D1::RoundedRect(
                itemRc,
                HeaderMetricsSpec::kFirstCornerDp,
                HeaderMetricsSpec::kFirstCornerDp);
            textBrush->SetColor(toD2DColor(colors.pressed.withAlpha(0.55f)));
            renderTarget_->FillRoundedRectangle(selBg, textBrush);
        }

        if (textBrush && indexFormat_ && wordFormat) {
            ColorArgb idxColor = pending_
                ? colors.secondary.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                : ((i == 0) ? colors.accent : colors.secondary);
            ColorArgb wordColor = pending_
                ? colors.secondary.withAlpha(HeaderMetricsSpec::kPendingAlpha)
                : ((i == 0) ? colors.accent : colors.text);

            float textX = curXDip + HeaderMetricsSpec::kItemHorizontalPaddingDp;
            D2D1_RECT_F idxRc = D2D1::RectF(textX, candTopDip, textX + idxWDip + 2.0f, candBottomDip);
            textBrush->SetColor(toD2DColor(idxColor));
            renderTarget_->DrawText(
                idxWide.c_str(),
                static_cast<UINT32>(idxWide.size()),
                indexFormat_,
                idxRc,
                textBrush);

            float wordX = textX + idxWDip + 4.0f;
            D2D1_RECT_F wordRc = D2D1::RectF(wordX, candTopDip, wordX + wordWDip + 2.0f, candBottomDip);
            textBrush->SetColor(toD2DColor(wordColor));
            renderTarget_->DrawText(
                candWide.c_str(),
                static_cast<UINT32>(candWide.size()),
                wordFormat,
                wordRc,
                textBrush);
        }

        RECT hitRc{
            static_cast<LONG>(std::floor(dpToPx(itemRc.left))),
            static_cast<LONG>(std::floor(dpToPx(itemRc.top))),
            static_cast<LONG>(std::ceil(dpToPx(itemRc.right))),
            static_cast<LONG>(std::ceil(dpToPx(itemRc.bottom)))
        };
        candidateRects_.push_back(hitRc);
        curXDip += itemWDip + 4.0f;
    }

    // 5. Outer border (0.6dp stroke)
    if (textBrush) {
        textBrush->SetColor(toD2DColor(colors.border.withAlpha(skin_ == SkinPreset::WarmGrid ? 0.8f : 0.65f)));
        D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, wDip - 0.5f, hDip - 0.5f),
            metrics.cornerRadiusDp,
            metrics.cornerRadiusDp);
        renderTarget_->DrawRoundedRectangle(borderRect, textBrush, 0.8f);
        textBrush->Release();
    }

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        releaseDeviceResources();
    }

    EndPaint(hwnd_, &ps);
}

} // namespace fangcun::tsf
