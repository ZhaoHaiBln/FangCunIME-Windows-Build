#pragma once
#include "fangcun/BuiltinSkinStyle.hpp"

namespace fangcun::tsf {
inline const std::optional<ColorArgb>& builtinColor(
    const skin::BuiltinStyleTone& tone, skin::ColorRole role) {
    return tone.colors[static_cast<size_t>(role)];
}
inline uint32_t builtinArgb(ColorArgb c) {
    return (uint32_t(c.a)<<24)|(uint32_t(c.r)<<16)|(uint32_t(c.g)<<8)|c.b;
}
// Overlay individual values; routing through the local pack palette would
// collapse distinct builtin border/chrome colors even on a font-only edit.
inline FangCunColors builtinColors(const skin::BuiltinStyleTone& edit, FangCunColors base) {
    auto set=[&](skin::ColorRole role,ColorArgb& target){if(auto c=builtinColor(edit,role))target=*c;};
    set(skin::ColorRole::Background,base.keyboard);set(skin::ColorRole::Surface,base.key);
    set(skin::ColorRole::Border,base.border);set(skin::ColorRole::Function,base.functionKey);
    set(skin::ColorRole::Text,base.text);set(skin::ColorRole::Muted,base.secondary);
    set(skin::ColorRole::Accent,base.accent);set(skin::ColorRole::Pressed,base.pressed);
    set(skin::ColorRole::OnAccent,base.onAccent);set(skin::ColorRole::OnSelection,base.onCandidate);
    return base;
}
inline SharpIme::WinPalette builtinPalette(const skin::BuiltinStyleTone& edit, SharpIme::WinPalette base) {
    auto set=[&](skin::ColorRole role,uint32_t& target){if(auto c=builtinColor(edit,role))target=builtinArgb(*c);};
    set(skin::ColorRole::CandidateBackground,base.windowBg);set(skin::ColorRole::Toolbar,base.windowHeaderBg);
    set(skin::ColorRole::CandidateText,base.textPrimary);set(skin::ColorRole::Muted,base.textSecondary);
    set(skin::ColorRole::Accent,base.accent);set(skin::ColorRole::OnAccent,base.accentText);
    set(skin::ColorRole::Selection,base.candidateActiveBg);set(skin::ColorRole::OnSelection,base.candidateActiveText);
    if(auto c=builtinColor(edit,skin::ColorRole::Border)) {
        base.windowOuterBorder=builtinArgb(*c);base.innerDivider=builtinArgb(*c);
    }
    return base;
}
inline bool sameBuiltinColor(ColorArgb a,ColorArgb b) {
    return a.a==b.a&&a.r==b.r&&a.g==b.g&&a.b==b.b;
}
inline uint32_t builtinMaterialTint(uint32_t original,const skin::BuiltinStyleTone* edit,
    skin::ColorRole role,ColorArgb originalTone) {
    if(!edit)return original;
    const auto& target=builtinColor(*edit,role);
    if(!target||sameBuiltinColor(*target,originalTone))return original;
    // Preserve highlights, grain and the original alpha while recoloring the
    // material. No edit follows this branch, so its pixels remain unchanged.
    auto c=ColorArgb::lerp(ColorArgb::fromHex(original),*target,.72f);
    c.a=static_cast<uint8_t>(original>>24);return builtinArgb(c);
}
inline const wchar_t* builtinFont(const std::optional<std::string>& font,const wchar_t* fallback) {
    if(!font)return fallback;
    if(*font=="mono")return L"Consolas";
    if(*font=="serif")return L"SimSun";
    return L"Microsoft YaHei UI";
}
} // namespace fangcun::tsf
