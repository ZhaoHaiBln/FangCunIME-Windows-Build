#include "CustomSkinCache.hpp"
#include "CandidateWindow.hpp"
#include <wrl/client.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <thread>
#include <stdexcept>
namespace fangcun::tsf {
HINSTANCE g_hInstance = GetModuleHandleW(nullptr);
struct CandidateWindowTestAccess {
    static HWND hwnd(const CandidateWindow& window) { return window.hwnd_; }
    static bool loaded(const CandidateWindow& window, const std::string& hash) {
        return hash.empty() ? !window.customSkin_ : window.customSkin_ && window.customSkin_->spec.hash == hash;
    }
    static IDWriteTextFormat* primary(const CandidateWindow& window) { return window.primaryFormat_; }
    static std::wstring family(const CandidateWindow& window, bool code=false) {
        auto* format = code ? window.codeFormat_ : window.primaryFormat_;
        if (!format) return {};
        std::wstring name(static_cast<size_t>(format->GetFontFamilyNameLength()) + 1, L'\0');
        if (FAILED(format->GetFontFamilyName(name.data(), static_cast<UINT32>(name.size())))) return {};
        name.resize(name.size()-1);return name;
    }
    static bool hasCandidateColor(const CandidateWindow& window,uint8_t red) {
        return window.customSkin_ && window.customSkin_->theme().color(skin::ColorRole::CandidateText).r == red;
    }
    static bool neutral(const CandidateWindow& window) {
        return window.skin_ == fangcun::SkinPreset::SystemDefault && !window.activeBuiltinStyle();
    }
};
}
using namespace fangcun::tsf;
using namespace fangcun::skin;
#define CHECK(c,m) do{if(!(c)){std::cerr<<"[FAIL] "<<m<<" at "<<__LINE__<<'\n';return 1;}}while(false)
std::string iniFixture(const std::string& hash){
    std::string s="schema_version=1\nid=fangcun.test\nhash="+hash+"\nbase_skin=mechanical\nasset_scale=2\nfont_family=sans\npress_offset_dp=1\n";
    for(const auto mode:{"light_","dark_"}){
        for(const auto key:colorNames)s+=std::string(mode)+std::string(key)+"=#123456\n";
        for(int i=0;i<4;++i)s+=std::string(mode)+"glow_"+std::to_string(i)+"=#abcdef\n";
        s+=std::string(mode)+"image_candidates=light/paper.png\n"+mode+"scale_candidates=stretch\n"+mode+"insets_candidates=0,0,0,0\n";
    }return s;
}
bool waitNotification(HWND hwnd){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(std::chrono::steady_clock::now()<deadline){MSG msg{};if(PeekMessageW(&msg,hwnd,CustomSkinCache::ReadyMessage,CustomSkinCache::ReadyMessage,PM_REMOVE))return true;MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_POSTMESSAGE);}return false;
}
void drainNotifications(HWND hwnd){MSG msg{};while(PeekMessageW(&msg,hwnd,CustomSkinCache::ReadyMessage,CustomSkinCache::ReadyMessage,PM_REMOVE)) {}}
bool waitCandidateStyle(CandidateWindow& window,const std::string& hash) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(std::chrono::steady_clock::now()<deadline) {
        MSG msg{};
        if(PeekMessageW(&msg,CandidateWindowTestAccess::hwnd(window),CustomSkinCache::ReadyMessage,CustomSkinCache::ReadyMessage,PM_REMOVE)) {
            DispatchMessageW(&msg);
            if(CandidateWindowTestAccess::loaded(window,hash))return true;
        }
        MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_POSTMESSAGE);
    }
    return false;
}
void writeBytes(const std::filesystem::path& path,const void* data,size_t size){std::ofstream out(path,std::ios::binary);out.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));if(!out)throw std::runtime_error("Cannot write owned fixture");}
int main(){
    wchar_t temporary[MAX_PATH]{},unique[MAX_PATH]{};CHECK(GetTempPathW(MAX_PATH,temporary)&&GetTempFileNameW(temporary,L"fcs",0,unique),"Create private temporary path");
    DeleteFileW(unique);const std::filesystem::path root(unique);std::filesystem::create_directory(root);
    struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code ec;std::filesystem::remove_all(root,ec);}}cleanup{root};
    const std::string hash(64,'a');const auto pack=root/"skins"/hash;std::filesystem::create_directories(pack/"light");
    const auto ini=iniFixture(hash);writeBytes(pack/"resolved.ini",ini.data(),ini.size());
    const unsigned char png[]{137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,0,0,0,20,73,68,65,84,120,156,99,248,207,192,240,31,8,27,24,128,52,24,0,0,68,210,9,120,182,135,35,75,0,0,0,0,73,69,78,68,174,66,96,130};
    writeBytes(pack/"light/paper.png",png,sizeof(png));
    const HWND hwnd=CreateWindowExW(0,L"STATIC",L"skin-test",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);CHECK(hwnd,"Create notification sink");
    struct WindowCleanup{HWND hwnd;~WindowCleanup(){DestroyWindow(hwnd);}}windowCleanup{hwnd};
    CustomSkinCache cache;cache.request(hwnd,root,"fangcun.test",hash,false);CHECK(waitNotification(hwnd),"Real WIC worker must finish");
    auto loaded=cache.takeReady();CHECK(loaded&&!loaded->dark,"Light package loaded on background worker");
    auto pixels=loaded->pixels[size_t(ImageRole::Candidates)];CHECK(pixels&&pixels->width==2&&pixels->height==2&&pixels->bgra.size()==16,"Real PNG decoded at exact size");
    CHECK(pixels->bgra[0]==0&&pixels->bgra[2]==255&&pixels->bgra[3]==255,"WIC converts to BGRA");
    CHECK(pixels->bgra[5]==128&&pixels->bgra[7]==128,"Transparent pixels use premultiplied alpha");
    CHECK(cache.takeReady()==loaded,"A second queued notification cannot erase a newer result");
    loaded.reset();pixels.reset();
    // Rapid requests are superseded without exposing stale light/identity data.
    cache.request(hwnd,root,"fangcun.missing",std::string(64,'b'),false);
    cache.request(hwnd,root,"fangcun.test",hash,true);CHECK(waitNotification(hwnd),"Replacement request must finish");
    loaded=cache.takeReady();
    if(!loaded){CHECK(waitNotification(hwnd),"Latest request notification must arrive");loaded=cache.takeReady();}
    CHECK(loaded&&loaded->dark&&loaded->spec.id=="fangcun.test","Superseded jobs may never apply to the newer request");
    // Missing/bad artwork leaves only the current package's palette, not old pixels.
    writeBytes(pack/"light/paper.png","not PNG",7);drainNotifications(hwnd);cache.request(hwnd,root,"fangcun.test",hash,false);CHECK(waitNotification(hwnd),"Broken image request finishes");
    loaded=cache.takeReady();CHECK(loaded&&!loaded->pixels[size_t(ImageRole::Candidates)]&&loaded->theme().color(ColorRole::Text).r==0x12,"Bad PNG safely uses package color fallback");
    auto unsafe=ini;unsafe.replace(unsafe.find("light/paper.png"),15,"../escape.png");writeBytes(pack/"resolved.ini",unsafe.data(),unsafe.size());
    drainNotifications(hwnd);cache.request(hwnd,root,"fangcun.test",hash,false);CHECK(waitNotification(hwnd)&&!cache.takeReady(),"Traversal rejected before opening external file");
    writeBytes(pack/"resolved.ini",ini.data(),ini.size());drainNotifications(hwnd);cache.request(hwnd,root,"fangcun.test",hash,false,false);CHECK(waitNotification(hwnd),"Palette-only fallback UI load");
    loaded=cache.takeReady();CHECK(loaded&&!loaded->pixels[size_t(ImageRole::Candidates)],"Palette-only mode never decodes artwork");
    // Retired editor data on disk cannot activate a named builtin or replace a
    // neutral/default font, even if an old caller invokes the compatibility API.
    const std::string profileHash(64,'c');
    const auto profileRoot=root/"builtin-styles"/"wood_type"/profileHash;
    std::filesystem::create_directories(profileRoot);
    const auto profileIni="schema_version=1\nbuiltin=wood_type\nhash="+profileHash+"\nfont_family=mono\ndark_candidate_text=#ff0000\n";
    writeBytes(profileRoot/"resolved.ini",profileIni.data(),profileIni.size());
    CandidateWindow candidate([](size_t){},[]{});
    CHECK(candidate.create(nullptr),"Real candidate HWND and DirectWrite factory are available");
    candidate.setBuiltinStyle(root,"wood_type",profileHash,true);
    candidate.update("a",{"啊","阿"},0,false,fangcun::InputScheme::Pinyin,fangcun::SkinPreset::WoodType,true,20,true);
    CHECK(CandidateWindowTestAccess::neutral(candidate),"Legacy builtin reference is ignored and no-package candidate remains neutral");
    CHECK(CandidateWindowTestAccess::family(candidate)!=L"Consolas","Retired builtin font never affects default candidates");

    // The existing first-key and color-only font-cache coverage now uses actual
    // local .fcskin-derived INI, rather than the removed builtin editor feature.
    const std::string fontHash(64,'e'),colorOnlyHash(64,'f');
    const auto fontRoot=root/"skins"/fontHash,colorRoot=root/"skins"/colorOnlyHash;
    std::filesystem::create_directories(fontRoot/"light");std::filesystem::create_directories(colorRoot/"light");
    auto fontIni=iniFixture(fontHash);fontIni.replace(fontIni.find("font_family=sans"),16,"font_family=mono");
    auto colorIni=iniFixture(colorOnlyHash);colorIni.replace(colorIni.find("font_family=sans"),16,"font_family=mono");
    const auto colorField=colorIni.find("dark_candidate_text=#123456");
    colorIni.replace(colorField,27,"dark_candidate_text=#654321");
    writeBytes(fontRoot/"resolved.ini",fontIni.data(),fontIni.size());writeBytes(colorRoot/"resolved.ini",colorIni.data(),colorIni.size());
    writeBytes(fontRoot/"light/paper.png",png,sizeof(png));writeBytes(colorRoot/"light/paper.png",png,sizeof(png));
    candidate.setCustomSkin(root,"fangcun.test",fontHash,true);
    CHECK(waitCandidateStyle(candidate,fontHash),"Saved dark local package loads before the first key");
    candidate.update("a",{"啊","阿"},0,false,fangcun::InputScheme::Pinyin,fangcun::SkinPreset::WoodType,true,20,true);
    CHECK(CandidateWindowTestAccess::family(candidate)==L"Consolas"&&CandidateWindowTestAccess::family(candidate,true)==L"Consolas","First key uses the package's saved monospace font");
    auto* format=CandidateWindowTestAccess::primary(candidate);
    candidate.update("ai",{"爱","矮"},0,false,fangcun::InputScheme::Pinyin,fangcun::SkinPreset::WoodType,true,20,true);
    CHECK(CandidateWindowTestAccess::primary(candidate)==format,"Further typing reuses the measured format");
    candidate.setBuiltinStyle(root,"wood_type",profileHash,true);
    CHECK(CandidateWindowTestAccess::primary(candidate)==format,"Retired builtin activation cannot replace a local format");
    candidate.setCustomSkin(root,"fangcun.test",colorOnlyHash,true);
    CHECK(waitCandidateStyle(candidate,colorOnlyHash),"Color-only local package replacement finishes");
    CHECK(CandidateWindowTestAccess::primary(candidate)==format&&CandidateWindowTestAccess::hasCandidateColor(candidate,0x65),"Only colors changed, so font formats remain cached");
    candidate.setCustomSkin(root,"","",true);
    CHECK(CandidateWindowTestAccess::neutral(candidate)&&CandidateWindowTestAccess::family(candidate)!=L"Consolas","Removing the last selected pack restores neutral type without a new key");
    candidate.destroy();
    cache.stop();std::cout<<"[PASS] Windows WIC, safe package colors, local font caching and retired builtin isolation checks passed\n";
    return 0;
}
