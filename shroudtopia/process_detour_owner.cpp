#include "process_detour_owner.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cwchar>

namespace esm::host::detours {
namespace {
struct Hook final { void* target; void* detour; void* trampoline; bool enabled; };
struct Resident final {
    SRWLOCK gate = SRWLOCK_INIT;
    HANDLE admission{};
    bool initialization_attempted{}, ready{};
    unsigned count{};
    Hook hooks[4096]{};
};
// Trivial static storage has no detach destructor; every attempt retains code,
// target, detour and MinHook storage to process exit, including failed enables.
Resident resident;
bool pin_executable_image(void* address) noexcept {
    MEMORY_BASIC_INFORMATION region{}; HMODULE image{};
    if (!address || !VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        region.Type != MEM_IMAGE || (region.Protect != PAGE_EXECUTE_READ && region.Protect != PAGE_EXECUTE)) return false;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(address), &image) && image == region.AllocationBase;
}
bool initialize_owner(Resident& state) noexcept {
    if (state.initialization_attempted) return state.ready;
    state.initialization_attempted = true;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return false;
    wchar_t name[160]{};
    // No shared session pooling: this mutex only refuses a second native owner.
    const auto stamp = (static_cast<unsigned long long>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    if (swprintf_s(name, L"Local\\ESM.Detours.v1.%lu.%llu", GetCurrentProcessId(), stamp) < 0) return false;
    state.admission = CreateMutexW(nullptr, FALSE, name);
    const auto error = GetLastError();
    if (!state.admission || error == ERROR_ALREADY_EXISTS) return false;
    // ALREADY_INITIALIZED may belong to another consumer of this MinHook image.
    // Never enable its hooks or uninitialize someone else's state.
    state.ready = MH_Initialize() == MH_OK;
    return state.ready;
}
bool create_and_enable(void* context, void* target, void* detour, void** original) noexcept {
    if (original) *original = nullptr;
    if (context != &resident || !original || !pin_executable_image(target) || !pin_executable_image(detour) ||
        !pin_executable_image(reinterpret_cast<void*>(&create_and_enable))) return false;
    auto& state = resident;
    AcquireSRWLockExclusive(&state.gate);
    struct Unlock { SRWLOCK* gate; ~Unlock() { ReleaseSRWLockExclusive(gate); } } unlock{&state.gate};
    if (!initialize_owner(state) || state.count == sizeof(state.hooks)/sizeof(state.hooks[0])) return false;
    for (unsigned n=0; n<state.count; ++n) if (state.hooks[n].target == target) return false;
    void* trampoline{};
    if (MH_CreateHook(target, detour, &trampoline) != MH_OK) return false;
    auto& hook = state.hooks[state.count++]; hook = {target, detour, trampoline, false};
    // Exact owned target only. Failure can leave suspended-thread/patch state
    // uncertain, so preserve the record, trampoline and pinned images without retry.
    if (MH_EnableHook(target) != MH_OK) return false;
    hook.enabled = true; *original = trampoline; return true;
}
const OwnerV1 owner{sizeof(OwnerV1), 1, 0, &resident, create_and_enable};
}
const OwnerV1& process_owner() noexcept {
    static const OwnerV1 unavailable{sizeof(OwnerV1), 1, 0, nullptr, nullptr};
    return pin_executable_image(reinterpret_cast<void*>(&create_and_enable)) ? owner : unavailable;
}
}
