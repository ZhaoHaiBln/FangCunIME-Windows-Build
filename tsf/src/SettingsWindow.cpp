#include "SettingsWindow.hpp"
#include "DllMain.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <windowsx.h>

namespace fangcun::tsf {

namespace {

constexpr wchar_t kSettingsWndClass[] = L"FangCunSettingsWindowClass";
constexpr wchar_t kUiFontFamily[] = L"Microsoft YaHei UI";

template <typename T>
void safeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
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

} // namespace

UINT SettingsWindow::settingsChangedMessage() {
    static UINT msg = RegisterWindowMessageW(L"FangCunIME_SettingsChanged");
    return msg;
}

void SettingsWindow::broadcastSettingsChanged() {
    UINT msg = settingsChangedMessage();
    if (msg == 0) return;
    HWND target = nullptr;
    while ((target = FindWindowExW(HWND_MESSAGE, target, L"FangCunTsfMessageWindow", nullptr)) != nullptr) {
        PostMessageW(target, msg, 0, 0);
    }
}

SettingsWindow::SettingsWindow(ChangeCallback onChanged)
    : onChanged_(std::move(onChanged)) {}

SettingsWindow::~SettingsWindow() {
    destroy();
}

bool SettingsWindow::ensureFactories() {
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
    if (dwriteFactory_ && !titleFormat_) {
        auto makeFmt = [this](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
            if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                    kUiFontFamily, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"", out)) && *out) {
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        };
        makeFmt(18.0f, DWRITE_FONT_WEIGHT_SEMIBOLD, &titleFormat_);
        makeFmt(13.0f, DWRITE_FONT_WEIGHT_SEMIBOLD, &sectionFormat_);
        makeFmt(13.0f, DWRITE_FONT_WEIGHT_NORMAL, &bodyFormat_);
        makeFmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL, &smallFormat_);
    }
    return true;
}

void SettingsWindow::releaseFactories() {
    safeRelease(titleFormat_);
    safeRelease(sectionFormat_);
    safeRelease(bodyFormat_);
    safeRelease(smallFormat_);
    releaseDeviceResources();
    safeRelease(dwriteFactory_);
    safeRelease(d2dFactory_);
}

bool SettingsWindow::ensureDeviceResources() {
    if (!hwnd_ || !ensureFactories()) return false;
    if (renderTarget_) return true;

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
        static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));

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

void SettingsWindow::releaseDeviceResources() {
    safeRelease(renderTarget_);
}

float SettingsWindow::dpToPx(float dp) const {
    return dp * static_cast<float>(dpi_) / 96.0f;
}

void SettingsWindow::showOrActivate(HWND owner) {
    settings_ = SettingsStore::load();
    glowEngine_.setEnabled(settings_.typingGlow);
    glowEngine_.setMotionAllowed(isClientAreaAnimationEnabled());

    if (hwnd_ && IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    ensureFactories();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SettingsWindow::wndProc;
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kSettingsWndClass;
    RegisterClassExW(&wc);

    // Center on primary / owner monitor
    POINT pt{0, 0};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    dpi_ = 96;
    if (HWND desktop = GetDesktopWindow()) {
        dpi_ = queryDpiForWindow(desktop);
    }

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{0, 0, static_cast<LONG>(dpToPx(560.0f)), static_cast<LONG>(dpToPx(650.0f))};
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    width_ = wr.right - wr.left;
    height_ = wr.bottom - wr.top;

    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width_) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - height_) / 2;

    hwnd_ = CreateWindowExW(
        0,
        kSettingsWndClass,
        L"方寸输入法 设置",
        style,
        x, y, width_, height_,
        owner,
        nullptr,
        g_hInstance,
        this);

    if (hwnd_) {
        dpi_ = queryDpiForWindow(hwnd_);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
}

void SettingsWindow::destroy() {
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

bool SettingsWindow::isOpen() const {
    return hwnd_ != nullptr && IsWindow(hwnd_) != FALSE;
}

void SettingsWindow::syncGlowTimer() {
    if (!hwnd_) return;
    bool needTimer = IsWindowVisible(hwnd_) && glowEngine_.isEnabled() && glowEngine_.isMotionAllowed() && glowEngine_.activeCount() > 0;
    if (needTimer && !glowTimerActive_) {
        SetTimer(hwnd_, kGlowTimerId, kGlowTimerIntervalMs, nullptr);
        glowTimerActive_ = true;
    } else if (!needTimer && glowTimerActive_) {
        KillTimer(hwnd_, kGlowTimerId);
        glowTimerActive_ = false;
    }
}

void SettingsWindow::onGlowTimer() {
    glowEngine_.advanceFrame(monotonicNowNanos());
    syncGlowTimer();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK SettingsWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
                self->onLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            case WM_MOUSEMOVE:
                self->onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            case WM_SIZE:
                if (self->renderTarget_) {
                    self->renderTarget_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_DPICHANGED: {
                self->dpi_ = HIWORD(wp);
                auto* suggested = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (self->renderTarget_) {
                    self->renderTarget_->SetDpi(static_cast<FLOAT>(self->dpi_), static_cast<FLOAT>(self->dpi_));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_DESTROY:
                if (self->glowTimerActive_) {
                    KillTimer(hwnd, kGlowTimerId);
                    self->glowTimerActive_ = false;
                }
                self->glowEngine_.clear();
                self->releaseDeviceResources();
                self->hwnd_ = nullptr;
                return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SettingsWindow::onMouseMove(int x, int y) {
    float xDip = static_cast<float>(x) * 96.0f / static_cast<float>(dpi_);
    float yDip = static_cast<float>(y) * 96.0f / static_cast<float>(dpi_);
    ActionId nextHover = ActionId::None;
    for (const auto& hc : hitControls_) {
        if (xDip >= hc.rectDip.left && xDip <= hc.rectDip.right &&
            yDip >= hc.rectDip.top && yDip <= hc.rectDip.bottom) {
            nextHover = hc.action;
            break;
        }
    }
    if (nextHover != hoveredAction_) {
        hoveredAction_ = nextHover;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void SettingsWindow::onLButtonDown(int x, int y) {
    float xDip = static_cast<float>(x) * 96.0f / static_cast<float>(dpi_);
    float yDip = static_cast<float>(y) * 96.0f / static_cast<float>(dpi_);
    for (const auto& hc : hitControls_) {
        if (xDip >= hc.rectDip.left && xDip <= hc.rectDip.right &&
            yDip >= hc.rectDip.top && yDip <= hc.rectDip.bottom) {
            if (hc.action == ActionId::PreviewGlowTrigger) {
                glowEngine_.setEnabled(settings_.typingGlow);
                glowEngine_.setMotionAllowed(isClientAreaAnimationEnabled());
                glowEngine_.trigger(true, xDip - 16.0f, yDip - 58.0f);
                glowEngine_.advanceFrame(monotonicNowNanos());
                syncGlowTimer();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            applyAction(hc.action);
            return;
        }
    }
}

void SettingsWindow::saveAndNotify() {
    settings_.darkMode = SettingsStore::effectiveDarkMode(settings_);
    SettingsStore::save(settings_);
    broadcastSettingsChanged();
    if (onChanged_) {
        onChanged_(settings_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::applyAction(ActionId action) {
    switch (action) {
        case ActionId::SchemeFlypy: settings_.scheme = InputScheme::Flypy; break;
        case ActionId::SchemePinyin: settings_.scheme = InputScheme::Pinyin; break;
        case ActionId::SchemeNineKey: settings_.scheme = InputScheme::NineKey; break;
        case ActionId::ToggleMixedInput: settings_.mixedInput = !settings_.mixedInput; break;
        case ActionId::ToggleChinesePunct: settings_.chinesePunctuation = !settings_.chinesePunctuation; break;
        case ActionId::SkinNativeLight: settings_.skin = SkinPreset::NativeLight; break;
        case ActionId::SkinWarmGrid: settings_.skin = SkinPreset::WarmGrid; break;
        case ActionId::SkinMint: settings_.skin = SkinPreset::Mint; break;
        case ActionId::SkinDeepBlue: settings_.skin = SkinPreset::DeepBlue; break;
        case ActionId::ModeSystem: settings_.themeMode = ThemeMode::System; break;
        case ActionId::ModeLight: settings_.themeMode = ThemeMode::Light; break;
        case ActionId::ModeDark: settings_.themeMode = ThemeMode::Dark; break;
        case ActionId::ToggleTypingGlow:
            settings_.typingGlow = !settings_.typingGlow;
            glowEngine_.setEnabled(settings_.typingGlow);
            if (settings_.typingGlow) {
                glowEngine_.trigger(false, 0.0f, 0.0f);
                glowEngine_.advanceFrame(monotonicNowNanos());
            }
            syncGlowTimer();
            break;
        case ActionId::ToggleKeyBold: settings_.keyLetterBold = !settings_.keyLetterBold; break;
        case ActionId::FirstSizeMinus:
            settings_.firstCandidateSize = std::clamp(settings_.firstCandidateSize - 1, 16, 30);
            break;
        case ActionId::FirstSizePlus:
            settings_.firstCandidateSize = std::clamp(settings_.firstCandidateSize + 1, 16, 30);
            break;
        case ActionId::LetterSizeMinus:
            settings_.keyLetterSize = std::clamp(settings_.keyLetterSize - 1, 14, 30);
            break;
        case ActionId::LetterSizePlus:
            settings_.keyLetterSize = std::clamp(settings_.keyLetterSize + 1, 14, 30);
            break;
        case ActionId::OpacityMinus:
            settings_.keyOpacity = std::clamp(std::round((settings_.keyOpacity - 0.05f) * 20.0f) / 20.0f, 0.4f, 1.0f);
            break;
        case ActionId::OpacityPlus:
            settings_.keyOpacity = std::clamp(std::round((settings_.keyOpacity + 0.05f) * 20.0f) / 20.0f, 0.4f, 1.0f);
            break;
        case ActionId::ResetDefaults:
            settings_ = AppSettings{};
            glowEngine_.setEnabled(settings_.typingGlow);
            syncGlowTimer();
            break;
        case ActionId::CloseWindow:
            DestroyWindow(hwnd_);
            return;
        default:
            return;
    }
    saveAndNotify();
}

void SettingsWindow::drawTianzigeLogo(float x, float y, float size) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(BrandMark::CreamBackground), &brush)) || !brush) return;

    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + size, y + size), 2.0f, 2.0f), brush);

    float pad = std::max(2.0f, size * 0.16f);
    float stroke = std::max(1.0f, size * 0.06f);
    brush->SetColor(toD2DColor(BrandMark::FrameColor));
    renderTarget_->DrawRectangle(D2D1::RectF(x + pad, y + pad, x + size - pad, y + size - pad), brush, stroke);

    brush->SetColor(toD2DColor(BrandMark::FrameColor.withAlpha(0.45f)));
    float midX = x + size * 0.5f;
    float midY = y + size * 0.5f;
    renderTarget_->DrawLine(D2D1::Point2F(midX, y + pad + stroke), D2D1::Point2F(midX, y + size - pad - stroke), brush, 0.8f);
    renderTarget_->DrawLine(D2D1::Point2F(x + pad + stroke, midY), D2D1::Point2F(x + size - pad - stroke, midY), brush, 0.8f);

    brush->SetColor(toD2DColor(BrandMark::CenterTerracotta));
    renderTarget_->FillRectangle(
        D2D1::RectF(x + size * 0.40f, y + size * 0.40f, x + size * 0.60f, y + size * 0.60f), brush);
    brush->Release();
}

void SettingsWindow::drawGlassButton(
    const D2D1_RECT_F& rect,
    const std::wstring& label,
    SkinPreset skin,
    const FangCunColors& colors,
    bool selected,
    bool functional,
    bool emphasized,
    bool hovered,
    float fontSizeDip,
    bool bold) {
    GlassKeyColors gc = computeGlassKeyColors(
        skin, colors, settings_.keyOpacity, hovered && !selected, functional, emphasized, selected);

    float h = rect.bottom - rect.top;
    float r = gc.cornerRadiusDp;
    float depth = std::min(gc.ledgeDepthDp, h / 5.0f);
    float dep = (hovered && !selected) ? std::min(gc.depressionDp, depth * 0.5f) : 0.0f;
    float faceBottom = rect.bottom - depth + dep;

    D2D1_ROUNDED_RECT outerRR = D2D1::RoundedRect(rect, r, r);
    ID2D1RoundedRectangleGeometry* clipGeo = nullptr;
    if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(outerRR, &clipGeo)) && clipGeo) {
        renderTarget_->PushLayer(D2D1::LayerParameters(rect, clipGeo), nullptr);

        ID2D1SolidColorBrush* solid = nullptr;
        renderTarget_->CreateSolidColorBrush(toD2DColor(gc.ledge), &solid);

        if (dep > 0.0f && solid) {
            renderTarget_->FillRectangle(D2D1::RectF(rect.left, rect.top, rect.right, rect.top + dep), solid);
        }

        // Face vertical gradient
        D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, toD2DColor(gc.top)},
            {1.0f, toD2DColor(gc.bottom)},
        };
        ID2D1GradientStopCollection* stopCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(stops, 2, &stopCol)) && stopCol) {
            ID2D1LinearGradientBrush* grad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(0.0f, rect.top + dep),
                        D2D1::Point2F(0.0f, faceBottom)),
                    stopCol, &grad)) && grad) {
                renderTarget_->FillRectangle(
                    D2D1::RectF(rect.left, rect.top + dep, rect.right, std::max(rect.top + dep, faceBottom)),
                    grad);
                grad->Release();
            }
            stopCol->Release();
        }

        // Ledge band
        if (solid) {
            solid->SetColor(toD2DColor(gc.ledge));
            renderTarget_->FillRectangle(
                D2D1::RectF(rect.left, faceBottom, rect.right, std::max(faceBottom, rect.bottom)), solid);

            // Top highlight
            solid->SetColor(toD2DColor(gc.highlight));
            renderTarget_->DrawLine(
                D2D1::Point2F(rect.left + r, rect.top + dep + gc.borderWidthDp),
                D2D1::Point2F(std::max(rect.left + r, rect.right - r), rect.top + dep + gc.borderWidthDp),
                solid, gc.topHighlightWidthDp);

            // Lower edge
            solid->SetColor(toD2DColor(gc.lowerEdge));
            renderTarget_->DrawLine(
                D2D1::Point2F(rect.left + r, rect.bottom - gc.borderWidthDp),
                D2D1::Point2F(std::max(rect.left + r, rect.right - r), rect.bottom - gc.borderWidthDp),
                solid, gc.lowerEdgeWidthDp);

            // Border
            solid->SetColor(toD2DColor(gc.edge));
            renderTarget_->DrawRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(rect.left + 0.3f, rect.top + 0.3f, rect.right - 0.3f, rect.bottom - 0.3f),
                    r, r),
                solid, gc.borderWidthDp);

            solid->Release();
        }

        renderTarget_->PopLayer();
        clipGeo->Release();
    }

    // Label
    if (dwriteFactory_ && !label.empty()) {
        IDWriteTextFormat* fmt = nullptr;
        if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                kUiFontFamily, nullptr,
                bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                fontSizeDip, L"", &fmt)) && fmt) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            ColorArgb txtCol = emphasized
                ? (colors.isDarkKeyboard() ? ColorArgb{0xFF, 0x18, 0x1C, 0x24} : ColorArgb{0xFF, 0xFF, 0xFF, 0xFF})
                : (selected ? colors.accent : colors.text);

            ID2D1SolidColorBrush* txtBrush = nullptr;
            if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(toD2DColor(txtCol), &txtBrush)) && txtBrush) {
                renderTarget_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), fmt, rect, txtBrush);
                txtBrush->Release();
            }
            fmt->Release();
        }
    }
}

void SettingsWindow::onPaint() {
    PAINTSTRUCT ps = {};
    BeginPaint(hwnd_, &ps);
    if (!ensureDeviceResources()) {
        EndPaint(hwnd_, &ps);
        return;
    }

    const bool dark = SettingsStore::effectiveDarkMode(settings_);
    const FangCunColors colors = skinColors(settings_.skin, dark);
    const SkinMetrics metrics = skinMetrics(settings_.skin);
    const BackdropGradientColors backdrop = computeBackdropGradient(settings_.skin, colors);

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const float wDip = static_cast<float>(rc.right - rc.left) * 96.0f / static_cast<float>(dpi_);
    const float hDip = static_cast<float>(rc.bottom - rc.top) * 96.0f / static_cast<float>(dpi_);

    hitControls_.clear();
    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // Background
    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(toD2DColor(colors.keyboard), &brush);
    if (backdrop.isFlat) {
        if (brush) renderTarget_->FillRectangle(D2D1::RectF(0, 0, wDip, hDip), brush);
    } else {
        D2D1_GRADIENT_STOP stops[3] = {
            {0.0f, toD2DColor(backdrop.top)},
            {0.5f, toD2DColor(backdrop.middle)},
            {1.0f, toD2DColor(backdrop.bottom)},
        };
        ID2D1GradientStopCollection* stopCol = nullptr;
        if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(stops, 3, &stopCol)) && stopCol) {
            ID2D1LinearGradientBrush* grad = nullptr;
            if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, hDip)),
                    stopCol, &grad)) && grad) {
                renderTarget_->FillRectangle(D2D1::RectF(0, 0, wDip, hDip), grad);
                grad->Release();
            }
            stopCol->Release();
        }
    }

    if (!brush) {
        renderTarget_->EndDraw();
        EndPaint(hwnd_, &ps);
        return;
    }

    const float pad = 16.0f;
    float y = 14.0f;

    // Header: Tianzige Logo + Title
    drawTianzigeLogo(pad, y, 28.0f);
    brush->SetColor(toD2DColor(colors.text));
    if (titleFormat_) {
        renderTarget_->DrawText(L"方寸输入法 设置", 8, titleFormat_,
                                D2D1::RectF(pad + 38.0f, y, wDip - pad, y + 28.0f), brush);
    }
    brush->SetColor(toD2DColor(colors.secondary));
    if (smallFormat_) {
        std::wstring sub = L"方寸之间，自有天地  ·  点击预览区可测试流光";
        renderTarget_->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()), smallFormat_,
                                D2D1::RectF(wDip - 260.0f, y + 4.0f, wDip - pad, y + 26.0f), brush);
    }
    y += 42.0f;

    // Live Preview Card (Candidate Bar + 3D Glass Keys + TypingGlow)
    D2D1_RECT_F previewRc = D2D1::RectF(pad, y, wDip - pad, y + 118.0f);
    hitControls_.push_back({previewRc, ActionId::PreviewGlowTrigger});

    D2D1_ROUNDED_RECT previewRR = D2D1::RoundedRect(previewRc, metrics.cornerRadiusDp + 2.0f, metrics.cornerRadiusDp + 2.0f);
    brush->SetColor(toD2DColor(colors.keyboard));
    renderTarget_->FillRoundedRectangle(previewRR, brush);

    // Glow inside preview card
    ID2D1RoundedRectangleGeometry* prevGeo = nullptr;
    if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(previewRR, &prevGeo)) && prevGeo) {
        renderTarget_->PushLayer(D2D1::LayerParameters(previewRc, prevGeo), nullptr);
        auto circles = glowEngine_.buildRenderCircles(
            previewRc.right - previewRc.left, previewRc.bottom - previewRc.top,
            1.0f, colors.isDarkKeyboard(), metrics.glowColors);
        for (const auto& c : circles) {
            D2D1_GRADIENT_STOP gStops[3] = {};
            UINT32 cnt = c.isTrailing ? 2 : 3;
            if (c.isTrailing) {
                gStops[0] = {0.0f, toD2DColor(c.innerColor)};
                gStops[1] = {1.0f, toD2DColor(c.midColor)};
            } else {
                gStops[0] = {0.0f, toD2DColor(c.innerColor)};
                gStops[1] = {0.55f, toD2DColor(c.midColor)};
                gStops[2] = {1.0f, toD2DColor(c.midColor.withAlpha(0.0f))};
            }
            ID2D1GradientStopCollection* sc = nullptr;
            if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(gStops, cnt, &sc)) && sc) {
                ID2D1RadialGradientBrush* rb = nullptr;
                if (SUCCEEDED(renderTarget_->CreateRadialGradientBrush(
                        D2D1::RadialGradientBrushProperties(
                            D2D1::Point2F(previewRc.left + c.cx, previewRc.top + c.cy),
                            D2D1::Point2F(0, 0), c.radius, c.radius),
                        sc, &rb)) && rb) {
                    renderTarget_->FillEllipse(
                        D2D1::Ellipse(D2D1::Point2F(previewRc.left + c.cx, previewRc.top + c.cy), c.radius, c.radius),
                        rb);
                    rb->Release();
                }
                sc->Release();
            }
        }
        renderTarget_->PopLayer();
        prevGeo->Release();
    }

    brush->SetColor(toD2DColor(colors.border.withAlpha(0.75f)));
    renderTarget_->DrawRoundedRectangle(previewRR, brush, 0.8f);

    // Preview candidate bar
    if (smallFormat_) {
        brush->SetColor(toD2DColor(colors.secondary));
        renderTarget_->DrawText(L"wanshang", 8, smallFormat_,
                                D2D1::RectF(previewRc.left + 12.0f, previewRc.top + 4.0f, previewRc.left + 160.0f, previewRc.top + 20.0f),
                                brush);
    }
    // First candidate highlight (accent @ 3.5% alpha + 4dp corner)
    D2D1_RECT_F firstCandRc = D2D1::RectF(previewRc.left + 10.0f, previewRc.top + 22.0f, previewRc.left + 94.0f, previewRc.top + 56.0f);
    brush->SetColor(toD2DColor(colors.accent.withAlpha(HeaderMetricsSpec::kFirstBackgroundAlpha)));
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(firstCandRc, 4.0f, 4.0f), brush);

    if (dwriteFactory_) {
        IDWriteTextFormat* pFmt = nullptr;
        if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                kUiFontFamily, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(settings_.firstCandidateSize), L"", &pFmt)) && pFmt) {
            pFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            brush->SetColor(toD2DColor(colors.accent));
            renderTarget_->DrawText(L"1 晚上", 4, pFmt,
                                    D2D1::RectF(firstCandRc.left + 8.0f, firstCandRc.top, firstCandRc.right + 40.0f, firstCandRc.bottom),
                                    brush);
            pFmt->Release();
        }
        IDWriteTextFormat* oFmt = nullptr;
        if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                kUiFontFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"", &oFmt)) && oFmt) {
            oFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            brush->SetColor(toD2DColor(colors.text));
            renderTarget_->DrawText(L"2 网上   3 晚上好", 11, oFmt,
                                    D2D1::RectF(firstCandRc.right + 14.0f, firstCandRc.top, previewRc.right - 12.0f, firstCandRc.bottom),
                                    brush);
            oFmt->Release();
        }
    }

    // Preview 3D glass keycaps row
    float kx = previewRc.left + 10.0f;
    float ky = previewRc.top + 64.0f;
    float kh = 44.0f;
    drawGlassButton(D2D1::RectF(kx, ky, kx + 48.0f, ky + kh), L"q", settings_.skin, colors,
                    false, false, false, false, static_cast<float>(settings_.keyLetterSize), settings_.keyLetterBold);
    kx += 54.0f;
    drawGlassButton(D2D1::RectF(kx, ky, kx + 48.0f, ky + kh), L"w", settings_.skin, colors,
                    false, false, false, false, static_cast<float>(settings_.keyLetterSize), settings_.keyLetterBold);
    kx += 54.0f;
    drawGlassButton(D2D1::RectF(kx, ky, kx + 48.0f, ky + kh), L"e", settings_.skin, colors,
                    false, false, false, false, static_cast<float>(settings_.keyLetterSize), settings_.keyLetterBold);
    kx += 54.0f;
    drawGlassButton(D2D1::RectF(kx, ky, kx + 150.0f, ky + kh),
                    settings_.mixedInput ? L"中英混输" : schemeLabel(settings_.scheme) == "小鹤" ? L"小鹤双拼" : L"全拼",
                    settings_.skin, colors, false, true, false, false, 13.0f, false);
    kx += 156.0f;
    drawGlassButton(D2D1::RectF(kx, ky, previewRc.right - 10.0f, ky + kh), L"发送", settings_.skin, colors,
                    false, false, true, false, 13.0f, false);

    y += 132.0f;

    auto drawSectionLabel = [&](const std::wstring& title) {
        brush->SetColor(toD2DColor(colors.secondary));
        if (sectionFormat_) {
            renderTarget_->DrawText(title.c_str(), static_cast<UINT32>(title.size()), sectionFormat_,
                                    D2D1::RectF(pad, y, wDip - pad, y + 20.0f), brush);
        }
        y += 24.0f;
    };

    auto addPill = [&](float x, float w, float h, const std::wstring& label, ActionId act, bool sel, bool func = false, bool emph = false) {
        D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
        hitControls_.push_back({r, act});
        drawGlassButton(r, label, settings_.skin, colors, sel, func, emph, hoveredAction_ == act, 13.0f, false);
    };

    const float fullW = wDip - pad * 2.0f;

    // Section 1: Input Scheme & Toggles
    drawSectionLabel(L"输入方案与行为");
    float w3 = (fullW - 12.0f) / 3.0f;
    addPill(pad, w3, 36.0f, L"小鹤双拼", ActionId::SchemeFlypy, settings_.scheme == InputScheme::Flypy);
    addPill(pad + w3 + 6.0f, w3, 36.0f, L"全拼", ActionId::SchemePinyin, settings_.scheme == InputScheme::Pinyin);
    addPill(pad + (w3 + 6.0f) * 2.0f, w3, 36.0f, L"九宫格", ActionId::SchemeNineKey, settings_.scheme == InputScheme::NineKey);
    y += 42.0f;

    float w2 = (fullW - 6.0f) / 2.0f;
    addPill(pad, w2, 34.0f, settings_.mixedInput ? L"✓ 中英混输：开启" : L"中英混输：关闭",
            ActionId::ToggleMixedInput, settings_.mixedInput, true);
    addPill(pad + w2 + 6.0f, w2, 34.0f, settings_.chinesePunctuation ? L"✓ 中文标点：开启" : L"中文标点：关闭",
            ActionId::ToggleChinesePunct, settings_.chinesePunctuation, true);
    y += 46.0f;

    // Section 2: 4 Skins
    drawSectionLabel(L"皮肤预设");
    float w4 = (fullW - 18.0f) / 4.0f;
    addPill(pad, w4, 38.0f, L"原生轻白", ActionId::SkinNativeLight, settings_.skin == SkinPreset::NativeLight);
    addPill(pad + w4 + 6.0f, w4, 38.0f, L"米白田字格", ActionId::SkinWarmGrid, settings_.skin == SkinPreset::WarmGrid);
    addPill(pad + (w4 + 6.0f) * 2.0f, w4, 38.0f, L"薄荷绿", ActionId::SkinMint, settings_.skin == SkinPreset::Mint);
    addPill(pad + (w4 + 6.0f) * 3.0f, w4, 38.0f, L"深蓝珊瑚", ActionId::SkinDeepBlue, settings_.skin == SkinPreset::DeepBlue);
    y += 48.0f;

    // Section 3: Theme Mode & Effects
    drawSectionLabel(L"明暗模式与特效");
    addPill(pad, w3, 34.0f, L"跟随系统", ActionId::ModeSystem, settings_.themeMode == ThemeMode::System);
    addPill(pad + w3 + 6.0f, w3, 34.0f, L"浅色模式", ActionId::ModeLight, settings_.themeMode == ThemeMode::Light);
    addPill(pad + (w3 + 6.0f) * 2.0f, w3, 34.0f, L"深色模式", ActionId::ModeDark, settings_.themeMode == ThemeMode::Dark);
    y += 40.0f;

    addPill(pad, w2, 34.0f, settings_.typingGlow ? L"✓ 打字流光：开启" : L"打字流光：关闭",
            ActionId::ToggleTypingGlow, settings_.typingGlow, true);
    addPill(pad + w2 + 6.0f, w2, 34.0f, settings_.keyLetterBold ? L"✓ 键帽字母：加粗" : L"键帽字母：常规",
            ActionId::ToggleKeyBold, settings_.keyLetterBold, true);
    y += 46.0f;

    // Section 4: Stepper sliders (First Candidate Size, Key Letter Size, Key Opacity)
    drawSectionLabel(L"字号与透明度");
    auto drawStepper = [&](const std::wstring& title, const std::wstring& valueStr, ActionId minusAct, ActionId plusAct) {
        brush->SetColor(toD2DColor(colors.text));
        if (bodyFormat_) {
            renderTarget_->DrawText(title.c_str(), static_cast<UINT32>(title.size()), bodyFormat_,
                                    D2D1::RectF(pad, y, pad + 180.0f, y + 32.0f), brush);
        }
        float rightX = wDip - pad;
        addPill(rightX - 42.0f, 42.0f, 32.0f, L"+", plusAct, false, true);
        brush->SetColor(toD2DColor(colors.accent));
        if (sectionFormat_) {
            IDWriteTextFormat* cFmt = nullptr;
            if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                    kUiFontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMIBOLD, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &cFmt)) && cFmt) {
                cFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                cFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                renderTarget_->DrawText(valueStr.c_str(), static_cast<UINT32>(valueStr.size()), cFmt,
                                        D2D1::RectF(rightX - 108.0f, y, rightX - 46.0f, y + 32.0f), brush);
                cFmt->Release();
            }
        }
        addPill(rightX - 154.0f, 42.0f, 32.0f, L"−", minusAct, false, true);
        y += 38.0f;
    };

    drawStepper(L"第一候选字号 (16–30)", std::to_wstring(settings_.firstCandidateSize) + L" pt",
                ActionId::FirstSizeMinus, ActionId::FirstSizePlus);
    drawStepper(L"键帽字母字号 (14–30)", std::to_wstring(settings_.keyLetterSize) + L" pt",
                ActionId::LetterSizeMinus, ActionId::LetterSizePlus);
    int opacityPct = static_cast<int>(std::lround(settings_.keyOpacity * 100.0f));
    drawStepper(L"键帽透明度 (40%–100%)", std::to_wstring(opacityPct) + L"%",
                ActionId::OpacityMinus, ActionId::OpacityPlus);

    y += 10.0f;
    addPill(pad, 120.0f, 36.0f, L"恢复默认", ActionId::ResetDefaults, false, true);
    addPill(wDip - pad - 120.0f, 120.0f, 36.0f, L"完成", ActionId::CloseWindow, false, false, true);

    brush->Release();

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        releaseDeviceResources();
    }
    EndPaint(hwnd_, &ps);
}

} // namespace fangcun::tsf
