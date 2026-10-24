#include "dedicated_native_v1.h"
#include <windows.h>
#include <cstdio>
extern "C" uint32_t __cdecl EsmDedicatedGate_GetApiV1(uint32_t, uint32_t, EsmDedicatedNativeApiV1*);
int main() {
    EsmDedicatedNativeApiV1 api{};
    if (!GetModuleHandleW(L"dbghelp.dll") || EsmDedicatedGate_GetApiV1(1, sizeof api, &api) != 0 ||
        api.struct_size != sizeof api || api.abi_major != 1) return 1;
    std::puts("PASS: non-server startup imports the companion without arming the server gate");
}