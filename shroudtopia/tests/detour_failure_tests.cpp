#include "detour_service.h"
#include <windows.h>
#include <MinHook.h>
#include <cstdio>
extern "C" int detour_fixture_target(int);
unsigned creates{}, enables{}, removes{}, uninitializes{};
void* enabled_target;
__declspec(noinline) int replacement(int value) { return value+100; }
extern "C" MH_STATUS WINAPI MH_Initialize() { return MH_OK; }
extern "C" MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID* original) { ++creates; *original=reinterpret_cast<void*>(&detour_fixture_target); return MH_OK; }
extern "C" MH_STATUS WINAPI MH_EnableHook(LPVOID target) { ++enables; enabled_target=target; return MH_ERROR_MEMORY_PROTECT; }
extern "C" MH_STATUS WINAPI MH_RemoveHook(LPVOID) { ++removes; return MH_OK; }
extern "C" MH_STATUS WINAPI MH_Uninitialize() { ++uninitializes; return MH_OK; }
int main() {
    void* original{};
    for(unsigned attempt=0;attempt<2;++attempt)
        if(ModContext_CreateDetour(reinterpret_cast<void*>(&detour_fixture_target),reinterpret_cast<void*>(&replacement),&original) || original) return 1;
    if(creates!=1 || enables!=1 || enabled_target!=reinterpret_cast<void*>(&detour_fixture_target) || removes || uninitializes) return 2;
    std::puts("PASS failed enable retains owned closure and refuses retry; exact target only"); return 0;
}
