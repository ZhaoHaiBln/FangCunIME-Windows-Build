#include "DllMain.hpp"
#include "Guid.hpp"

#include <msctf.h>
#include <objbase.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <windows.h>

namespace fangcun::tsf {

namespace {

constexpr wchar_t kServiceName[] = L"方寸输入法";
constexpr wchar_t kProfileDesc[] = L"方寸输入法";

// {3A4259AC-640D-4AD4-89F7-1EB67E7C4EE8}
constexpr GUID kGuidTfcatTipcapImmersiveEnabled = {
    0x3a4259ac, 0x640d, 0x4ad4, {0x89, 0xf7, 0x1e, 0xb6, 0x7e, 0x7c, 0x4e, 0xe8}
};

// {25504FB4-7BAB-4BC1-9C69-CF81890F0EF5}
constexpr GUID kGuidTfcatTipcapSystraySupport = {
    0x25504fb4, 0x7bab, 0x4bc1, {0x9c, 0x69, 0xcf, 0x81, 0x89, 0x0f, 0x0e, 0xf5}
};

const GUID kSupportedCategories[] = {
    GUID_TFCAT_TIP_KEYBOARD,
    GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    GUID_TFCAT_TIPCAP_SECUREMODE,
    GUID_TFCAT_TIPCAP_COMLESS,
    GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
    kGuidTfcatTipcapImmersiveEnabled,
    kGuidTfcatTipcapSystraySupport,
};

bool clsidToString(REFCLSID clsid, wchar_t* buf, size_t cch) {
    return StringFromGUID2(clsid, buf, static_cast<int>(cch)) > 0;
}

bool registerComServer() {
    wchar_t clsidStr[64] = {};
    if (!clsidToString(CLSID_FangCunTextService, clsidStr, _countof(clsidStr))) return false;

    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hInstance, modulePath, _countof(modulePath)) == 0) return false;

    wchar_t keyPath[128] = {};
    StringCchPrintfW(keyPath, _countof(keyPath), L"CLSID\\%s", clsidStr);

    HKEY hClsidKey = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &hClsidKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    RegSetValueExW(hClsidKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(kServiceName),
                   static_cast<DWORD>((wcslen(kServiceName) + 1) * sizeof(wchar_t)));

    HKEY hInprocKey = nullptr;
    bool ok = false;
    if (RegCreateKeyExW(hClsidKey, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &hInprocKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hInprocKey, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(modulePath),
                       static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t)));
        constexpr wchar_t kThreading[] = L"Apartment";
        RegSetValueExW(hInprocKey, L"ThreadingModel", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(kThreading),
                       static_cast<DWORD>((wcslen(kThreading) + 1) * sizeof(wchar_t)));
        RegCloseKey(hInprocKey);
        ok = true;
    }
    RegCloseKey(hClsidKey);
    return ok;
}

void unregisterComServer() {
    wchar_t clsidStr[64] = {};
    if (!clsidToString(CLSID_FangCunTextService, clsidStr, _countof(clsidStr))) return;
    wchar_t keyPath[128] = {};
    StringCchPrintfW(keyPath, _countof(keyPath), L"CLSID\\%s", clsidStr);
    SHDeleteKeyW(HKEY_CLASSES_ROOT, keyPath);
}

bool registerProfileAndCategories() {
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hInstance, modulePath, _countof(modulePath)) == 0) return false;

    ITfInputProcessorProfileMgr* profileMgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&profileMgr));
    if (FAILED(hr) || !profileMgr) return false;

    hr = profileMgr->RegisterProfile(
        CLSID_FangCunTextService,
        kLangIdZhCn,
        GUID_FangCunProfile_Flypy,
        kProfileDesc,
        static_cast<ULONG>(wcslen(kProfileDesc)),
        modulePath,
        static_cast<ULONG>(wcslen(modulePath)),
        0,
        nullptr,
        0,
        TRUE,
        0);
    profileMgr->Release();
    if (FAILED(hr)) return false;

    ITfCategoryMgr* catMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr, reinterpret_cast<void**>(&catMgr));
    if (FAILED(hr) || !catMgr) return false;

    for (const GUID& cat : kSupportedCategories) {
        catMgr->RegisterCategory(CLSID_FangCunTextService, cat, CLSID_FangCunTextService);
    }
    catMgr->Release();
    return true;
}

void unregisterProfileAndCategories() {
    ITfCategoryMgr* catMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, reinterpret_cast<void**>(&catMgr))) && catMgr) {
        for (const GUID& cat : kSupportedCategories) {
            catMgr->UnregisterCategory(CLSID_FangCunTextService, cat, CLSID_FangCunTextService);
        }
        catMgr->Release();
    }

    ITfInputProcessorProfileMgr* profileMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&profileMgr))) && profileMgr) {
        profileMgr->UnregisterProfile(CLSID_FangCunTextService, kLangIdZhCn, GUID_FangCunProfile_Flypy, 0);
        profileMgr->Release();
    }
}

} // namespace

} // namespace fangcun::tsf

STDAPI DllRegisterServer() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comOk = fangcun::tsf::registerComServer();
    bool tsfOk = comOk && fangcun::tsf::registerProfileAndCategories();
    if (SUCCEEDED(coHr)) CoUninitialize();
    return tsfOk ? S_OK : E_FAIL;
}

STDAPI DllUnregisterServer() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    fangcun::tsf::unregisterProfileAndCategories();
    fangcun::tsf::unregisterComServer();
    if (SUCCEEDED(coHr)) CoUninitialize();
    return S_OK;
}
