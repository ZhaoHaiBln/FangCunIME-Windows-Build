#include "EditSession.hpp"
#include "TextService.hpp"

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
    bool cancelOnly)
    : EditSessionBase(service, context),
      text_(std::move(text)),
      commit_(commit),
      cancelOnly_(cancelOnly) {}

STDMETHODIMP UpdateCompositionEditSession::DoEditSession(TfEditCookie ec) {
    if (!service_ || !context_) return E_FAIL;

    ITfComposition* comp = service_->composition();

    if (cancelOnly_) {
        if (comp) {
            ITfRange* range = nullptr;
            if (SUCCEEDED(comp->GetRange(&range)) && range) {
                range->SetText(ec, 0, L"", 0);
                range->Release();
            }
            comp->EndComposition(ec);
            service_->setComposition(nullptr);
        }
        return S_OK;
    }

    if (!comp) {
        ITfInsertAtSelection* insertAtSel = nullptr;
        if (FAILED(context_->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insertAtSel))) || !insertAtSel) {
            return E_FAIL;
        }
        ITfRange* range = nullptr;
        HRESULT hr = insertAtSel->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range);
        insertAtSel->Release();
        if (FAILED(hr) || !range) return E_FAIL;

        if (commit_) {
            range->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.size()));
            range->Collapse(ec, TF_ANCHOR_END);
            TF_SELECTION sel = {};
            sel.range = range;
            sel.style.ase = TF_AE_NONE;
            sel.style.fInterimChar = FALSE;
            context_->SetSelection(ec, 1, &sel);
            range->Release();
            return S_OK;
        }

        ITfContextComposition* ctxComp = nullptr;
        hr = context_->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void**>(&ctxComp));
        if (SUCCEEDED(hr) && ctxComp) {
            ITfComposition* newComp = nullptr;
            hr = ctxComp->StartComposition(ec, range, service_, &newComp);
            if (SUCCEEDED(hr) && newComp) {
                service_->setComposition(newComp);
                comp = newComp;
                newComp->Release();
            }
            ctxComp->Release();
        }
        range->Release();
        if (!comp) return E_FAIL;
    }

    ITfRange* compRange = nullptr;
    if (FAILED(comp->GetRange(&compRange)) || !compRange) return E_FAIL;

    compRange->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.size()));

    if (commit_) {
        compRange->Collapse(ec, TF_ANCHOR_END);
        TF_SELECTION sel = {};
        sel.range = compRange;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        context_->SetSelection(ec, 1, &sel);
        comp->EndComposition(ec);
        service_->setComposition(nullptr);
    } else {
        // Apply display attribute underline
        if (service_->displayAttributeAtom() != TF_INVALID_GUIDATOM) {
            ITfProperty* dispProp = nullptr;
            if (SUCCEEDED(context_->GetProperty(GUID_PROP_ATTRIBUTE, &dispProp)) && dispProp) {
                VARIANT var = {};
                var.vt = VT_I4;
                var.lVal = static_cast<LONG>(service_->displayAttributeAtom());
                dispProp->SetValue(ec, compRange, &var);
                dispProp->Release();
            }
        }

        // Collapse caret to end of composition
        ITfRange* selRange = nullptr;
        if (SUCCEEDED(compRange->Clone(&selRange)) && selRange) {
            selRange->Collapse(ec, TF_ANCHOR_END);
            TF_SELECTION sel = {};
            sel.range = selRange;
            sel.style.ase = TF_AE_NONE;
            sel.style.fInterimChar = FALSE;
            context_->SetSelection(ec, 1, &sel);
            selRange->Release();
        }

        // Query caret screen rect for CandidateWindow
        ITfContextView* view = nullptr;
        if (SUCCEEDED(context_->GetActiveView(&view)) && view) {
            RECT rc = {};
            BOOL clipped = FALSE;
            if (SUCCEEDED(view->GetTextExt(ec, compRange, &rc, &clipped))) {
                service_->updateCandidateWindowPosition(rc);
            }
            view->Release();
        }
    }

    compRange->Release();
    return S_OK;
}

GetCaretRectEditSession::GetCaretRectEditSession(TextService* service, ITfContext* context)
    : EditSessionBase(service, context) {}

STDMETHODIMP GetCaretRectEditSession::DoEditSession(TfEditCookie ec) {
    if (!service_ || !context_) return E_FAIL;

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
