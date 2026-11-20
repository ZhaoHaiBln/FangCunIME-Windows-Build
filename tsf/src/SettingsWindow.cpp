#include "SettingsWindow.hpp"
#include "DllMain.hpp"
#include "SquareWindow.hpp"

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

uint32_t packArgb(ColorArgb c) {
    return (static_cast<uint32_t>(c.a) << 24) |
           (static_cast<uint32_t>(c.r) << 16) |
           (static_cast<uint32_t>(c.g) << 8) |
           static_cast<uint32_t>(c.b);
}

uint32_t blendOver(ColorArgb fg, ColorArgb bg) {
    float a = static_cast<float>(fg.a) / 255.0f;
    float inv = 1.0f - a;
    uint8_t r = static_cast<uint8_t>(std::lround(fg.r * a + bg.r * inv));
    uint8_t g = static_cast<uint8_t>(std::lround(fg.g * a + bg.g * inv));
    uint8_t b = static_cast<uint8_t>(std::lround(fg.b * a + bg.b * inv));
    return (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

HICON buildTianzigeIcon(int sizePx) {
    if (sizePx <= 0) return nullptr;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sizePx;
    bi.bV5Height = -sizePx;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    if (screenDc) ReleaseDC(nullptr, screenDc);
    if (!colorBmp || !bits) {
        if (colorBmp) DeleteObject(colorBmp);
        return nullptr;
    }

    auto* pixels = static_cast<uint32_t*>(bits);
    const uint32_t cream = packArgb(BrandMark::CreamBackground);
    const uint32_t frame = packArgb(BrandMark::FrameColor);
    const uint32_t guide = blendOver(BrandMark::FrameColor.withAlpha(0.45f), BrandMark::CreamBackground);
    const uint32_t center = packArgb(BrandMark::CenterTerracotta);

    const int radius = std::max(2, sizePx / 6);
    const int pad = std::max(2, sizePx * 16 / 100);
    const int stroke = std::max(1, sizePx * 8 / 100);
    const int mid = sizePx / 2;
    const int cMin = sizePx * 38 / 100;
    const int cMax = sizePx * 62 / 100;

    for (int y = 0; y < sizePx; ++y) {
        for (int x = 0; x < sizePx; ++x) {
            bool inside = true;
            int dx = 0;
            int dy = 0;
            if (x < radius && y < radius) {
                dx = radius - x - 1;
                dy = radius - y - 1;
                inside = (dx * dx + dy * dy) <= (radius * radius);
            } else if (x >= sizePx - radius && y < radius) {
                dx = x - (sizePx - radius);
                dy = radius - y - 1;
                inside = (dx * dx + dy * dy) <= (radius * radius);
            } else if (x < radius && y >= sizePx - radius) {
                dx = radius - x - 1;
                dy = y - (sizePx - radius);
                inside = (dx * dx + dy * dy) <= (radius * radius);
            } else if (x >= sizePx - radius && y >= sizePx - radius) {
                dx = x - (sizePx - radius);
                dy = y - (sizePx - radius);
                inside = (dx * dx + dy * dy) <= (radius * radius);
            }
            if (!inside) {
                pixels[y * sizePx + x] = 0x00000000;
                continue;
            }

            uint32_t px = cream;
            bool inFrameBox = (x >= pad && x < sizePx - pad && y >= pad && y < sizePx - pad);
            if (inFrameBox) {
                bool onBorder = (x < pad + stroke || x >= sizePx - pad - stroke ||
                                 y < pad + stroke || y >= sizePx - pad - stroke);
                if (onBorder) {
                    px = frame;
                } else if (x >= cMin && x < cMax && y >= cMin && y < cMax) {
                    px = center;
                } else if (x == mid || y == mid) {
                    px = guide;
                }
            }
            pixels[y * sizePx + x] = px;
        }
    }

    HBITMAP maskBmp = CreateBitmap(sizePx, sizePx, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);

    if (maskBmp) DeleteObject(maskBmp);
    DeleteObject(colorBmp);
    return icon;
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
        auto makeFmt = [this](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
            if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                    family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"", out)) && *out) {
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        };
        makeFmt(kUiFontFamily, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &titleFormat_);
        makeFmt(kUiFontFamily, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &sectionFormat_);
        makeFmt(kUiFontFamily, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, &bodyFormat_);
        makeFmt(kUiFontFamily, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, &smallFormat_);
        makeFmt(L"Consolas", 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &monoFormat_);
    }
    return true;
}

void SettingsWindow::releaseFactories() {
    safeRelease(titleFormat_);
    safeRelease(sectionFormat_);
    safeRelease(bodyFormat_);
    safeRelease(smallFormat_);
    safeRelease(monoFormat_);
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
    RECT wr{0, 0, static_cast<LONG>(dpToPx(820.0f)), static_cast<LONG>(dpToPx(640.0f))};
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
        preferSquareWindow(hwnd_);
        dpi_ = queryDpiForWindow(hwnd_);
        createTianzigeIcons();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
}

void SettingsWindow::createTianzigeIcons() {
    destroyTianzigeIcons();
    int smallPx = std::max(16, static_cast<int>(std::lround(dpToPx(16.0f))));
    int bigPx = std::max(32, static_cast<int>(std::lround(dpToPx(32.0f))));
    smallIcon_ = buildTianzigeIcon(smallPx);
    bigIcon_ = buildTianzigeIcon(bigPx);
    if (hwnd_) {
        if (smallIcon_) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon_));
        if (bigIcon_) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon_));
    }
}

void SettingsWindow::destroyTianzigeIcons() {
    if (smallIcon_) {
        DestroyIcon(smallIcon_);
        smallIcon_ = nullptr;
    }
    if (bigIcon_) {
        DestroyIcon(bigIcon_);
        bigIcon_ = nullptr;
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
    destroyTianzigeIcons();
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
                glowEngine_.trigger(true, xDip - hc.rectDip.left, yDip - hc.rectDip.top);
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
        case ActionId::TabAll:
            activeTab_ = CategoryTab::All;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case ActionId::TabInput:
            activeTab_ = CategoryTab::Input;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case ActionId::TabTheme:
            activeTab_ = CategoryTab::Theme;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case ActionId::TabTypography:
            activeTab_ = CategoryTab::Typography;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case ActionId::SchemeFlypy: settings_.scheme = InputScheme::Flypy; break;
        case ActionId::SchemePinyin: settings_.scheme = InputScheme::Pinyin; break;
        case ActionId::SchemeNineKey: settings_.scheme = InputScheme::NineKey; break;
        case ActionId::ToggleMixedInput: settings_.mixedInput = !settings_.mixedInput; break;
        case ActionId::ToggleChinesePunct: settings_.chinesePunctuation = !settings_.chinesePunctuation; break;
        case ActionId::SkinTerminal: settings_.skin = SkinPreset::Terminal; break;
        case ActionId::SkinWoodType: settings_.skin = SkinPreset::WoodType; break;
        case ActionId::SkinTypewriter: settings_.skin = SkinPreset::Typewriter; break;
        case ActionId::SkinBrutal: settings_.skin = SkinPreset::Brutal; break;
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
    if (action == ActionId::SkinTerminal || action == ActionId::SkinWoodType ||
        action == ActionId::SkinTypewriter || action == ActionId::SkinBrutal) {
        settings_.themeMode = skinMetrics(settings_.skin).defaultThemeMode;
    }
    saveAndNotify();
}

void SettingsWindow::drawTianzigeLogo(float x, float y, float size) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(toD2DColor(BrandMark::CreamBackground), &brush)) || !brush) return;

    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + size, y + size), 0.0f, 0.0f), brush);

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
        skin, colors, settings_.keyOpacity, false, functional, emphasized, selected);

    const auto face = D2D1::RectF(rect.left, rect.top, rect.right - gc.ledgeDepthDp, rect.bottom - gc.ledgeDepthDp);
    ID2D1SolidColorBrush* solid = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(toD2DColor(gc.top), &solid)) && solid) {
        if (gc.ledgeDepthDp > 0.0f) {
            const float offset = gc.ledgeDepthDp;
            solid->SetColor(toD2DColor(gc.ledge));
            renderTarget_->FillRectangle(D2D1::RectF(face.left + offset, face.top + offset,
                face.right + offset, face.bottom + offset), solid);
        }
        solid->SetColor(toD2DColor(gc.top));
        const auto cap = D2D1::RoundedRect(face, gc.cornerRadiusDp, gc.cornerRadiusDp);
        renderTarget_->FillRoundedRectangle(cap, solid);
        if (gc.borderWidthDp > 0) {
            solid->SetColor(toD2DColor(gc.edge));
            renderTarget_->DrawRoundedRectangle(cap, solid, gc.borderWidthDp);
        }
        solid->Release();
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
                ? accentForeground(colors)
                : (selected ? colors.onCandidate : colors.text);

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

    const float pad = 20.0f;
    const float fullW = wDip - pad * 2.0f;
    const float cardRadius = metrics.cornerRadiusDp;
    float y = 14.0f;

    auto addPill = [&](float x, float py, float w, float h, const std::wstring& label, ActionId act,
                       bool sel, bool func = false, bool emph = false, float fSize = 12.5f) {
        D2D1_RECT_F r = D2D1::RectF(x, py, x + w, py + h);
        hitControls_.push_back({r, act});
        drawGlassButton(r, label, settings_.skin, colors, sel, func, emph, hoveredAction_ == act, fSize, false);
    };

    // 1. Showcase Top Bar: Tianzige Logo + Brand + ~/SETTINGS + Action Pills
    drawTianzigeLogo(pad, y + 4.0f, 32.0f);
    brush->SetColor(toD2DColor(colors.text));
    if (titleFormat_) {
        renderTarget_->DrawText(L"方寸输入法", 5, titleFormat_,
                                D2D1::RectF(pad + 42.0f, y + 1.0f, pad + 190.0f, y + 24.0f), brush);
    }
    if (monoFormat_) {
        brush->SetColor(toD2DColor(colors.accent));
        std::wstring tag = L"~/SETTINGS";
        renderTarget_->DrawText(tag.c_str(), static_cast<UINT32>(tag.size()), monoFormat_,
                                D2D1::RectF(pad + 42.0f, y + 23.0f, pad + 130.0f, y + 38.0f), brush);
    }
    if (smallFormat_) {
        brush->SetColor(toD2DColor(colors.secondary));
        std::wstring sub = L"·  方寸之间，自有天地";
        renderTarget_->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()), smallFormat_,
                                D2D1::RectF(pad + 126.0f, y + 23.0f, pad + 320.0f, y + 38.0f), brush);
    }

    // Top-right version badge + Reset + Done
    {
        float rightX = wDip - pad;
        addPill(rightX - 88.0f, y + 3.0f, 88.0f, 32.0f, L"完成", ActionId::CloseWindow, false, false, true, 12.5f);
        addPill(rightX - 184.0f, y + 3.0f, 88.0f, 32.0f, L"恢复默认", ActionId::ResetDefaults, false, true, false, 12.0f);

        D2D1_RECT_F verRc = D2D1::RectF(rightX - 294.0f, y + 6.0f, rightX - 194.0f, y + 32.0f);
        D2D1_ROUNDED_RECT verRR = D2D1::RoundedRect(verRc, 0.0f, 0.0f);
        brush->SetColor(toD2DColor(colors.key.withAlpha(0.65f)));
        renderTarget_->FillRoundedRectangle(verRR, brush);
        brush->SetColor(toD2DColor(colors.border.withAlpha(0.75f)));
        renderTarget_->DrawRoundedRectangle(verRR, brush, 0.8f);
        if (monoFormat_) {
            monoFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(toD2DColor(colors.secondary));
            std::wstring ver = L"v0.4.0 · x64";
            renderTarget_->DrawText(ver.c_str(), static_cast<UINT32>(ver.size()), monoFormat_, verRc, brush);
            monoFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
    y += 48.0f;

    // Subtle hairline divider
    brush->SetColor(toD2DColor(colors.border.withAlpha(0.45f)));
    renderTarget_->DrawLine(D2D1::Point2F(pad, y), D2D1::Point2F(wDip - pad, y), brush, 0.8f);
    y += 8.0f;

    // 2. Category Filter Bar (awesomemygo.dev showcase style)
    {
        float tx = pad;
        auto addTab = [&](const std::wstring& label, float w, ActionId act, CategoryTab tab) {
            addPill(tx, y, w, 28.0f, label, act, activeTab_ == tab, false, false, 12.0f);
            tx += w + 8.0f;
        };
        addTab(L"全部 · 4", 82.0f, ActionId::TabAll, CategoryTab::All);
        addTab(L"输入方案", 86.0f, ActionId::TabInput, CategoryTab::Input);
        addTab(L"皮肤外观", 86.0f, ActionId::TabTheme, CategoryTab::Theme);
        addTab(L"字号与特效", 96.0f, ActionId::TabTypography, CategoryTab::Typography);

        if (smallFormat_) {
            smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            brush->SetColor(toD2DColor(colors.secondary));
            std::wstring status = L"当前方案：";
            if (settings_.scheme == InputScheme::Flypy) status += L"小鹤双拼";
            else if (settings_.scheme == InputScheme::Pinyin) status += L"全拼";
            else status += L"九宫格";
            status += settings_.mixedInput ? L" · 中英混输" : L" · 纯中文";
            renderTarget_->DrawText(status.c_str(), static_cast<UINT32>(status.size()), smallFormat_,
                                    D2D1::RectF(tx, y, wDip - pad, y + 28.0f), brush);
            smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
    y += 38.0f;

    // 3. Live Preview Showcase Card (Candidate Bar + 3D Glass Keys + TypingGlow)
    D2D1_RECT_F previewRc = D2D1::RectF(pad, y, wDip - pad, y + 114.0f);
    hitControls_.push_back({previewRc, ActionId::PreviewGlowTrigger});

    D2D1_ROUNDED_RECT previewRR = D2D1::RoundedRect(previewRc, cardRadius, cardRadius);
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

    brush->SetColor(toD2DColor(colors.border.withAlpha(0.85f)));
    renderTarget_->DrawRoundedRectangle(previewRR, brush, 1.0f);

    if (smallFormat_) {
        brush->SetColor(toD2DColor(colors.secondary));
        renderTarget_->DrawText(L"wanshang", 8, smallFormat_,
                                D2D1::RectF(previewRc.left + 14.0f, previewRc.top + 5.0f, previewRc.left + 180.0f, previewRc.top + 21.0f),
                                brush);
    }
    if (monoFormat_) {
        monoFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        brush->SetColor(toD2DColor(colors.secondary.withAlpha(0.85f)));
        std::wstring hint = L"LIVE PREVIEW · 点击预览区测试流光";
        renderTarget_->DrawText(hint.c_str(), static_cast<UINT32>(hint.size()), monoFormat_,
                                D2D1::RectF(previewRc.left + 200.0f, previewRc.top + 5.0f, previewRc.right - 14.0f, previewRc.top + 21.0f),
                                brush);
        monoFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // First candidate highlight
    D2D1_RECT_F firstCandRc = D2D1::RectF(previewRc.left + 12.0f, previewRc.top + 22.0f, previewRc.left + 98.0f, previewRc.top + 56.0f);
    brush->SetColor(toD2DColor(colors.pressed));
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(firstCandRc, HeaderMetricsSpec::kFirstCornerDp, HeaderMetricsSpec::kFirstCornerDp), brush);

    if (dwriteFactory_) {
        IDWriteTextFormat* pFmt = nullptr;
        if (SUCCEEDED(dwriteFactory_->CreateTextFormat(
                kUiFontFamily, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(settings_.firstCandidateSize), L"", &pFmt)) && pFmt) {
            pFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            brush->SetColor(toD2DColor(colors.onCandidate));
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
            renderTarget_->DrawText(L"2 网上   3 晚上好   4 婉赏   ‹ 1/3 ›", 26, oFmt,
                                    D2D1::RectF(firstCandRc.right + 14.0f, firstCandRc.top, previewRc.right - 14.0f, firstCandRc.bottom),
                                    brush);
            oFmt->Release();
        }
    }

    // Preview 3D glass keycaps row
    {
        float kx = previewRc.left + 12.0f;
        float ky = previewRc.top + 62.0f;
        float kh = 42.0f;
        drawGlassButton(D2D1::RectF(kx, ky, kx + 52.0f, ky + kh), L"q", settings_.skin, colors,
                        false, false, false, false, static_cast<float>(settings_.keyLetterSize), settings_.keyLetterBold);
        kx += 58.0f;
        drawGlassButton(D2D1::RectF(kx, ky, kx + 52.0f, ky + kh), L"w", settings_.skin, colors,
                        false, false, false, false, static_cast<float>(settings_.keyLetterSize), settings_.keyLetterBold);
        kx += 58.0f;
        drawGlassButton(D2D1::RectF(kx, ky, kx + 52.0f, ky + kh), L"e", settings_.skin, colors,
                        false, false, false, false, static_cast<float>(settings_.keyLetterSize), settings_.keyLetterBold);
        kx += 58.0f;
        std::wstring modeKeyLabel = settings_.mixedInput ? L"中英混输"
            : (settings_.scheme == InputScheme::Flypy ? L"小鹤双拼"
                : (settings_.scheme == InputScheme::Pinyin ? L"全拼" : L"九宫格"));
        drawGlassButton(D2D1::RectF(kx, ky, kx + 200.0f, ky + kh), modeKeyLabel,
                        settings_.skin, colors, false, true, false, false, 13.0f, false);
        kx += 206.0f;
        drawGlassButton(D2D1::RectF(kx, ky, previewRc.right - 12.0f, ky + kh), L"发送", settings_.skin, colors,
                        false, false, true, false, 13.0f, false);
    }

    y += 126.0f;

    // Helper to draw a showcase Bento card container
    auto drawCardFrame = [&](float x, float cy, float w, float h, const std::wstring& monoTag, const std::wstring& title) {
        D2D1_RECT_F rc = D2D1::RectF(x, cy, x + w, cy + h);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rc, cardRadius, cardRadius);
        brush->SetColor(toD2DColor(colors.key.withAlpha(colors.isDarkKeyboard() ? 0.35f : 0.55f)));
        renderTarget_->FillRoundedRectangle(rr, brush);
        brush->SetColor(toD2DColor(colors.border.withAlpha(0.75f)));
        renderTarget_->DrawRoundedRectangle(rr, brush, 0.9f);

        // Accent indicator dot + monospace tag + title
        float innerX = x + 14.0f;
        float headerY = cy + 12.0f;
        brush->SetColor(toD2DColor(colors.accent));
        renderTarget_->FillRectangle(D2D1::RectF(innerX, headerY + 6.0f, innerX + 6.0f, headerY + 12.0f), brush);

        if (monoFormat_) {
            brush->SetColor(toD2DColor(colors.accent));
            renderTarget_->DrawText(monoTag.c_str(), static_cast<UINT32>(monoTag.size()), monoFormat_,
                                    D2D1::RectF(innerX + 11.0f, headerY, innerX + 110.0f, headerY + 18.0f), brush);
        }
        if (sectionFormat_) {
            brush->SetColor(toD2DColor(colors.text));
            renderTarget_->DrawText(title.c_str(), static_cast<UINT32>(title.size()), sectionFormat_,
                                    D2D1::RectF(innerX + 106.0f, headerY - 1.0f, x + w - 14.0f, headerY + 19.0f), brush);
        }
    };

    auto drawInputCard = [&](float x, float cy, float w, float h) {
        drawCardFrame(x, cy, w, h, L"01 · ~/INPUT", L"输入方案与行为");
        float ix = x + 14.0f;
        float iw = w - 28.0f;
        float ry = cy + 40.0f;
        float w3 = (iw - 12.0f) / 3.0f;
        addPill(ix, ry, w3, 36.0f, L"小鹤双拼", ActionId::SchemeFlypy, settings_.scheme == InputScheme::Flypy);
        addPill(ix + w3 + 6.0f, ry, w3, 36.0f, L"全拼", ActionId::SchemePinyin, settings_.scheme == InputScheme::Pinyin);
        addPill(ix + (w3 + 6.0f) * 2.0f, ry, w3, 36.0f, L"九宫格", ActionId::SchemeNineKey, settings_.scheme == InputScheme::NineKey);

        ry += 44.0f;
        float w2 = (iw - 6.0f) / 2.0f;
        addPill(ix, ry, w2, 34.0f, settings_.mixedInput ? L"✓ 中英混输：开启" : L"中英混输：关闭",
                ActionId::ToggleMixedInput, settings_.mixedInput, true);
        addPill(ix + w2 + 6.0f, ry, w2, 34.0f, settings_.chinesePunctuation ? L"✓ 中文标点：开启" : L"中文标点：关闭",
                ActionId::ToggleChinesePunct, settings_.chinesePunctuation, true);

        ry += 44.0f;
        if (smallFormat_) {
            brush->SetColor(toD2DColor(colors.secondary));
            std::wstring tip = L"候选分页：每页 9 项 · 支持 = / - / PageUp / PageDown / 滚轮翻页";
            renderTarget_->DrawText(tip.c_str(), static_cast<UINT32>(tip.size()), smallFormat_,
                                    D2D1::RectF(ix, ry, ix + iw, ry + 24.0f), brush);
        }
    };

    auto drawThemeCard = [&](float x, float cy, float w, float h) {
        drawCardFrame(x, cy, w, h, L"02 · ~/THEME", L"皮肤预设与明暗");
        float ix = x + 14.0f;
        float iw = w - 28.0f;
        float ry = cy + 40.0f;
        float w2 = (iw - 6.0f) / 2.0f;
        addPill(ix, ry, w2, 36.0f, L"木框活字", ActionId::SkinWoodType, settings_.skin == SkinPreset::WoodType);
        addPill(ix + w2 + 6.0f, ry, w2, 36.0f, L"经典打字机", ActionId::SkinTypewriter, settings_.skin == SkinPreset::Typewriter);

        ry += 42.0f;
        addPill(ix, ry, w2, 36.0f, L"绿屏终端", ActionId::SkinTerminal, settings_.skin == SkinPreset::Terminal);
        addPill(ix + w2 + 6.0f, ry, w2, 36.0f, L"极客机械", ActionId::SkinBrutal, settings_.skin == SkinPreset::Brutal);

        ry += 46.0f;
        float w3 = (iw - 12.0f) / 3.0f;
        addPill(ix, ry, w3, 34.0f, L"跟随系统", ActionId::ModeSystem, settings_.themeMode == ThemeMode::System);
        addPill(ix + w3 + 6.0f, ry, w3, 34.0f, L"浅色模式", ActionId::ModeLight, settings_.themeMode == ThemeMode::Light);
        addPill(ix + (w3 + 6.0f) * 2.0f, ry, w3, 34.0f, L"深色模式", ActionId::ModeDark, settings_.themeMode == ThemeMode::Dark);
    };

    auto drawEffectsCard = [&](float x, float cy, float w, float h) {
        drawCardFrame(x, cy, w, h, L"03 · ~/EFFECTS", L"特效与视觉");
        float ix = x + 14.0f;
        float iw = w - 28.0f;
        float ry = cy + 42.0f;
        addPill(ix, ry, iw, 36.0f, settings_.typingGlow ? L"✓ 打字流光特效：开启" : L"打字流光特效：关闭",
                ActionId::ToggleTypingGlow, settings_.typingGlow, true);
        ry += 44.0f;
        addPill(ix, ry, iw, 36.0f, settings_.keyLetterBold ? L"✓ 键帽字母加粗：开启" : L"键帽字母加粗：常规",
                ActionId::ToggleKeyBold, settings_.keyLetterBold, true);
        ry += 44.0f;
        if (smallFormat_) {
            brush->SetColor(toD2DColor(colors.secondary));
            std::wstring note = L"窗口标题栏、候选窗与设置页已启用田字格品牌标识";
            renderTarget_->DrawText(note.c_str(), static_cast<UINT32>(note.size()), smallFormat_,
                                    D2D1::RectF(ix, ry, ix + iw, ry + 22.0f), brush);
        }
    };

    auto drawTypographyCard = [&](float x, float cy, float w, float h) {
        drawCardFrame(x, cy, w, h, L"04 · ~/METRICS", L"字号与透明度");
        float ix = x + 14.0f;
        float iw = w - 28.0f;
        float ry = cy + 42.0f;

        auto drawStepper = [&](const std::wstring& title, const std::wstring& valueStr, ActionId minusAct, ActionId plusAct) {
            brush->SetColor(toD2DColor(colors.text));
            if (bodyFormat_) {
                renderTarget_->DrawText(title.c_str(), static_cast<UINT32>(title.size()), bodyFormat_,
                                        D2D1::RectF(ix, ry, ix + iw - 146.0f, ry + 32.0f), brush);
            }
            float rightX = ix + iw;
            addPill(rightX - 38.0f, ry, 38.0f, 32.0f, L"+", plusAct, false, true);
            brush->SetColor(toD2DColor(colors.accent));
            if (sectionFormat_) {
                sectionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                renderTarget_->DrawText(valueStr.c_str(), static_cast<UINT32>(valueStr.size()), sectionFormat_,
                                        D2D1::RectF(rightX - 98.0f, ry, rightX - 42.0f, ry + 32.0f), brush);
                sectionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            addPill(rightX - 140.0f, ry, 38.0f, 32.0f, L"−", minusAct, false, true);
            ry += 42.0f;
        };

        drawStepper(L"第一候选字号 (16–30)", std::to_wstring(settings_.firstCandidateSize) + L" px",
                    ActionId::FirstSizeMinus, ActionId::FirstSizePlus);
        drawStepper(L"键帽字母字号 (14–30)", std::to_wstring(settings_.keyLetterSize) + L" pt",
                    ActionId::LetterSizeMinus, ActionId::LetterSizePlus);
        int opacityPct = static_cast<int>(std::lround(settings_.keyOpacity * 100.0f));
        drawStepper(L"键帽透明度 (40%–100%)", std::to_wstring(opacityPct) + L"%",
                    ActionId::OpacityMinus, ActionId::OpacityPlus);
    };

    // 4. Render Bento Grid based on active category tab
    const float colGap = 14.0f;
    const float colW = (fullW - colGap) / 2.0f;
    const float leftX = pad;
    const float rightX = pad + colW + colGap;

    if (activeTab_ == CategoryTab::All) {
        drawInputCard(leftX, y, colW, 184.0f);
        drawThemeCard(rightX, y, colW, 184.0f);
        drawEffectsCard(leftX, y + 196.0f, colW, 184.0f);
        drawTypographyCard(rightX, y + 196.0f, colW, 184.0f);
    } else if (activeTab_ == CategoryTab::Input) {
        drawInputCard(pad, y, fullW, 200.0f);
    } else if (activeTab_ == CategoryTab::Theme) {
        drawThemeCard(pad, y, fullW, 200.0f);
    } else if (activeTab_ == CategoryTab::Typography) {
        drawEffectsCard(leftX, y, colW, 200.0f);
        drawTypographyCard(rightX, y, colW, 200.0f);
    }

    brush->Release();

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        releaseDeviceResources();
    }
    EndPaint(hwnd_, &ps);
}

} // namespace fangcun::tsf
