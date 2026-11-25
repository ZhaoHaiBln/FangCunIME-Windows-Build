#include "EditSession.hpp"
#include "TextService.hpp"

#include <wrl/client.h>

namespace fangcun::tsf {

EditSessionBase::EditSessionBase(TextService* service, ITfContext* context)
    : service_(service), context_(context) {
    if (service_) service_->AddRef();
    if (context_) context_->AddRef();
}

EditSessionBase::~EditSessionBase() {
    if (context_) context_->Release();
    if (service_) service_->Release();
}

STDMETHODIMP EditSessionBase::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EditSessionBase::AddRef() {
    return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
}

STDMETHODIMP_(ULONG) EditSessionBase::Release() {
    LONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
}

UpdateCompositionEditSession::UpdateCompositionEditSession(
    TextService* service,
    ITfContext* context,
    std::wstring text,
    bool commit,
    bool cancelOnly,
    std::optional<std::wstring> followup)
    : EditSessionBase(service, context),
      text_(std::move(text)),
      commit_(commit),
      cancelOnly_(cancelOnly),
      followup_(std::move(followup)) {
    if (cancelOnly_ && service_) {
        expectedCancellation_ = service_->composition();
        if (expectedCancellation_) expectedCancellation_->AddRef();
    }
}

UpdateCompositionEditSession::~UpdateCompositionEditSession() {
    if (expectedCancellation_) expectedCancellation_->Release();
}

STDMETHODIMP UpdateCompositionEditSession::DoEditSession(TfEditCookie ec) {
    using Microsoft::WRL::ComPtr;
    if (!service_ || !context_) return E_FAIL;

    // Keep our own reference: EndComposition can synchronously invoke the sink.
    ComPtr<ITfComposition> comp = cancelOnly_ ? expectedCancellation_ : service_->composition();
    const auto endComposition = [&]() -> HRESULT {
        if (service_->composition() != comp.Get()) return E_UNEXPECTED;
        // An intentional end belongs to the prefix, not the new draft that may
        // follow it. The old callback must not reset the KeyHandler's tail.
        service_->setComposition(nullptr);
        const HRESULT hr = comp->EndComposition(ec);
        if (FAILED(hr) && !service_->composition()) service_->setComposition(comp.Get());
        return hr;
    };
    const auto selectionRange = [&](ComPtr<ITfRange>& range) -> HRESULT {
        ComPtr<ITfInsertAtSelection> insert;
        HRESULT hr = context_->QueryInterface(IID_ITfInsertAtSelection,
            reinterpret_cast<void**>(insert.GetAddressOf()));
        if (FAILED(hr)) return hr;
        if (!insert) return E_NOINTERFACE;
        hr = insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, range.GetAddressOf());
        return SUCCEEDED(hr) && !range ? E_FAIL : hr;
    };
    const auto selectEnd = [&](ITfRange* range, ComPtr<ITfRange>& end) -> HRESULT {
        HRESULT hr = range->Clone(end.GetAddressOf());
        if (FAILED(hr)) return hr;
        if (!end) return E_FAIL;
        hr = end->Collapse(ec, TF_ANCHOR_END);
        if (FAILED(hr)) return hr;
        TF_SELECTION sel = {};
        sel.range = end.Get();
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        // The collapsed range is authoritative for the following draft. Some
        // hosts reject explicit caret placement despite accepting SetText.
        // Do not turn their selection limitation into a failed text edit.
        context_->SetSelection(ec, 1, &sel);
        return S_OK;
    };

    if (cancelOnly_) {
        // Focus-loss cancellation can be scheduled asynchronously. It may only
        // erase the object that existed when cancellation was requested.
        if (!comp) return S_OK;
        ComPtr<ITfRange> range;
        HRESULT hr = comp->GetRange(range.GetAddressOf());
        if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;
        hr = range->SetText(ec, 0, L"", 0);
        if (FAILED(hr)) return hr;
        // resetAllState detaches this object before scheduling us. A later
        // composition may already belong to another document; never touch it.
        const bool wasCurrent = service_->composition() == comp.Get();
        if (wasCurrent) service_->setComposition(nullptr);
        hr = comp->EndComposition(ec);
        if (FAILED(hr) && wasCurrent && !service_->composition()) service_->setComposition(comp.Get());
        return hr;
    }

    ComPtr<ITfRange> followupPosition;
    if (commit_) {
        ComPtr<ITfRange> range;
        HRESULT hr = comp ? comp->GetRange(range.GetAddressOf()) : selectionRange(range);
        if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;
        hr = range->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.size()));
        if (FAILED(hr)) return hr;
        // Without a composition, SetText already inserted permanent text. A
        // later caret/followup failure must not insert it a second time.
        if (!comp) commitAccepted_ = true;
        hr = selectEnd(range.Get(), followupPosition);
        if (FAILED(hr)) return hr;
        if (comp) {
            hr = endComposition();
            if (FAILED(hr)) return hr;
            commitAccepted_ = true;
            comp.Reset();
        }
        if (!followup_ || followup_->empty()) {
            followupApplied_ = true;
            return S_OK;
        }
    }

    // The same edit cookie now starts/updates the tail after the accepted
    // prefix. There is no unlocked gap in which another editor can move it.
    const auto& draft = commit_ ? *followup_ : text_;
    comp = service_->composition();
    if (!comp) {
        ComPtr<ITfRange> range;
        HRESULT hr = followupPosition ? followupPosition->Clone(range.GetAddressOf()) : selectionRange(range);
        if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;
        ComPtr<ITfContextComposition> contextComposition;
        hr = context_->QueryInterface(IID_ITfContextComposition,
            reinterpret_cast<void**>(contextComposition.GetAddressOf()));
        if (FAILED(hr)) return hr;
        if (!contextComposition) return E_NOINTERFACE;
        hr = contextComposition->StartComposition(ec, range.Get(), service_, comp.GetAddressOf());
        if (FAILED(hr) || !comp) return FAILED(hr) ? hr : E_FAIL;
        service_->setComposition(comp.Get());
    }

    ComPtr<ITfRange> range;
    HRESULT hr = comp->GetRange(range.GetAddressOf());
    if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;
    hr = range->SetText(ec, 0, draft.c_str(), static_cast<LONG>(draft.size()));
    if (FAILED(hr)) return hr;
    ComPtr<ITfRange> caret;
    hr = selectEnd(range.Get(), caret);
    if (FAILED(hr)) return hr;
    followupApplied_ = true;

    // Underline and placement are decoration; a provider that does not support
    // them must not turn an accepted text edit into a failed commit.
    if (service_->displayAttributeAtom() != TF_INVALID_GUIDATOM) {
        ComPtr<ITfProperty> property;
        if (SUCCEEDED(context_->GetProperty(GUID_PROP_ATTRIBUTE, property.GetAddressOf())) && property) {
            VARIANT value = {};
            value.vt = VT_I4;
            value.lVal = static_cast<LONG>(service_->displayAttributeAtom());
            property->SetValue(ec, range.Get(), &value);
        }
    }
    ComPtr<ITfContextView> view;
    if (SUCCEEDED(context_->GetActiveView(view.GetAddressOf())) && view) {
        RECT rect = {};
        BOOL clipped = FALSE;
        if (SUCCEEDED(view->GetTextExt(ec, range.Get(), &rect, &clipped))) {
            service_->updateCandidateWindowPosition(rect);
        }
    }
    return S_OK;
}

GetCaretRectEditSession::GetCaretRectEditSession(TextService* service, ITfContext* context)
    : EditSessionBase(service, context), expectedComposition_(service ? service->composition() : nullptr) {
    if (expectedComposition_) expectedComposition_->AddRef();
}

GetCaretRectEditSession::~GetCaretRectEditSession() {
    if (expectedComposition_) expectedComposition_->Release();
}

STDMETHODIMP GetCaretRectEditSession::DoEditSession(TfEditCookie ec) {
    if (!service_ || !context_) return E_FAIL;
    if (service_->activeContext() != context_ || service_->composition() != expectedComposition_) return S_OK;

    ITfRange* range = nullptr;
    if (service_->composition()) {
        service_->composition()->GetRange(&range);
    } else {
        TF_SELECTION sel = {};
        ULONG fetched = 0;
        if (SUCCEEDED(context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) && fetched == 1) {
            range = sel.range;
        }
    }

    if (range) {
        ITfContextView* view = nullptr;
        if (SUCCEEDED(context_->GetActiveView(&view)) && view) {
            RECT rc = {};
            BOOL clipped = FALSE;
            if (SUCCEEDED(view->GetTextExt(ec, range, &rc, &clipped))) {
                service_->updateCandidateWindowPosition(rc);
            }
            view->Release();
        }
        range->Release();
    }
    return S_OK;
}

} // namespace fangcun::tsf
