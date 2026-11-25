#pragma once

#include "fangcun/ThemeConfig.hpp"
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fangcun::skin {
enum class ImageRole : size_t { Deck, Toolbar, Code, Candidates, Selection, KeyNormal, KeyFunction, KeyAction, Count };
enum class ColorRole : size_t { Background, Surface, Border, Function, Text, Muted, Accent, OnAccent, Pressed, Toolbar, CandidateBackground, CandidateText, Selection, OnSelection, KeyText, KeyHint, FunctionText, CodeText, Count };
enum class ImageScale { Stretch, Contain, Tile, NineSlice };
inline constexpr std::array<std::string_view, 8> imageNames{"deck","toolbar","code","candidates","selection","key_normal","key_function","key_action"};
inline constexpr std::array<std::string_view, 18> colorNames{"background","surface","border","function","text","muted","accent","on_accent","pressed","toolbar","candidate_background","candidate_text","selection","on_selection","key_text","key_hint","function_text","code_text"};
struct ImageSpec { std::string file; ImageScale scale = ImageScale::Stretch; std::array<uint32_t,4> insets{}; };
struct Theme {
    std::array<ColorArgb,18> colors{};
    std::array<ColorArgb,4> glow{};
    std::array<std::optional<ImageSpec>,8> images{};
    ColorArgb color(ColorRole role) const { return colors[static_cast<size_t>(role)]; }
};
struct ResolvedSkin {
    std::string id, hash;
    SkinPreset baseSkin = SkinPreset::WoodType;
    int assetScale = 1;
    std::string fontFamily;
    float pressOffsetDp = 0;
    Theme light, dark;
};
inline bool validHash(std::string_view s) {
    return s.size()==64 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
inline bool validId(std::string_view s) {
    if(s.size()<3 || s.size()>64 || s[0]<'a'||s[0]>'z') return false;
    bool dot=false, componentStart=false;
    for(char c:s) {
        if(c=='.') { if(componentStart) return false; dot=true;componentStart=true;continue; }
        const bool alnum=(c>='a'&&c<='z')||(c>='0'&&c<='9');
        if(!alnum && (!dot || componentStart || (c!='_'&&c!='-'))) return false;
        componentStart=false;
    }
    return dot&&!componentStart;
}
inline bool validImagePath(std::string_view s) {
    if(s.size()<5||s.size()>96||s.substr(s.size()-4)!=".png") return false;
    auto stem=s.substr(0,s.size()-4); bool start=true;
    for(char c:stem) {
        if(c=='/') {if(start)return false;start=true;continue;}
        if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
        start=false;
    }
    if(start) return false;
    while(!stem.empty()) {
        const auto slash=stem.find('/');
        const auto component=stem.substr(0,slash);
        const bool numberedDevice=component.size()==4 &&
            (component.substr(0,3)=="com" || component.substr(0,3)=="lpt") &&
            component[3]>='1' && component[3]<='9';
        if(component=="con" || component=="prn" || component=="aux" || component=="nul" || numberedDevice) return false;
        if(slash==std::string_view::npos) break;
        stem.remove_prefix(slash+1);
    }
    return true;
}
inline bool unsignedValue(std::string_view s,uint32_t& result) {
    if(s.empty())return false;
    const auto parsed=std::from_chars(s.data(),s.data()+s.size(),result);
    return parsed.ec==std::errc{}&&parsed.ptr==s.data()+s.size();
}
inline std::optional<ColorArgb> parseColor(std::string_view s) {
    if(s.size()!=7||s[0]!='#')return {};
    uint32_t rgb=0;
    for(size_t i=1;i<7;++i) {char c=s[i];int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(n<0)return {};rgb=(rgb<<4)|n;}
    return ColorArgb::fromHex(0xFF000000|rgb);
}
inline std::optional<ResolvedSkin> parseResolvedSkinIni(std::string_view content,std::string_view id,std::string_view hash) {
    if(content.size()>16384||!validId(id)||!validHash(hash))return {};
    std::map<std::string,std::string> values;
    while(!content.empty()) {
        auto end=content.find('\n');auto line=content.substr(0,end);content=end==std::string_view::npos?std::string_view{}:content.substr(end+1);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        if(line.empty())continue;
        auto eq=line.find('=');if(eq==std::string_view::npos||eq==0)return {};
        for(unsigned char c:line)if(c<32||c==127)return {};
        if(!values.emplace(std::string(line.substr(0,eq)),std::string(line.substr(eq+1))).second)return {};
    }
    auto take=[&](const std::string& key)->std::optional<std::string>{auto it=values.find(key);if(it==values.end())return {};auto result=std::move(it->second);values.erase(it);return result;};
    auto schema=take("schema_version"),rid=take("id"),rhash=take("hash"),base=take("base_skin"),scale=take("asset_scale"),font=take("font_family"),press=take("press_offset_dp");
    if(!schema||*schema!="1"||!rid||*rid!=id||!rhash||*rhash!=hash||!base||!scale||!font||!press)return {};
    ResolvedSkin out;out.id=*rid;out.hash=*rhash;
    if(*base=="mechanical")out.baseSkin=SkinPreset::WoodType;else if(*base=="typewriter")out.baseSkin=SkinPreset::Typewriter;else if(*base=="mahjong")out.baseSkin=SkinPreset::Terminal;else if(*base=="brutal")out.baseSkin=SkinPreset::Brutal;else return {};
    uint32_t assetScale=0;if(!unsignedValue(*scale,assetScale)||assetScale<1||assetScale>4)return {};out.assetScale=static_cast<int>(assetScale);
    if(*font!="sans"&&*font!="serif"&&*font!="mono") return {};
    out.fontFamily=*font;
    // The manifest/exporter uses f64. Validate its original range before
    // narrowing for D2D; float underflow is harmless for a subpixel offset.
    double pressOffset=0;
    const auto parsed=std::from_chars(press->data(),press->data()+press->size(),pressOffset);
    if(parsed.ec!=std::errc{}||parsed.ptr!=press->data()+press->size()||!std::isfinite(pressOffset)||pressOffset<0||pressOffset>2)return {};
    out.pressOffsetDp=static_cast<float>(pressOffset);
    for(int dark=0;dark<2;++dark) {
        auto& theme=dark?out.dark:out.light;const std::string prefix=dark?"dark_":"light_";
        for(size_t i=0;i<colorNames.size();++i) {auto value=take(prefix+std::string(colorNames[i]));if(!value)return {};auto color=parseColor(*value);if(!color)return {};theme.colors[i]=*color;}
        for(size_t i=0;i<4;++i) {auto value=take(prefix+"glow_"+std::to_string(i));if(!value)return {};auto color=parseColor(*value);if(!color)return {};theme.glow[i]=*color;}
        for(size_t i=0;i<imageNames.size();++i) {
            auto file=take(prefix+"image_"+std::string(imageNames[i]));auto sizing=take(prefix+"scale_"+std::string(imageNames[i]));auto inset=take(prefix+"insets_"+std::string(imageNames[i]));
            if(!file&&!sizing&&!inset)continue;
            if(!file||!sizing||!inset||!validImagePath(*file))return {};
            ImageSpec image;image.file=*file;
            if(*sizing=="stretch")image.scale=ImageScale::Stretch;else if(*sizing=="contain")image.scale=ImageScale::Contain;else if(*sizing=="tile")image.scale=ImageScale::Tile;else if(*sizing=="nineSlice")image.scale=ImageScale::NineSlice;else return {};
            std::string_view rest=*inset;
            for(size_t n=0;n<4;++n) {auto comma=rest.find(',');if((n<3&&comma==std::string_view::npos)||(n==3&&comma!=std::string_view::npos))return {};if(!unsignedValue(rest.substr(0,comma),image.insets[n])||image.insets[n]>2048)return {};rest=comma==std::string_view::npos?std::string_view{}:rest.substr(comma+1);}
            if(image.scale!=ImageScale::NineSlice&&std::any_of(image.insets.begin(),image.insets.end(),[](uint32_t n){return n!=0;}))return {};
            theme.images[i]=std::move(image);
        }
    }
    if(!values.empty())return {};
    return out;
}
struct ImageSlice { float sl,st,sr,sb,dl,dt,dr,db; };
inline std::vector<ImageSlice> imageSlices(const ImageSpec& spec,uint32_t width,uint32_t height,int assetScale,float dw,float dh) {
    std::vector<ImageSlice> out;
    if(!width||!height||width>2048||height>2048||assetScale<1||assetScale>4||!std::isfinite(dw)||!std::isfinite(dh)||dw<=0||dh<=0)return out;
    const float w=static_cast<float>(width),h=static_cast<float>(height);
    if(spec.scale==ImageScale::Stretch)out.push_back({0,0,w,h,0,0,dw,dh});
    else if(spec.scale==ImageScale::Contain) {float k=std::min(dw/w,dh/h),tw=w*k,th=h*k;out.push_back({0,0,w,h,(dw-tw)/2,(dh-th)/2,(dw+tw)/2,(dh+th)/2});}
    else if(spec.scale==ImageScale::Tile) {
        const float tw=w/assetScale,th=h/assetScale;
        // Constant-memory rendering consumes this one tile descriptor and clips
        // partial tiles; callers cap pathological counts before iteration.
        const double count=std::ceil(dw/tw)*std::ceil(dh/th);if(count>16384)return out;
        for(float y=0;y<dh;y+=th)for(float x=0;x<dw;x+=tw) {float rw=std::min(tw,dw-x),rh=std::min(th,dh-y);out.push_back({0,0,rw*assetScale,rh*assetScale,x,y,x+rw,y+rh});}
    } else {
        const auto& n=spec.insets;if(n[0]+n[2]>=width||n[1]+n[3]>=height)return out;
        float l=n[0]/static_cast<float>(assetScale),t=n[1]/static_cast<float>(assetScale),r=n[2]/static_cast<float>(assetScale),b=n[3]/static_cast<float>(assetScale);
        float k=std::min({1.f,l+r>0?dw/(l+r):1.f,t+b>0?dh/(t+b):1.f});l*=k;t*=k;r*=k;b*=k;
        const std::array<float,4> sx{0.f,static_cast<float>(n[0]),w-n[2],w},sy{0.f,static_cast<float>(n[1]),h-n[3],h};
        const std::array<float,4> dx{0,l,dw-r,dw},dy{0,t,dh-b,dh};
        for(size_t y=0;y<3;++y)for(size_t x=0;x<3;++x)if(dx[x+1]>dx[x]&&dy[y+1]>dy[y])out.push_back({sx[x],sy[y],sx[x+1],sy[y+1],dx[x],dy[y],dx[x+1],dy[y+1]});
    }
    return out;
}
inline SharpIme::WinPalette palette(const Theme& t) {
    auto c=[&](ColorRole role){auto v=t.color(role);return (uint32_t(v.a)<<24)|(uint32_t(v.r)<<16)|(uint32_t(v.g)<<8)|v.b;};
    return {c(ColorRole::CandidateBackground),c(ColorRole::Toolbar),c(ColorRole::Border),c(ColorRole::Border),c(ColorRole::CandidateText),c(ColorRole::Muted),c(ColorRole::Accent),c(ColorRole::OnAccent),c(ColorRole::Selection),c(ColorRole::OnSelection),c(ColorRole::Border)};
}
inline FangCunColors colors(const Theme& t) {
    return {t.color(ColorRole::Background),t.color(ColorRole::Surface),t.color(ColorRole::Border),t.color(ColorRole::Function),t.color(ColorRole::Text),t.color(ColorRole::Muted),t.color(ColorRole::Accent),t.color(ColorRole::Pressed),t.color(ColorRole::OnAccent),t.color(ColorRole::OnSelection),t.color(ColorRole::Border)};
}
} // namespace fangcun::skin
