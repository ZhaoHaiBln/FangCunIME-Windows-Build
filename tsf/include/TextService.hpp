#pragma once

#include "CandidateWindow.hpp"
#include "KeyHandler.hpp"
#include "SettingsStore.hpp"
#include "SettingsWindow.hpp"
#include "fangcun/FangCunEngine.hpp"

#include <msctf.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fangcun::tsf {

class TextService : public ITfTextInputProcessorEx,
                    public ITfThreadMgrEventSink,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfDisplayAttributeProvider {
public:
    TextService();
    virtual ~TextService();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pic) override;
    STDMETHODIMP OnPopContext(ITfContext* pic) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;

    // Helpers for EditSessions
    ITfComposition* composition() const { return composition_; }
    void setComposition(ITfComposition* comp);
    TfGuidAtom displayAttributeAtom() const { return displayAttrAtom_; }
    void updateCandidateWindowPosition(const RECT& caretRect);
    ITfThreadMgr* threadMgr() const { return threadMgr_; }
    TfClientId clientId() const { return clientId_; }

private:
    struct AsyncPayload {
        uint64_t sessionGen = 0;
        std::string input;
        std::vector<std::string> candidates;
    };

    static constexpr UINT WM_ASYNC_CANDIDATES = WM_APP + 101;
    static LRESULT CALLBACK msgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool initThreadMgrSink();
    void uninitThreadMgrSink();
    bool initKeyEventSink();
    void uninitKeyEventSink();
    bool initDisplayAttribute();
    bool createMessageWindow();
    void destroyMessageWindow();

    void applyKeyAction(ITfContext* context, const KeyAction& action);
    void onCandidateClicked(size_t index);
    void onPageChange(int delta);
    void openSettingsWindow();
    void applySettings(const AppSettings& newSettings);
    void requestCaretRect(ITfContext* context);
    void resetAllState(ITfContext* context = nullptr);
    void finishShiftTap(ITfContext* context, WPARAM wParam, LPARAM lParam);

    KeyInput translateKey(WPARAM wParam, LPARAM lParam) const;
    static std::wstring utf8ToWide(std::string_view utf8);

    std::atomic<LONG> refCount_{1};
    ITfThreadMgr* threadMgr_ = nullptr;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    DWORD threadMgrSinkCookie_ = TF_INVALID_COOKIE;

    ITfComposition* composition_ = nullptr;
    ITfContext* activeContext_ = nullptr;
    TfGuidAtom displayAttrAtom_ = TF_INVALID_GUIDATOM;

    HWND msgHwnd_ = nullptr;
    AppSettings settings_;
    std::unique_ptr<FangCunEngine> engine_;
    std::unique_ptr<KeyHandler> keyHandler_;
    std::unique_ptr<CandidateWindow> candidateWindow_;
    std::unique_ptr<SettingsWindow> settingsWindow_;

    std::mutex asyncMutex_;
    std::vector<AsyncPayload> asyncQueue_;
    ShiftTapTracker shiftTap_;
};

} // namespace fangcun::tsf
