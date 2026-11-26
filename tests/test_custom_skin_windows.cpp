#include "CustomSkinCache.hpp"
#include "BuiltinStyleDrawing.hpp"
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
        return hash.empty() ? !window.builtinSkin_ : window.builtinSkin_ && window.builtinSkin_->spec.hash == hash;
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
        const auto* style=window.activeBuiltinStyle();
        return style&&style->tone().colors[size_t(skin::ColorRole::CandidateText)]&&style->tone().colors[size_t(skin::ColorRole::CandidateText)]->r==red;
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
    // Builtin profiles are an independent, partial palette; they never become
    // a local image package or discard the existing physical skin renderer.
    const std::string profileHash(64,'c'),replacementHash(64,'d');
    const auto profileRoot=root/"builtin-styles"/"typewriter"/profileHash;
    const auto replacementRoot=root/"builtin-styles"/"typewriter"/replacementHash;
    std::filesystem::create_directories(profileRoot);std::filesystem::create_directories(replacementRoot);
    const auto profileIni="schema_version=1\nbuiltin=typewriter\nhash="+profileHash+"\nfont_family=mono\npress_offset_dp=1.25\nlight_candidate_text=#123456\ndark_candidate_text=#abcdef\nlight_glow_0=#111111\nlight_glow_1=#222222\nlight_glow_2=#333333\nlight_glow_3=#444444\n";
    const auto replacementIni="schema_version=1\nbuiltin=typewriter\nhash="+replacementHash+"\nlight_candidate_text=#654321\n";
    writeBytes(profileRoot/"resolved.ini",profileIni.data(),profileIni.size());
    writeBytes(replacementRoot/"resolved.ini",replacementIni.data(),replacementIni.size());
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"typewriter",profileHash,false);
    CHECK(waitNotification(hwnd),"Builtin style background worker finishes");
    auto builtin=cache.takeBuiltinReady();
    CHECK(builtin&&builtin->spec.builtin=="typewriter"&&builtin->spec.hash==profileHash&&!builtin->dark,"Builtin profile identity and light mode");
    CHECK(builtin->spec.fontFamily=="mono"&&builtin->spec.pressOffsetDp==1.25f,"Builtin font and physical press values survive loading");
    CHECK(builtin->tone().colors[size_t(ColorRole::CandidateText)]&&builtin->tone().colors[size_t(ColorRole::CandidateText)]->r==0x12&&builtin->tone().glow.has_value(),"Partial candidate color and four glow values survive loading");
    CHECK(!cache.takeReady(),"Builtin profile never masquerades as a local pack");
    CHECK(cache.takeBuiltinReady()==builtin,"Queued profile notifications keep the same latest immutable object");
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"typewriter",profileHash,true);CHECK(waitNotification(hwnd),"Builtin dark mode loads");
    builtin=cache.takeBuiltinReady();CHECK(builtin&&builtin->dark&&builtin->tone().colors[size_t(ColorRole::CandidateText)]&&builtin->tone().colors[size_t(ColorRole::CandidateText)]->r==0xab&&!builtin->tone().glow,"Builtin modes stay independent");
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"typewriter",profileHash,false);cache.requestBuiltinStyle(hwnd,root,"typewriter",replacementHash,false);
    CHECK(waitNotification(hwnd),"Replacement builtin profile finishes");builtin=cache.takeBuiltinReady();
    if(!builtin){CHECK(waitNotification(hwnd),"Latest builtin replacement notification arrives");builtin=cache.takeBuiltinReady();}
    CHECK(builtin&&builtin->spec.hash==replacementHash&&builtin->tone().colors[size_t(ColorRole::CandidateText)]&&builtin->tone().colors[size_t(ColorRole::CandidateText)]->r==0x65,"Old profile generation cannot overwrite a new saved revision");
    // Empty inherited edits preserve every builtin palette value and material.
    const BuiltinStyleTone untouched;
    for(auto preset:{fangcun::SkinPreset::WoodType,fangcun::SkinPreset::Typewriter,fangcun::SkinPreset::Terminal,fangcun::SkinPreset::Brutal})for(bool dark:{false,true}){
        const auto original=fangcun::skinColors(preset,dark),overlaid=builtinColors(untouched,original);
        CHECK(sameBuiltinColor(original.keyboard,overlaid.keyboard)&&sameBuiltinColor(original.key,overlaid.key)&&sameBuiltinColor(original.border,overlaid.border)&&sameBuiltinColor(original.functionKey,overlaid.functionKey)&&sameBuiltinColor(original.text,overlaid.text)&&sameBuiltinColor(original.secondary,overlaid.secondary)&&sameBuiltinColor(original.accent,overlaid.accent)&&sameBuiltinColor(original.pressed,overlaid.pressed)&&sameBuiltinColor(original.onAccent,overlaid.onAccent)&&sameBuiltinColor(original.onCandidate,overlaid.onCandidate)&&sameBuiltinColor(original.hardShadow,overlaid.hardShadow),"Inherited colors preserve original builtin drawing");
        const auto palette=fangcun::sharpPalette(preset,dark),result=builtinPalette(untouched,palette);
        CHECK(palette.windowBg==result.windowBg&&palette.windowHeaderBg==result.windowHeaderBg&&palette.windowOuterBorder==result.windowOuterBorder&&palette.innerDivider==result.innerDivider&&palette.textPrimary==result.textPrimary&&palette.textSecondary==result.textSecondary&&palette.accent==result.accent&&palette.accentText==result.accentText&&palette.candidateActiveBg==result.candidateActiveBg&&palette.candidateActiveText==result.candidateActiveText&&palette.hardShadowColor==result.hardShadowColor,"Inherited palette preserves its independently defined borders");
        CHECK(builtinMaterialTint(0x458c5f41,&untouched,ColorRole::Background,original.keyboard)==0x458c5f41,"No material edit is pixel-identical");
    }
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"","",false);CHECK(waitNotification(hwnd),"Reset cancels previous builtin profile");
    CHECK(!cache.takeBuiltinReady()&&!cache.takeReady(),"Reset exposes no stale builtin or pack snapshot");
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"../outside",profileHash,false);CHECK(waitNotification(hwnd)&&!cache.takeBuiltinReady(),"Builtin traversal identity rejected");
    const auto corrupted=profileIni+"external_file=outside.png\n";writeBytes(profileRoot/"resolved.ini",corrupted.data(),corrupted.size());
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"typewriter",profileHash,false);CHECK(waitNotification(hwnd)&&!cache.takeBuiltinReady(),"Unknown builtin file fields fail to original palette rather than old profile");
    const std::string oversized(16385,'x');writeBytes(profileRoot/"resolved.ini",oversized.data(),oversized.size());
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"typewriter",profileHash,false);CHECK(waitNotification(hwnd)&&!cache.takeBuiltinReady(),"Oversized builtin description is bounded");
    drainNotifications(hwnd);cache.requestBuiltinStyle(hwnd,root,"typewriter",replacementHash,false);cache.request(hwnd,root,"fangcun.test",hash,false,false);
    CHECK(waitNotification(hwnd),"Local pack supersedes builtin work");loaded=cache.takeReady();
    if(!loaded){CHECK(waitNotification(hwnd),"Latest local pack notification arrives");loaded=cache.takeReady();}
    CHECK(loaded&&loaded->spec.id=="fangcun.test"&&!cache.takeBuiltinReady(),"Local package wins and cannot receive builtin style");
    // Reproduce startup in saved dark mode: a profile finishes loading while
    // CandidateWindow still has its default light WoodType measurement cache.
    const std::string fontHash(64,'e'),colorOnlyHash(64,'f');
    const auto fontRoot=root/"builtin-styles"/"wood_type"/fontHash;
    const auto colorOnlyRoot=root/"builtin-styles"/"wood_type"/colorOnlyHash;
    std::filesystem::create_directories(fontRoot);std::filesystem::create_directories(colorOnlyRoot);
    const auto fontIni="schema_version=1\nbuiltin=wood_type\nhash="+fontHash+"\nfont_family=mono\ndark_candidate_text=#123456\n";
    const auto colorOnlyIni="schema_version=1\nbuiltin=wood_type\nhash="+colorOnlyHash+"\nfont_family=mono\ndark_candidate_text=#654321\n";
    writeBytes(fontRoot/"resolved.ini",fontIni.data(),fontIni.size());writeBytes(colorOnlyRoot/"resolved.ini",colorOnlyIni.data(),colorOnlyIni.size());
    CandidateWindow candidate([](size_t){});CHECK(candidate.create(nullptr),"Create real hidden candidate window and DirectWrite formats");
    CHECK(CandidateWindowTestAccess::family(candidate)==L"SimSun","Startup precondition: default WoodType uses its original serif family");
    candidate.setBuiltinStyle(root,"wood_type",fontHash,true);
    CHECK(waitCandidateStyle(candidate,fontHash),"Saved dark builtin profile loads before first key");
    CHECK(CandidateWindowTestAccess::family(candidate)==L"SimSun","Before first key the default light view still measures its original family");
    candidate.update("abc",{"entry"},0,false,fangcun::InputScheme::Flypy,fangcun::SkinPreset::WoodType,true,20,false);
    CHECK(CandidateWindowTestAccess::family(candidate)==L"Consolas"&&CandidateWindowTestAccess::family(candidate,true)==L"Consolas","First dark-mode key activates the saved profile font without skin/size changes");
    Microsoft::WRL::ComPtr<IDWriteTextFormat> measured{CandidateWindowTestAccess::primary(candidate)};
    candidate.update("abcd",{"entry"},0,false,fangcun::InputScheme::Flypy,fangcun::SkinPreset::WoodType,true,20,false);
    CHECK(measured.Get()==CandidateWindowTestAccess::primary(candidate),"Ordinary keys reuse the measured effective font budget");
    candidate.setBuiltinStyle(root,"wood_type",colorOnlyHash,true);
    CHECK(waitCandidateStyle(candidate,colorOnlyHash)&&CandidateWindowTestAccess::hasCandidateColor(candidate,0x65),"A color-only revision is actually loaded");
    CHECK(measured.Get()==CandidateWindowTestAccess::primary(candidate),"A same-family color-only revision does not recreate text formats");
    candidate.setBuiltinStyle(root,"wood_type","",true);CHECK(waitCandidateStyle(candidate,""),"Builtin reset completion arrives");
    CHECK(CandidateWindowTestAccess::family(candidate)==L"SimSun","Reset restores the original family without requiring another key");
    candidate.destroy();
    cache.stop();std::cout<<"[PASS] Windows WIC, builtin profile replacement, reset, local isolation and real font budget checks passed\n";
}
