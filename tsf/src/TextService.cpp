#include "TextService.hpp"
#include "DllMain.hpp"
#include "EditSession.hpp"
#include "Guid.hpp"

#include <new>

namespace fangcun::tsf {

namespace {

constexpr wchar_t kMsgWndClass[] = L"FangCunTsfMessageWindow";
constexpr wchar_t kCompanionWndTitle[] = L"FangCunCompanionMsgWindow";

// Companion notify codes (wParam): 1 = English mode (lParam 0/1), 2 = settings changed, 3 = IME active (lParam 0/1)
UINT companionNotifyMessage() {
    static UINT m = RegisterWindowMessageW(L"FangCunIME_CompanionNotify");
    return m;
}

UINT setEnglishMessage() {
    static UINT m = RegisterWindowMessageW(L"FangCunIME_SetEnglish");
    return m;
}

void postToCompanion(WPARAM code, LPARAM value) {
    HWND h = FindWindowExW(HWND_MESSAGE, nullptr, L"Static", kCompanionWndTitle);
    if (h) PostMessageW(h, companionNotifyMessage(), code, value);
}

void broadcastEnglishToTsf(bool english) {
    HWND h = nullptr;
    while ((h = FindWindowExW(HWND_MESSAGE, h, kMsgWndClass, nullptr)) != nullptr) {
        PostMessageW(h, setEnglishMessage(), english ? 1 : 0, 0);
    }
}

CandidateLayout parseCandidateLayout(const std::string& id) {
    if (id == "single") return CandidateLayout::SingleRow;
    if (id == "vertical") return CandidateLayout::Vertical;
    return CandidateLayout::DoubleRow;
}

std::wstring applyOutputStyle(std::wstring text, const AppSettings& settings) {
    if (settings.traditional && !text.empty()) {
        const auto length = static_cast<int>(text.size());
        int size = LCMapStringEx(L"zh-CN", LCMAP_TRADITIONAL_CHINESE, text.data(), length,
            nullptr, 0, nullptr, nullptr, 0);
        if (size > 0) {
            std::wstring converted(size, L'\0');
            if (LCMapStringEx(L"zh-CN", LCMAP_TRADITIONAL_CHINESE, text.data(), length,
                    converted.data(), size, nullptr, nullptr, 0) > 0) text = std::move(converted);
        }
    }
    if (settings.fullWidth) {
        for (wchar_t& c : text) {
            if (c == L' ') c = L'\x3000';
            else if (c >= L'!' && c <= L'~') c = static_cast<wchar_t>(c + 0xFEE0);
        }
    }
    return text;
}

bool fullWidthDirectKey(const KeyInput& key, const AppSettings& settings) {
    return settings.fullWidth && !key.ctrl && !key.alt && !key.win &&
        key.ascii >= ' ' && key.ascii <= '~';
}

ShiftSide shiftSide(WPARAM key, LPARAM message) {
    if (key == VK_LSHIFT) return ShiftSide::Left;
    if (key == VK_RSHIFT) return ShiftSide::Right;
    if (key != VK_SHIFT) return ShiftSide::None;
    const auto scanCode = (static_cast<UINT>(message) >> 16) & 0xFF;
    return scanCode == 0x36 ? ShiftSide::Right : ShiftSide::Left;
}

class FangCunDisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    FangCunDisplayAttributeInfo() { DllAddRef(); }
    virtual ~FangCunDisplayAttributeInfo() { DllRelease(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_INVALIDARG;
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
            *ppvObj = static_cast<ITfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    STDMETHODIMP_(ULONG) Release() override {
        LONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    STDMETHODIMP GetGUID(GUID* pguid) override {
        if (!pguid) return E_INVALIDARG;
        *pguid = GUID_FangCunDisplayAttributeInput;
        return S_OK;
    }

    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override {
        if (!pbstrDesc) return E_INVALIDARG;
        *pbstrDesc = SysAllocString(L"FangCun Input Composition");
        return *pbstrDesc ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) return E_INVALIDARG;
        pda->crText.type = TF_CT_NONE;
        pda->crBk.type = TF_CT_NONE;
        pda->lsStyle = TF_LS_DOT;
        pda->fBoldLine = FALSE;
        pda->crLine.type = TF_CT_NONE;
        pda->bAttr = TF_ATTR_INPUT;
        return S_OK;
    }

    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) override { return E_NOTIMPL; }
    STDMETHODIMP Reset() override { return S_OK; }

private:
    std::atomic<LONG> refCount_{1};
};

class EnumDisplayAttributeInfoImpl : public IEnumTfDisplayAttributeInfo {
public:
    EnumDisplayAttributeInfoImpl() : index_(0) { DllAddRef(); }
    virtual ~EnumDisplayAttributeInfoImpl() { DllRelease(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_INVALIDARG;
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
            *ppvObj = static_cast<IEnumTfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    STDMETHODIMP_(ULONG) Release() override {
        LONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override {
        if (!ppEnum) return E_INVALIDARG;
        auto* clone = new (std::nothrow) EnumDisplayAttributeInfoImpl();
        if (!clone) return E_OUTOFMEMORY;
        clone->index_ = index_;
        *ppEnum = clone;
        return S_OK;
    }

    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) override {
        if (!rgInfo) return E_INVALIDARG;
        ULONG fetched = 0;
        if (ulCount > 0 && index_ == 0) {
            rgInfo[0] = new (std::nothrow) FangCunDisplayAttributeInfo();
            if (!rgInfo[0]) return E_OUTOFMEMORY;
            index_ = 1;
            fetched = 1;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == ulCount) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }

    STDMETHODIMP Skip(ULONG ulCount) override {
        if (ulCount > 0) index_ = 1;
        return S_OK;
    }

private:
    std::atomic<LONG> refCount_{1};
    ULONG index_;
};

} // namespace

TextService::TextService() {
    DllAddRef();
}

TextService::~TextService() {
    Deactivate();
    DllRelease();
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor) ||
        IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
        *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppvObj = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
        *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
    return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
}

STDMETHODIMP_(ULONG) TextService::Release() {
    LONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    return ActivateEx(ptim, tid, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD) {
    if (!ptim) return E_INVALIDARG;
    Deactivate();

    threadMgr_ = ptim;
    threadMgr_->AddRef();
    clientId_ = tid;

    settings_ = SettingsStore::load();
    engine_ = std::make_unique<FangCunEngine>(
        SettingsStore::getAssetDirectory(),
        SettingsStore::getDataDirectory());
    engine_->prepare(settings_.scheme);

    createMessageWindow();

    candidateWindow_ = std::make_unique<CandidateWindow>(
        [this](size_t index) { onCandidateClicked(index); },
        [this]() { openSettingsWindow(); },
        [this](int delta) { onPageChange(delta); });
    candidateWindow_->setPageSize(static_cast<size_t>(settings_.pageSize));
    candidateWindow_->setWheelPaging(settings_.wheelPaging);
    candidateWindow_->setLayout(parseCandidateLayout(settings_.candidateLayout));
    candidateWindow_->create(nullptr);
    candidateWindow_->setCustomSkin(SettingsStore::getDataDirectory(), settings_.customSkinId, settings_.customSkinHash,
                                    SettingsStore::effectiveDarkMode(settings_));

    keyHandler_ = std::make_unique<KeyHandler>(
        [this](uint64_t sessionGen, std::string input, InputScheme scheme, bool mixed) -> uint64_t {
            if (!engine_) return 0;
            return engine_->queryAsync(
                std::move(input),
                scheme,
                mixed,
                [this, sessionGen](uint64_t, std::string inp, InputScheme, bool, std::vector<std::string> res) {
                    {
                        std::lock_guard<std::mutex> lock(asyncMutex_);
                        asyncQueue_.push_back({sessionGen, std::move(inp), std::move(res)});
                    }
                    if (msgHwnd_) {
                        PostMessageW(msgHwnd_, WM_ASYNC_CANDIDATES, 0, 0);
                    }
                });
        },
        [this](std::string_view text) {
            if (engine_) engine_->learn(text);
        },
        [this]() {
            if (engine_) engine_->cancelPendingQueries();
        });

    keyHandler_->setScheme(settings_.scheme);
    keyHandler_->setMixedInput(settings_.mixedInput);
    keyHandler_->setChinesePunctuation(settings_.chinesePunctuation);
    keyHandler_->setCapsLockBypass(settings_.capsLockBypass);
    keyHandler_->setPageSize(static_cast<size_t>(settings_.pageSize));

    if (!initThreadMgrSink() || !initKeyEventSink()) {
        Deactivate();
        return E_FAIL;
    }
    initDisplayAttribute();
    if (!FindWindowExW(HWND_MESSAGE, nullptr, L"Static", kCompanionWndTitle)) {
        std::filesystem::path exePath = SettingsStore::getDllDirectory() / L"FangCunSettings.exe";
        std::error_code ec;
        if (std::filesystem::exists(exePath, ec)) {
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            std::wstring cmd = L"\"" + exePath.wstring() + L"\" --tray";
            if (CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    } else {
        postToCompanion(3, 1);
    }
    return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    postToCompanion(3, 0);
    resetAllState();
    uninitKeyEventSink();
    uninitThreadMgrSink();

    if (settingsWindow_) {
        settingsWindow_->destroy();
        settingsWindow_.reset();
    }
    if (candidateWindow_) {
        candidateWindow_->destroy();
        candidateWindow_.reset();
    }
    destroyMessageWindow();
    keyHandler_.reset();
    if (engine_) {
        engine_->close();
        engine_.reset();
    }
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (activeContext_) {
        activeContext_->Release();
        activeContext_ = nullptr;
    }
    if (threadMgr_) {
        threadMgr_->Release();
        threadMgr_ = nullptr;
    }
    clientId_ = TF_CLIENTID_NULL;
    return S_OK;
}

bool TextService::initThreadMgrSink() {
    ITfSource* source = nullptr;
    if (FAILED(threadMgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))) || !source) {
        return false;
    }
    HRESULT hr = source->AdviseSink(IID_ITfThreadMgrEventSink,
                                    static_cast<ITfThreadMgrEventSink*>(this),
                                    &threadMgrSinkCookie_);
    source->Release();
    return SUCCEEDED(hr);
}

void TextService::uninitThreadMgrSink() {
    if (threadMgr_ && threadMgrSinkCookie_ != TF_INVALID_COOKIE) {
        ITfSource* source = nullptr;
        if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))) && source) {
            source->UnadviseSink(threadMgrSinkCookie_);
            source->Release();
        }
        threadMgrSinkCookie_ = TF_INVALID_COOKIE;
    }
}

bool TextService::initKeyEventSink() {
    ITfKeystrokeMgr* keystrokeMgr = nullptr;
    if (FAILED(threadMgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keystrokeMgr))) || !keystrokeMgr) {
        return false;
    }
    HRESULT hr = keystrokeMgr->AdviseKeyEventSink(clientId_, static_cast<ITfKeyEventSink*>(this), TRUE);
    keystrokeMgr->Release();
    return SUCCEEDED(hr);
}

void TextService::uninitKeyEventSink() {
    if (threadMgr_ && clientId_ != TF_CLIENTID_NULL) {
        ITfKeystrokeMgr* keystrokeMgr = nullptr;
        if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keystrokeMgr))) && keystrokeMgr) {
            keystrokeMgr->UnadviseKeyEventSink(clientId_);
            keystrokeMgr->Release();
        }
    }
}

bool TextService::initDisplayAttribute() {
    ITfCategoryMgr* catMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, reinterpret_cast<void**>(&catMgr))) && catMgr) {
        catMgr->RegisterGUID(GUID_FangCunDisplayAttributeInput, &displayAttrAtom_);
        catMgr->Release();
        return true;
    }
    return false;
}

bool TextService::createMessageWindow() {
    if (msgHwnd_) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TextService::msgWndProc;
    wc.hInstance = g_hInstance;
    wc.lpszClassName = kMsgWndClass;
    RegisterClassExW(&wc);

    msgHwnd_ = CreateWindowExW(0, kMsgWndClass, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, g_hInstance, this);
    return msgHwnd_ != nullptr;
}

void TextService::destroyMessageWindow() {
    if (msgHwnd_) {
        DestroyWindow(msgHwnd_);
        msgHwnd_ = nullptr;
    }
}

LRESULT CALLBACK TextService::msgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TextService* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<TextService*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TextService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self && msg == WM_ASYNC_CANDIDATES) {
        std::vector<AsyncPayload> batch;
        {
            std::lock_guard<std::mutex> lock(self->asyncMutex_);
            batch.swap(self->asyncQueue_);
        }
        for (auto& item : batch) {
            if (self->keyHandler_) {
                KeyAction act = self->keyHandler_->onAsyncCandidates(
                    item.sessionGen, item.input, std::move(item.candidates));
                self->applyKeyAction(self->activeContext_, act);
            }
        }
        return 0;
    }
    if (self && msg == SettingsWindow::settingsChangedMessage()) {
        const auto loaded = SettingsStore::load();
        if (loaded.readSucceeded) self->applySettings(loaded);
        return 0;
    }
    if (self && msg == setEnglishMessage()) {
        bool want = (wp != 0);
        if (self->keyHandler_ && self->keyHandler_->englishMode() != want) {
            KeyAction act = self->keyHandler_->toggleEnglishMode();
            if (act.handled || act.hideCandidates || act.updateComposition) {
                self->applyKeyAction(self->activeContext_, act);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TextService::openSettingsWindow() {
    std::filesystem::path exePath = SettingsStore::getDllDirectory() / L"FangCunSettings.exe";
    std::error_code ec;
    if (std::filesystem::exists(exePath, ec)) {
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
        if (CreateProcessW(
                exePath.c_str(),
                cmd.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &si,
                &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
    if (!settingsWindow_) {
        settingsWindow_ = std::make_unique<SettingsWindow>([this](const AppSettings& updated) {
            applySettings(updated);
        });
    }
    settingsWindow_->showOrActivate(nullptr);
}

void TextService::applySettings(const AppSettings& newSettings) {
    bool schemeChanged = (settings_.scheme != newSettings.scheme);
    if (settings_.shiftToggleEnglish != newSettings.shiftToggleEnglish) shiftTap_.reset();
    settings_ = newSettings;
    if (engine_ && schemeChanged) {
        engine_->prepare(settings_.scheme);
    }
    if (keyHandler_) {
        keyHandler_->setScheme(settings_.scheme);
        keyHandler_->setMixedInput(settings_.mixedInput);
        keyHandler_->setChinesePunctuation(settings_.chinesePunctuation);
        keyHandler_->setCapsLockBypass(settings_.capsLockBypass);
        keyHandler_->setPageSize(static_cast<size_t>(settings_.pageSize));
    }
    if (candidateWindow_) {
        candidateWindow_->setCustomSkin(SettingsStore::getDataDirectory(), settings_.customSkinId, settings_.customSkinHash,
                                        SettingsStore::effectiveDarkMode(settings_));
        candidateWindow_->setPageSize(static_cast<size_t>(settings_.pageSize));
        candidateWindow_->setWheelPaging(settings_.wheelPaging);
        candidateWindow_->setLayout(parseCandidateLayout(settings_.candidateLayout));
        if (candidateWindow_->isVisible() && keyHandler_) {
            candidateWindow_->update(
                keyHandler_->composing(),
                keyHandler_->candidates(),
                keyHandler_->selectedIndex(),
                keyHandler_->candidatesPending(),
                keyHandler_->scheme(),
                settings_.skin,
                SettingsStore::effectiveDarkMode(settings_),
                settings_.firstCandidateSize,
                settings_.typingGlow);
        }
    }
}

void TextService::setComposition(ITfComposition* comp) {
    if (comp) comp->AddRef();
    if (composition_) composition_->Release();
    composition_ = comp;
}

void TextService::updateCandidateWindowPosition(const RECT& caretRect) {
    if (candidateWindow_) {
        candidateWindow_->moveNearCaret(caretRect);
    }
}

void TextService::resetAllState(ITfContext* context) {
    shiftTap_.reset();
    {
        std::lock_guard<std::mutex> lock(asyncMutex_);
        asyncQueue_.clear();
    }
    if (keyHandler_) {
        KeyAction act = keyHandler_->resetContext();
        if (context && composition_) {
            auto* session = new (std::nothrow) UpdateCompositionEditSession(this, context, L"", false, true);
            // Detach before scheduling: the next document must never reuse
            // this range with its own edit cookie, even if cleanup is delayed.
            setComposition(nullptr);
            if (session) {
                HRESULT hrSession = S_OK;
                context->RequestEditSession(clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
                session->Release();
            }
        }
        if (candidateWindow_ && act.hideCandidates) {
            candidateWindow_->hide();
        }
    }
    if (candidateWindow_) candidateWindow_->hide();
}

void TextService::applyKeyAction(ITfContext* context, const KeyAction& action) {
    KeyAction visible = action;
    bool editRejected = action.commitText && !context;
    if (context) {
        if (activeContext_ != context) {
            if (activeContext_) activeContext_->Release();
            activeContext_ = context;
            activeContext_->AddRef();
        }
        if (action.commitText) {
            std::optional<std::wstring> followup;
            if (action.updateComposition && !action.compositionText.empty()) {
                followup = utf8ToWide(action.compositionText);
            }
            auto* session = new (std::nothrow) UpdateCompositionEditSession(
                this, context, applyOutputStyle(utf8ToWide(action.textToCommit), settings_), true, false, std::move(followup));
            bool committed = false;
            bool followupApplied = false;
            if (session) {
                HRESULT hrSession = E_FAIL;
                const HRESULT requested = context->RequestEditSession(
                    clientId_, session, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
                // A failed followup may leave the prefix committed. Inspect the
                // edit's receipt instead of retrying based only on HRESULT.
                const bool synchronous = SUCCEEDED(requested) && hrSession != TF_S_ASYNC;
                committed = synchronous && session->commitAccepted();
                followupApplied = synchronous && session->followupApplied();
                session->Release();
            }
            editRejected = !committed || !followupApplied;
            if (keyHandler_ && action.commitReceipt) {
                auto recovery = keyHandler_->acknowledgeCommit(action.commitReceipt, committed, followupApplied);
                if (recovery.handled) {
                    visible = std::move(recovery);
                    // Restore once, synchronously. Never queue the failed
                    // commit for automatic retry: Space/Enter can retry it.
                    auto* restore = new (std::nothrow) UpdateCompositionEditSession(
                        this, context, utf8ToWide(visible.compositionText), false, visible.compositionText.empty());
                    if (restore) {
                        HRESULT hrRestore = E_FAIL;
                        context->RequestEditSession(clientId_, restore, TF_ES_SYNC | TF_ES_READWRITE, &hrRestore);
                        restore->Release();
                    }
                }
            }
        } else if (action.updateComposition) {
            bool cancelOnly = action.compositionText.empty();
            auto* session = new (std::nothrow) UpdateCompositionEditSession(
                this, context, utf8ToWide(action.compositionText), false, cancelOnly);
            if (session) {
                HRESULT hrSession = E_FAIL;
                const HRESULT requested = context->RequestEditSession(
                    clientId_, session, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
                editRejected = FAILED(requested) || FAILED(hrSession) || hrSession == TF_S_ASYNC;
                session->Release();
            } else editRejected = true;
        }
    } else if (keyHandler_ && action.commitReceipt) {
        const auto recovery = keyHandler_->acknowledgeCommit(action.commitReceipt, false);
        if (recovery.handled) visible = recovery;
    }
    if (editRejected || action.inputLimitReached) MessageBeep(MB_ICONWARNING);

    if (candidateWindow_) {
        if (visible.hideCandidates) {
            candidateWindow_->hide();
        } else if (visible.showCandidates && keyHandler_ && !keyHandler_->composing().empty()) {
            candidateWindow_->update(
                keyHandler_->displayComposing(),
                visible.candidates,
                visible.selectedIndex,
                visible.candidatesPending,
                keyHandler_->scheme(),
                settings_.skin,
                SettingsStore::effectiveDarkMode(settings_),
                settings_.firstCandidateSize,
                settings_.typingGlow);
            if (context) requestCaretRect(context);
            candidateWindow_->show();
        }
    }
}

void TextService::requestCaretRect(ITfContext* context) {
    if (!context) return;
    auto* session = new (std::nothrow) GetCaretRectEditSession(this, context);
    if (session) {
        HRESULT hrSession = S_OK;
        context->RequestEditSession(clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READ, &hrSession);
        session->Release();
    }
}

void TextService::onCandidateClicked(size_t index) {
    if (!keyHandler_ || !activeContext_) return;
    KeyAction act = keyHandler_->selectCandidateIndex(index);
    applyKeyAction(activeContext_, act);
}

void TextService::onPageChange(int delta) {
    if (!keyHandler_ || !activeContext_) return;
    KeyAction act = keyHandler_->movePage(delta);
    applyKeyAction(activeContext_, act);
}

KeyInput TextService::translateKey(WPARAM wParam, LPARAM lParam) const {
    KeyInput ki;
    ki.vk = static_cast<uint32_t>(wParam);
    ki.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    ki.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    ki.win = ((GetKeyState(VK_LWIN) & 0x8000) != 0) || ((GetKeyState(VK_RWIN) & 0x8000) != 0);
    ki.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    ki.capsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;

    BYTE kbdState[256] = {};
    if (GetKeyboardState(kbdState)) {
        wchar_t buf[4] = {};
        UINT scanCode = (static_cast<UINT>(lParam) >> 16) & 0xFF;
        int n = ToUnicode(ki.vk, scanCode, kbdState, buf, 4, 0);
        if (n == 1 && buf[0] >= 0x20 && buf[0] < 0x7F) {
            ki.ascii = static_cast<char>(buf[0]);
        }
    }
    return ki;
}

std::wstring TextService::utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

// ITfThreadMgrEventSink
STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) {
    resetAllState(activeContext_);
    if (activeContext_) {
        activeContext_->Release();
        activeContext_ = nullptr;
    }
    return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext*) { return S_OK; }
STDMETHODIMP TextService::OnPopContext(ITfContext*) {
    resetAllState(activeContext_);
    return S_OK;
}

// ITfKeyEventSink
STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) {
    if (!fForeground) {
        resetAllState(activeContext_);
    }
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (!keyHandler_) return S_OK;
    if (activeContext_ != pic) {
        resetAllState(activeContext_);
        if (activeContext_) activeContext_->Release();
        activeContext_ = pic;
        if (activeContext_) activeContext_->AddRef();
    }
    KeyInput ki = translateKey(wParam, lParam);
    const auto shift = shiftSide(wParam, lParam);
    shiftTap_.keyDown(shift, settings_.shiftToggleEnglish && !ki.ctrl && !ki.alt && !ki.win, (lParam & (1LL << 30)) != 0);
    if (shift != ShiftSide::None) return S_OK;
    if (ki.alt && !ki.ctrl && !ki.win && !ki.shift && (wParam == 'V' || wParam == 'v')) {
        *pfEaten = TRUE;
        return S_OK;
    }
    if (ki.ctrl && !ki.alt && !ki.win && !ki.shift && (wParam == VK_OEM_PERIOD || ki.ascii == '.')) {
        *pfEaten = TRUE;
        return S_OK;
    }
    *pfEaten = (keyHandler_->shouldEatKey(ki) || fullWidthDirectKey(ki, settings_)) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (!keyHandler_) return S_OK;

    KeyInput ki = translateKey(wParam, lParam);
    const auto shift = shiftSide(wParam, lParam);
    shiftTap_.keyDown(shift, settings_.shiftToggleEnglish && !ki.ctrl && !ki.alt && !ki.win, (lParam & (1LL << 30)) != 0);
    if (shift != ShiftSide::None) return S_OK;
    if (ki.alt && !ki.ctrl && !ki.win && !ki.shift && (wParam == 'V' || wParam == 'v')) {
        HWND h = FindWindowExW(HWND_MESSAGE, nullptr, L"Static", kCompanionWndTitle);
        if (h) {
            PostMessageW(h, companionNotifyMessage(), 4, 0);
        } else {
            std::filesystem::path exePath = SettingsStore::getDllDirectory() / L"FangCunSettings.exe";
            std::error_code ec;
            if (std::filesystem::exists(exePath, ec)) {
                STARTUPINFOW si = {};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {};
                std::wstring cmd = L"\"" + exePath.wstring() + L"\" --clipboard";
                if (CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
        }
        *pfEaten = TRUE;
        return S_OK;
    }
    if (ki.ctrl && !ki.alt && !ki.win && !ki.shift && (wParam == VK_OEM_PERIOD || ki.ascii == '.')) {
        auto next = settings_;
        next.chinesePunctuation = !next.chinesePunctuation;
        if (SettingsStore::save(next)) {
            applySettings(next);
            SettingsWindow::broadcastSettingsChanged();
            postToCompanion(2, 0);
        } else {
            // Do not change the running mode when persistence did not succeed.
            MessageBeep(MB_ICONWARNING);
        }
        *pfEaten = TRUE;
        return S_OK;
    }
    if (!keyHandler_->shouldEatKey(ki)) {
        if (fullWidthDirectKey(ki, settings_)) {
            KeyAction direct;
            direct.commitText = true;
            direct.textToCommit = std::string(1, ki.ascii);
            applyKeyAction(pic, direct);
            *pfEaten = TRUE;
        }
        return S_OK;
    }
    KeyAction act = keyHandler_->onKeyDown(ki);
    *pfEaten = act.handled ? TRUE : FALSE;
    if (act.handled) {
        applyKeyAction(pic, act);
    }
    return S_OK;
}

void TextService::finishShiftTap(ITfContext* context, WPARAM wParam, LPARAM lParam) {
    // TSF hosts can observe the release through TestKeyUp, KeyUp, or both.
    // Consume our tap state once and leave the hardware key event with the host.
    if (!shiftTap_.keyUp(shiftSide(wParam, lParam)) || !keyHandler_ || !settings_.shiftToggleEnglish) return;
    ITfContext* ctx = context ? context : activeContext_;
    KeyAction act = keyHandler_->toggleEnglishMode();
    if (act.handled || act.hideCandidates || act.updateComposition) applyKeyAction(ctx, act);
    if (ctx) requestCaretRect(ctx);
    broadcastEnglishToTsf(keyHandler_->englishMode());
    postToCompanion(1, keyHandler_->englishMode() ? 1 : 0);
    if (candidateWindow_) {
        candidateWindow_->showModeToast(keyHandler_->englishMode(), settings_.skin,
                                        SettingsStore::effectiveDarkMode(settings_));
    }
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    finishShiftTap(pic, wParam, lParam);
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    finishShiftTap(pic, wParam, lParam);
    return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    return S_OK;
}

// ITfCompositionSink
STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie, ITfComposition* pComposition) {
    // Intentional EndComposition detaches the old object first. Its delayed
    // callback must not clear the following draft or a different document.
    if (!pComposition || pComposition != composition_) return S_OK;
    setComposition(nullptr);
    resetAllState();
    return S_OK;
}

// ITfDisplayAttributeProvider
STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) return E_INVALIDARG;
    *ppEnum = new (std::nothrow) EnumDisplayAttributeInfoImpl();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo) return E_INVALIDARG;
    *ppInfo = nullptr;
    if (IsEqualGUID(guid, GUID_FangCunDisplayAttributeInput)) {
        *ppInfo = new (std::nothrow) FangCunDisplayAttributeInfo();
        return *ppInfo ? S_OK : E_OUTOFMEMORY;
    }
    return E_INVALIDARG;
}

} // namespace fangcun::tsf
