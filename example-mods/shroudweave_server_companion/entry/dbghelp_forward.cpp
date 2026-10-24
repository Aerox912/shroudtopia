#include "dbghelp_forward.hpp"

namespace shroudweave::server {
namespace {
using Minidump = decltype(&MiniDumpWriteDump);
INIT_ONCE provider_once = INIT_ONCE_STATIC_INIT;
PVOID volatile provider{};

BOOL CALLBACK resolve_system_provider(PINIT_ONCE, PVOID, PVOID*) noexcept {
    if (InterlockedCompareExchangePointer(&provider, nullptr, nullptr)) return TRUE;
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length + 13 >= MAX_PATH) return FALSE;
    lstrcatW(system, L"\\dbghelp.dll");
    const HMODULE genuine = LoadLibraryExW(system, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!genuine) return FALSE;
    const auto resolved = reinterpret_cast<Minidump>(GetProcAddress(genuine, "MiniDumpWriteDump"));
    if (!resolved) return FALSE;
    InterlockedCompareExchangePointer(&provider, reinterpret_cast<PVOID>(resolved), nullptr);
    return TRUE;
}
}

bool set_dbghelp_chain(HMODULE module) noexcept {
    if (!module) return false;
    const auto next = reinterpret_cast<Minidump>(GetProcAddress(module, "MiniDumpWriteDump"));
    if (!next) return false;
    return InterlockedCompareExchangePointer(&provider, reinterpret_cast<PVOID>(next), nullptr) == nullptr;
}

BOOL WINAPI forward_minidump(HANDLE process, DWORD id, HANDLE file, MINIDUMP_TYPE type,
    PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION streams,
    PMINIDUMP_CALLBACK_INFORMATION callback) noexcept {
    const DWORD incoming_error = GetLastError();
    if (!InitOnceExecuteOnce(&provider_once, resolve_system_provider, nullptr, nullptr)) return FALSE;
    const auto next = reinterpret_cast<Minidump>(InterlockedCompareExchangePointer(&provider, nullptr, nullptr));
    if (!next) return FALSE;
    SetLastError(incoming_error);
    return next(process, id, file, type, exception, streams, callback);
}
}
