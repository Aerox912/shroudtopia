#pragma once
#include <windows.h>
#include "process_detour_owner.hpp"

// Preserve the public ModContext callback signature. Success always supplies a
// real trampoline; failed/duplicate requests never borrow another hook's state.
inline bool ModContext_CreateDetour(LPVOID target, LPVOID detour, void** trampoline = nullptr) {
    if (trampoline) *trampoline = nullptr;
    const auto& owner = esm::host::detours::process_owner();
    return owner.context && owner.create_and_enable &&
        owner.create_and_enable(owner.context, target, detour, trampoline);
}
