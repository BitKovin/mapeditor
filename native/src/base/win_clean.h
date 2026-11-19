// base/win_clean -- force-included (/FI) into every module on MSVC: windows.h with its `near` / `far` macros removed.
// The Core has a member function called `near` (core/playable.h); Vulkan and Qt headers include windows.h wherever they like.
#pragma once
#ifdef _WIN32
#include <windows.h>
#undef near
#undef far
#endif
