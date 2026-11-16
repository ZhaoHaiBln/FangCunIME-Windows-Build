#pragma once

#include <guiddef.h>
#include <windows.h>

namespace fangcun::tsf {

// {A7F6E5D4-3C2B-4A19-8F0E-1D2C3B4A5968}
inline constexpr CLSID CLSID_FangCunTextService = {
    0xa7f6e5d4, 0x3c2b, 0x4a19, {0x8f, 0x0e, 0x1d, 0x2c, 0x3b, 0x4a, 0x59, 0x68}
};

// {B8E7D6C5-4D3C-4B2A-9E1F-2E3D4C5B6A79}
inline constexpr GUID GUID_FangCunProfile_Flypy = {
    0xb8e7d6c5, 0x4d3c, 0x4b2a, {0x9e, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79}
};

// {C9F8E7D6-5E4D-4C3B-AF20-3F4E5D6C7B8A}
inline constexpr GUID GUID_FangCunDisplayAttributeInput = {
    0xc9f8e7d6, 0x5e4d, 0x4c3b, {0xaf, 0x20, 0x3f, 0x4e, 0x5d, 0x6c, 0x7b, 0x8a}
};

inline constexpr LANGID kLangIdZhCn = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

} // namespace fangcun::tsf
