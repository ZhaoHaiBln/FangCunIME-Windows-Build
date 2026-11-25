#include "fangcun/ResolvedSkin.hpp"
#include <iostream>
#include <string>
#include <cmath>
#include <stdexcept>
#define CHECK(c,m) do{if(!(c)){std::cerr<<"[FAIL] "<<m<<" at "<<__LINE__<<'\n';return 1;}}while(false)
using namespace fangcun::skin;
std::string fixture(){
    std::string ini="schema_version=1\nid=fangcun.test\nhash="+std::string(64,'a')+"\nbase_skin=typewriter\nasset_scale=3\nfont_family=sans\npress_offset_dp=1.5\n";
    for(const auto prefix:{"light_","dark_"}) {
        for(const auto key:colorNames)ini+=std::string(prefix)+std::string(key)+"=#123456\n";
        for(int i=0;i<4;++i)ini+=std::string(prefix)+"glow_"+std::to_string(i)+"=#abcdef\n";
    }
    ini+="light_image_candidates=light/paper.png\nlight_scale_candidates=nineSlice\nlight_insets_candidates=18,12,18,12\n";
    return ini;
}
std::string replaced(std::string data,const std::string& from,const std::string& to){auto at=data.find(from);if(at==std::string::npos)throw std::runtime_error("bad test fixture");data.replace(at,from.size(),to);return data;}
int main(){
    const auto ini=fixture();const auto hash=std::string(64,'a');const auto parsed=parseResolvedSkinIni(ini,"fangcun.test",hash);
    CHECK(parsed&&parsed->baseSkin==fangcun::SkinPreset::Typewriter&&parsed->assetScale==3&&parsed->fontFamily=="sans","Complete exporter contract must parse");
    CHECK(parsed->light.color(ColorRole::Text).r==0x12&&parsed->dark.glow[3].g==0xcd,"Both complete mode palettes retain exact colors");
    CHECK(parsed->light.images[size_t(ImageRole::Candidates)]->insets[0]==18&&!parsed->dark.images[0],"Missing role stays plain package color");
    const auto palette=fangcun::skin::palette(parsed->light);CHECK(palette.textPrimary==0xff123456&&palette.candidateActiveBg==0xff123456,"Native palette uses package colors");
    CHECK(!parseResolvedSkinIni(ini,"fangcun.wrong",hash)&&!parseResolvedSkinIni(ini,"fangcun.test",std::string(64,'b')),"Selection identity and immutable hash must match");
    CHECK(!parseResolvedSkinIni(ini+"schema_version=1\n","fangcun.test",hash),"Duplicate fields are not accepted");
    CHECK(!parseResolvedSkinIni(ini+"surprise=x\n","fangcun.test",hash),"Unknown exported fields are rejected");
    CHECK(!parseResolvedSkinIni(replaced(ini,"dark_text=#123456\n",""),"fangcun.test",hash),"Missing dark mode color cannot silently reuse light");
    CHECK(!parseResolvedSkinIni(replaced(ini,"#123456","red"),"fangcun.test",hash),"Expressions and color aliases cannot enter native drawing");
    for(const auto path:{"../escape.png","/root.png","a\\b.png","http://a.png","a//b.png","a/../b.png","A.png","a/.png"})CHECK(!validImagePath(path),"Unsafe image path must fail before I/O");
    for(const auto device:{"con","prn","aux","nul","com1","com2","com3","com4","com5","com6","com7","com8","com9","lpt1","lpt2","lpt3","lpt4","lpt5","lpt6","lpt7","lpt8","lpt9"}) {
        CHECK(!validImagePath(std::string(device)+".png")&&!validImagePath("light/"+std::string(device)+".png")&&!validImagePath("light/"+std::string(device)+"/paper.png"),"Windows device names are forbidden in every path component");
    }
    CHECK(validImagePath("com0.png")&&validImagePath("lpt10/paper.png")&&validImagePath("light/con-paper.png"),"Device-name rejection does not reject ordinary longer names");
    CHECK(validImagePath("dark/key_1.png")&&validId("fangcun.skin-one")&&!validId("fangcun..x")&&!validId("fangcun._x")&&!validId("Fang.cun"),"Exact namespace/path grammar");
    for(const auto bad:{"0","5","1.5","-1","nan"})CHECK(!parseResolvedSkinIni(replaced(ini,"asset_scale=3","asset_scale="+std::string(bad)),"fangcun.test",hash),"Asset scale is a bounded integer");
    for(const auto bad:{"nan","inf","-1","2.1","1oops"})CHECK(!parseResolvedSkinIni(replaced(ini,"press_offset_dp=1.5","press_offset_dp="+std::string(bad)),"fangcun.test",hash),"Press offset is finite and bounded");
    const auto tinyOffset=parseResolvedSkinIni(replaced(ini,"press_offset_dp=1.5","press_offset_dp=1e-100"),"fangcun.test",hash);
    CHECK(tinyOffset&&tinyOffset->pressOffsetDp==0,"A valid f64 offset below float precision may draw as zero without rejecting the pack");
    CHECK(!parseResolvedSkinIni(replaced(ini,"press_offset_dp=1.5","press_offset_dp=2.00000001"),"fangcun.test",hash),"Check the original f64 range before float rounding could hide an out-of-range offset");
    CHECK(!parseResolvedSkinIni(replaced(ini,"light_scale_candidates=nineSlice\n",""),"fangcun.test",hash),"Role fields are all-or-none");
    CHECK(!parseResolvedSkinIni(replaced(ini,"nineSlice","tile"),"fangcun.test",hash),"Non-nine-slice images cannot smuggle edge insets");
    CHECK(!parseResolvedSkinIni(ini+std::string(16384,'x'),"fangcun.test",hash),"Native snapshot is capped at 16KiB");
    auto image=*parsed->light.images[size_t(ImageRole::Candidates)];
    auto slices=imageSlices(image,120,90,3,200,50);CHECK(slices.size()==9,"Nine-slice has all nine patches at ordinary size");
    CHECK(std::abs(slices.front().dr-6)<.001&&std::abs(slices.front().db-4)<.001,"Insets are source pixels divided by assetScale");
    slices=imageSlices(image,120,90,3,5,3);CHECK(!slices.empty(),"Tiny destinations scale their borders instead of overflowing");
    for(const auto& s:slices)CHECK(s.dl>=0&&s.dt>=0&&s.dr<=5.0001&&s.db<=3.0001&&s.dr>s.dl&&s.db>s.dt,"Every tiny edge stays inside destination");
    image.insets={70,0,50,0};CHECK(imageSlices(image,120,90,3,100,20).empty(),"Invalid nine-slice center cannot divide by zero");
    image.scale=ImageScale::Contain;slices=imageSlices(image,100,50,1,100,100);CHECK(slices.size()==1&&slices[0].dt==25&&slices[0].db==75,"Contain centers without distorting aspect ratio");
    image.scale=ImageScale::Tile;slices=imageSlices(image,30,30,3,15,15);CHECK(slices.size()==4&&slices.back().sr==15&&slices.back().dr==15,"Tile crops partial source at natural dp size");
    CHECK(imageSlices(image,1,1,4,10000,10000).empty(),"Pathological tile count cannot block a paint frame");
    image.scale=ImageScale::Stretch;slices=imageSlices(image,10,20,2,7,9);CHECK(slices.size()==1&&slices[0].dr==7&&slices[0].db==9,"Stretch uses exact component bounds");
    CHECK(imageSlices(image,0,20,2,7,9).empty()&&imageSlices(image,10,20,0,7,9).empty(),"Invalid decoding metadata is harmless");
    std::cout<<"[PASS] Resolved skin contract and image geometry checks passed\n";
}
