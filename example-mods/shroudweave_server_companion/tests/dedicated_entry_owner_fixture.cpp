#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

#pragma comment(linker, "/BASE:0x1B0000000")

namespace {
volatile LONG callback_count{};
}

extern "C" __declspec(dllexport) void __cdecl EsmStartupOwnerReplacement() {
    InterlockedIncrement(&callback_count);
}

extern "C" __declspec(dllexport) LONG __cdecl EsmStartupOwnerCount() {
    return InterlockedCompareExchange(&callback_count, 0, 0);
}

extern "C" __declspec(dllexport) void __cdecl EsmStartupOwnerReset() {
    InterlockedExchange(&callback_count, 0);
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
EsmStartupOwnerFinalValidator(void*) {
    InterlockedIncrement(&callback_count);
    return 0;
}
