#pragma once

#include <windows.h>

namespace fangcun::tsf {

extern HINSTANCE g_hInstance;

void DllAddRef();
void DllRelease();

} // namespace fangcun::tsf
