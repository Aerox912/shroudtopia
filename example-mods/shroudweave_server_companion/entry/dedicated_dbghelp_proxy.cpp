#include "dedicated_entry_gate.hpp"
#if defined(ESM_DEDICATED_GATE_FIXTURES)
#include <dbghelp.h>
#else
#include "dbghelp_forward.hpp"
#endif
#include <intrin.h>

#if defined(ESM_DEDICATED_GATE_FIXTURES)
namespace {
INIT_ONCE provider_once = INIT_ONCE_STATIC_INIT;
decltype(&MiniDumpWriteDump) provider{};
BOOL CALLBACK resolve_provider(PINIT_ONCE, PVOID, PVOID*) noexcept {
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length + 13 >= MAX_PATH) return FALSE;
    lstrcatW(system, L"\\dbghelp.dll");
    const HMODULE genuine = LoadLibraryExW(system, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!genuine) return FALSE;
    provider = reinterpret_cast<decltype(provider)>(GetProcAddress(genuine, "MiniDumpWriteDump"));
    return provider != nullptr;
}
}
#endif

extern "C" BOOL WINAPI EsmMiniDumpWriteDump(HANDLE process, DWORD id, HANDLE file, MINIDUMP_TYPE type,
    PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION streams,
    PMINIDUMP_CALLBACK_INFORMATION callback) {
#if defined(ESM_DEDICATED_GATE_FIXTURES)
    const DWORD incoming_error = GetLastError();
    if (!InitOnceExecuteOnce(&provider_once, resolve_provider, nullptr, nullptr)) return FALSE;
    SetLastError(incoming_error);
    return provider(process, id, file, type, exception, streams, callback);
#else
    return shroudweave::server::forward_minidump(process, id, file, type, exception, streams, callback);
#endif
}

#if !defined(ESM_DEDICATED_GATE_FIXTURES)
bool is_dedicated_name() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return true; // Unknown process identity fails closed.
    const wchar_t* name = path;
    for (DWORD i = 0; i < length; ++i) if (path[i] == L'\\') name = path + i + 1;
    return CompareStringOrdinal(name, -1, L"enshrouded_server.exe", -1, TRUE) == CSTR_EQUAL;
}
#endif

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    const bool startup = reserved != nullptr;
#if !defined(ESM_DEDICATED_GATE_FIXTURES)
    if (!is_dedicated_name()) return TRUE;
#endif
    if (esm::host::DedicatedEntryGate::arm(GetModuleHandleW(nullptr), startup)) return TRUE;
    if (!startup) return FALSE;
    OutputDebugStringW(L"ESM Shroudweave Server: bootstrap profile refused.\n");
    TerminateProcess(GetCurrentProcess(), 0xE5340001);
    __fastfail(7);
}
