#pragma once

#include <msctf.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace fangcun::tsf {

class TextService;

class EditSessionBase : public ITfEditSession {
public:
    explicit EditSessionBase(TextService* service, ITfContext* context);
    virtual ~EditSessionBase();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

protected:
    TextService* service_;
    ITfContext* context_;
    std::atomic<LONG> refCount_{1};
};

/**
 * Updates the active TSF composition text (or starts one if needed),
 * applies the display attribute underline, and retrieves the caret bounding rect.
 */
class UpdateCompositionEditSession : public EditSessionBase {
public:
    UpdateCompositionEditSession(
        TextService* service,
        ITfContext* context,
        std::wstring text,
        bool commit,
        bool cancelOnly,
        std::optional<std::wstring> followup = std::nullopt);
    ~UpdateCompositionEditSession() override;

    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

    // A committed prefix must never be retried when only its following draft
    // failed. RequestEditSession's HRESULT alone cannot express that outcome.
    bool commitAccepted() const { return commitAccepted_; }
    bool followupApplied() const { return followupApplied_; }

private:
    std::wstring text_;
    bool commit_;
    bool cancelOnly_;
    std::optional<std::wstring> followup_;
    bool commitAccepted_ = false;
    bool followupApplied_ = false;
    ITfComposition* expectedCancellation_ = nullptr;
};

/**
 * Queries the current caret / composition bounding rectangle for CandidateWindow placement.
 */
class GetCaretRectEditSession : public EditSessionBase {
public:
    GetCaretRectEditSession(TextService* service, ITfContext* context);
    ~GetCaretRectEditSession() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    ITfComposition* expectedComposition_ = nullptr;
};

} // namespace fangcun::tsf
