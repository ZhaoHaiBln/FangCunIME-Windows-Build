#include "DllMain.hpp"
#include "Guid.hpp"
#include "SettingsWindow.hpp"
#include "TextService.hpp"

#include <atomic>
#include <new>

namespace fangcun::tsf {

HINSTANCE g_hInstance = nullptr;
static std::atomic<LONG> g_dllRefCount{0};

void DllAddRef() {
    g_dllRefCount.fetch_add(1, std::memory_order_relaxed);
}

void DllRelease() {
    g_dllRefCount.fetch_sub(1, std::memory_order_relaxed);
}

class ClassFactory : public IClassFactory {
public:
    ClassFactory() { DllAddRef(); }
    virtual ~ClassFactory() { DllRelease(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_INVALIDARG;
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppvObj = static_cast<IClassFactory*>(this);
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

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_INVALIDARG;
        *ppvObj = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

        TextService* service = new (std::nothrow) TextService();
        if (!service) return E_OUTOFMEMORY;

        HRESULT hr = service->QueryInterface(riid, ppvObj);
        service->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) DllAddRef();
        else DllRelease();
        return S_OK;
    }

private:
    std::atomic<LONG> refCount_{1};
};

} // namespace fangcun::tsf

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        fangcun::tsf::g_hInstance = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_INVALIDARG;
    *ppvObj = nullptr;
    if (!IsEqualCLSID(rclsid, fangcun::tsf::CLSID_FangCunTextService)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) fangcun::tsf::ClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    HRESULT hr = factory->QueryInterface(riid, ppvObj);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (fangcun::tsf::g_dllRefCount.load(std::memory_order_relaxed) == 0) ? S_OK : S_FALSE;
}

extern "C" void CALLBACK OpenSettings(HWND hwnd, HINSTANCE, LPSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    fangcun::tsf::SettingsWindow win;
    win.showOrActivate(hwnd);

    MSG msg = {};
    while (win.isOpen() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
}
