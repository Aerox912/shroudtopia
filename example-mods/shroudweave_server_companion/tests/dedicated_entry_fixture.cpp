#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>

#pragma function(memset)
extern "C" void* memset(void* target, int value, size_t count) {
    __stosb(static_cast<unsigned char*>(target), static_cast<unsigned char>(value), count);
    return target;
}

extern "C" void FixtureEntry();
extern "C" void EsmFixtureClobberXstate();
extern "C" __declspec(dllexport) void (*EsmFixtureEntryAddress)() = &FixtureEntry;
#if defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
#pragma comment(linker, "/BASE:0x140000000")
extern "C" void* EsmFixtureCallSiteAddresses[65];
extern "C" void (*EsmFixtureCallFunctionAddresses[65])();
extern "C" void EsmFixtureOriginalCall();
extern "C" __declspec(dllexport) void* const* __cdecl EsmFixtureGetCallSites() {
    return EsmFixtureCallSiteAddresses;
}
extern "C" __declspec(dllexport) void* __cdecl EsmFixtureGetOriginalCallAddress() {
    return reinterpret_cast<void*>(&EsmFixtureOriginalCall);
}
#endif
namespace {
CONTEXT captured{};
DWORD startup_thread{};
bool initialized{};
unsigned char captured_avx[256]{};
bool verify_avx{};
bool unrelated_handled{};
LONG CALLBACK unrelated_exception(PEXCEPTION_POINTERS exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    unrelated_handled = true;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void marker(const char* text, DWORD size) {
    DWORD written{};
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, size, &written, nullptr) || written != size)
        ExitProcess(90);
}
bool mode(const wchar_t* expected) {
    wchar_t value[64]{};
    GetEnvironmentVariableW(L"ESM_ENTRY_FIXTURE", value, 64);
    return lstrcmpW(value, expected) == 0;
}
DWORD WINAPI wrong_thread(void*) { FixtureEntry(); return 91; }
}

#ifdef ESM_DEDICATED_TLS_FIXTURE
void NTAPI fixture_tls(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) marker("TLS\n", 4);
}
#pragma section(".fixtls", read)
extern "C" __declspec(allocate(".fixtls")) const PIMAGE_TLS_CALLBACK fixture_tls_callbacks[2] = {fixture_tls, nullptr};
// The custom entry bypasses CRT startup; provide the PE TLS directory explicitly.
#pragma section(".rdata$T", read)
extern "C" {
DWORD fixture_tls_index{};
extern __declspec(allocate(".rdata$T")) const IMAGE_TLS_DIRECTORY64 _tls_used = {
    0, 0, reinterpret_cast<ULONGLONG>(&fixture_tls_index),
    reinterpret_cast<ULONGLONG>(&fixture_tls_callbacks[0]), 0, 0};
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#endif

extern "C" __declspec(dllexport) BOOL EsmFixtureInitialize(const CONTEXT* context, DWORD thread) {
    marker("INIT\n", 5);
    if (!context || context->Rip != reinterpret_cast<DWORD64>(&FixtureEntry) ||
        context->Rsp % 16 != 8 || thread != GetCurrentThreadId()) return FALSE;
    __movsb(reinterpret_cast<unsigned char*>(&captured), reinterpret_cast<const unsigned char*>(context), sizeof captured);
    startup_thread = thread;
    if (mode(L"unrelated")) {
        const PVOID handler = AddVectoredExceptionHandler(0, unrelated_exception);
        if (!handler) return FALSE;
        RaiseException(EXCEPTION_BREAKPOINT, 0, 0, nullptr);
        if (!unrelated_handled || !RemoveVectoredExceptionHandler(handler)) return FALSE;
        marker("UNRELATED\n", 10);
    }
    if (IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) {
        DWORD64 features{};
        if (!GetXStateFeaturesMask(const_cast<PCONTEXT>(context), &features)) return FALSE;
        if ((features & 4) != 0) {
            DWORD length{};
            const auto* avx = static_cast<const unsigned char*>(LocateXStateFeature(const_cast<PCONTEXT>(context), 2, &length));
            if (!avx || length != sizeof captured_avx) return FALSE;
            __movsb(captured_avx, avx, sizeof captured_avx);
        }
        bool seeded = false;
        for (unsigned i = 0; i != sizeof captured_avx; ++i) if (captured_avx[i]) seeded = true;
        if (mode(L"xstate") && !seeded) return FALSE;
        verify_avx = true;
    }
    // A worker must execute and finish outside the loader lock before release.
    if (mode(L"wrong-thread")) {
        const HANDLE worker = CreateThread(nullptr, 0, wrong_thread, nullptr, 0, nullptr);
        if (!worker) return FALSE;
        WaitForSingleObject(worker, 5000);
        CloseHandle(worker);
        return FALSE;
    }
    const HANDLE worker = CreateThread(nullptr, 0, [](void*) -> DWORD {
        HMODULE loaded = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loaded) return 1;
        FreeLibrary(loaded);
        return 0;
    }, nullptr, 0, nullptr);
    if (!worker) return FALSE;
    DWORD result = 1;
    const bool finished = WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(worker, &result) && result == 0;
    CloseHandle(worker);
    if (!finished || mode(L"failed")) return FALSE;
    void* frames[16]{};
    const USHORT count = CaptureStackBackTrace(0, 16, frames, nullptr);
    bool proxy_frame = false;
    bool outer_frame = false;
    const HMODULE bootstrap = GetModuleHandleW(L"dbghelp.dll");
    for (USHORT i = 0; i < count; ++i) {
        MEMORY_BASIC_INFORMATION frame{};
        if (!VirtualQuery(frames[i], &frame, sizeof frame)) return FALSE;
        if (frame.AllocationBase == bootstrap) proxy_frame = true;
        else if (proxy_frame) outer_frame = true;
    }
    if (!proxy_frame || !outer_frame) return FALSE;
    marker("UNWIND\n", 7);
#ifndef ESM_DEDICATED_DYNAMIC_FIXTURE
    if (mode(L"forward")) {
        wchar_t system[MAX_PATH]{};
        const UINT system_chars = GetSystemDirectoryW(system, MAX_PATH);
        if (!system_chars || system_chars + 13 >= MAX_PATH) return FALSE;
        lstrcatW(system, L"\\dbghelp.dll");
        HMODULE provider = LoadLibraryExW(system, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!provider) return FALSE;
        const auto genuine = reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(provider, "MiniDumpWriteDump"));
        if (!genuine || reinterpret_cast<void*>(genuine) == reinterpret_cast<void*>(&MiniDumpWriteDump)) return FALSE;
        SetLastError(0);
        const BOOL direct = genuine(GetCurrentProcess(), GetCurrentProcessId(), INVALID_HANDLE_VALUE, MiniDumpNormal, nullptr, nullptr, nullptr);
        const DWORD direct_error = GetLastError();
        SetLastError(0);
        const BOOL proxy = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), INVALID_HANDLE_VALUE, MiniDumpNormal, nullptr, nullptr, nullptr);
        if (proxy != direct || GetLastError() != direct_error) return FALSE;
        marker("FORWARD\n", 8);
    }
#endif
    initialized = true;
    marker("READY\n", 6);
    if (verify_avx) EsmFixtureClobberXstate();
    return TRUE;
}

extern "C" void EsmFixtureEntered(const unsigned char* stack) {
    marker("ENTRY\n", 6);
#if defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
    (void)stack;
    if (mode(L"native-no-commit")) {
        marker("NATIVE_READY\n", 13);
        ExitProcess(0);
    }
    if (mode(L"native-call") || mode(L"native-relay")) {
        const HMODULE owner = GetModuleHandleW(L"shroudweave_startup_owner.dll");
        using Count = LONG (__cdecl*)();
        const auto count = owner ? reinterpret_cast<Count>(GetProcAddress(owner, "EsmStartupOwnerCount")) : nullptr;
        if (!count) ExitProcess(101);
        EsmFixtureCallFunctionAddresses[0]();
        if (count() != 1) ExitProcess(102);
        if (mode(L"native-relay")) marker("NATIVE_RELAY\n", 13);
        else marker("NATIVE_CALL\n", 12);
        ExitProcess(0);
    }
    ExitProcess(103);
#elif defined(ESM_DEDICATED_DYNAMIC_FIXTURE)
    (void)stack;
    const HMODULE loaded = LoadLibraryW(L"dbghelp.dll");
    if (loaded) ExitProcess(92);
    marker("REJECTED\n", 9);
    ExitProcess(7);
#else
    if (!initialized || startup_thread != GetCurrentThreadId()) ExitProcess(93);
    const auto* regs = reinterpret_cast<const DWORD64*>(stack + 0x528);
    const DWORD64 expected[] = {captured.R15, captured.R14, captured.R13, captured.R12,
        captured.R11, captured.R10, captured.R9, captured.R8, captured.Rdi, captured.Rsi,
        captured.Rbp, captured.Rbx, captured.Rdx, captured.Rcx, captured.Rax};
    for (unsigned i = 0; i != 15; ++i) if (regs[i] != expected[i]) ExitProcess(94);
    if (reinterpret_cast<DWORD64>(regs + 16) != captured.Rsp ||
        (regs[15] & 0xffff) != (captured.EFlags & 0xffff)) ExitProcess(95);
    const auto* xmm = reinterpret_cast<const unsigned char*>(&captured.Xmm0);
    for (unsigned i = 0; i != 256; ++i) if (stack[0x20 + i] != xmm[i]) ExitProcess(96);
    if (*reinterpret_cast<const DWORD*>(stack + 0x120) != captured.MxCsr) ExitProcess(97);
    if (verify_avx) {
        const auto* xsave = *reinterpret_cast<const unsigned char* const*>(stack + 0x128);
        for (unsigned i = 0; i != sizeof captured_avx; ++i)
            if (xsave[576 + i] != captured_avx[i]) ExitProcess(100);
    }
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<void*>(&FixtureEntry), &page, sizeof page) ||
        page.Protect != PAGE_EXECUTE_READ || *reinterpret_cast<const unsigned char*>(&FixtureEntry) != 0x9c) ExitProcess(98);
    marker("CONTEXT\nNETWORK\n", 16);
    ExitProcess(0);
#endif
}

extern "C" __declspec(dllexport) void FixtureAlternate() { marker("ALTERNATE\n", 10); ExitProcess(99); }

#ifndef ESM_DEDICATED_DYNAMIC_FIXTURE
// Keep the normal PE import even when the selected case does not create a dump.
extern "C" __declspec(dllexport) auto fixture_dump_import = &MiniDumpWriteDump;
#endif
