#include "dedicated_entry_gate.hpp"
#include <intrin.h>
#include "dedicated_native_v1.h"
#if defined(ESM_DEDICATED_NATIVE_BRIDGE)
#include <bcrypt.h>
extern bool esm_dedicated_run_host(const EsmDedicatedNativeStartV1*);
#endif

extern "C" void EsmDedicatedEntryThunk();
extern "C" void EsmDedicatedRefusalThunk();

namespace {
enum class Phase : unsigned { empty, armed, redirected, released };
struct CallCommit {
    unsigned char* site;
    void* owner;
    const unsigned char* relay;
    const void* callback;
    unsigned char replacement[5];
    unsigned char relay_bytes[14];
};
struct GateState {
    Phase phase;
    DWORD thread;
    unsigned char* entry;
    unsigned char original;
    DWORD protection;
    PVOID handler;
    PCONTEXT context;
    EsmDedicatedNativeStartV1 witness;
    void* host;
    bool claimed;
    unsigned committed;
    CallCommit calls[64];
    volatile LONG active;
    HANDLE completion;
    ULONGLONG deadline;
#ifdef ESM_DEDICATED_GATE_FIXTURES
    bool seed_xstate;
#endif
};
GateState gate{};
alignas(64) unsigned char context_storage[65536];

bool native_current(const EsmDedicatedNativeStartV1* args) noexcept {
    return args&&args->struct_size==sizeof(*args)&&args->abi_major==1&&
        GetCurrentThreadId()==gate.thread&&gate.phase==Phase::redirected&&InterlockedCompareExchange(&gate.active,1,1)==1&&GetTickCount64()<gate.deadline&&
        args->proxy==gate.witness.proxy&&args->generation==gate.witness.generation&&
        args->witness[0]==gate.witness.witness[0]&&args->witness[1]==gate.witness.witness[1];
}
bool readable(const void* address, SIZE_T size) noexcept;

bool overlaps(const void* left, SIZE_T left_size, const void* right, SIZE_T right_size) noexcept {
    const auto a = reinterpret_cast<uintptr_t>(left), b = reinterpret_cast<uintptr_t>(right);
    return a <= b ? b - a < left_size : a - b < right_size;
}

// Both the RX mapping and PE section must be executable. No module/host data
// may be patched, and writable code is deliberately unsupported by this API.
bool image_code(const void* address, SIZE_T size, const void* module) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !module || !size || !VirtualQuery(address, &region, sizeof region) ||
        region.AllocationBase != module || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        region.Protect != PAGE_EXECUTE_READ || !readable(address, size)) return false;
    const auto* image = static_cast<const unsigned char*>(module);
    if (!readable(image, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000 ||
        !readable(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96) return false;
    const auto a = reinterpret_cast<uintptr_t>(address), base = reinterpret_cast<uintptr_t>(module);
    if (a < base || a - base >= nt->OptionalHeader.SizeOfImage ||
        size > nt->OptionalHeader.SizeOfImage - (a - base)) return false;
    const auto rva = a - base;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    if (!readable(section, nt->FileHeader.NumberOfSections * sizeof *section)) return false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section[i].VirtualAddress &&
            rva - section[i].VirtualAddress < section[i].Misc.VirtualSize &&
            size <= section[i].Misc.VirtualSize - (rva - section[i].VirtualAddress)) return true;
    }
    return false;
}

const void* call_target(const void* site, const unsigned char* bytes) noexcept {
    int32_t displacement{};
    __movsb(reinterpret_cast<unsigned char*>(&displacement), bytes + 1, 4);
    const auto base = reinterpret_cast<uintptr_t>(site) + 5;
    if (displacement < 0) {
        const auto magnitude = static_cast<uintptr_t>(-static_cast<int64_t>(displacement));
        return magnitude <= base ? reinterpret_cast<const void*>(base - magnitude) : nullptr;
    }
    return static_cast<uintptr_t>(displacement) <= UINTPTR_MAX - base ?
        reinterpret_cast<const void*>(base + displacement) : nullptr;
}

bool relay_destination(const unsigned char* relay, const void*& callback) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(relay, &region, sizeof region) || region.Type != MEM_PRIVATE ||
        region.State != MEM_COMMIT || region.Protect != PAGE_EXECUTE_READ || !readable(relay, 14) ||
        relay[0] != 0xff || relay[1] != 0x25) return false;
    for (unsigned i = 2; i < 6; ++i) if (relay[i]) return false;
    __movsb(reinterpret_cast<unsigned char*>(&callback), relay + 6, sizeof callback);
    return callback != nullptr;
}

bool owned_callback(const void* callback, void* owner, bool pin) noexcept {
    if (!owner || owner == GetModuleHandleW(nullptr) || owner == gate.witness.proxy ||
        !image_code(callback, 1, owner)) return false;
    HMODULE actual{};
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        (pin ? GET_MODULE_HANDLE_EX_FLAG_PIN : GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
    return GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(callback), &actual) && actual == owner;
}

bool commitments_valid() noexcept {
    for (unsigned i = 0; i < gate.committed; ++i) {
        const auto& record = gate.calls[i];
        if (!image_code(record.site, 5, GetModuleHandleW(nullptr)) ||
            !owned_callback(record.callback, record.owner, false)) return false;
        for (unsigned b = 0; b < 5; ++b)
            if (record.site[b] != record.replacement[b]) return false;
        if (record.relay) {
            const void* callback{};
            if (!relay_destination(record.relay, callback) || callback != record.callback) return false;
            for (unsigned b = 0; b < 14; ++b)
                if (record.relay[b] != record.relay_bytes[b]) return false;
        }
    }
    return true;
}

void native_view(EsmDedicatedNativeViewV1* out) noexcept {
    *out = {sizeof(*out), 1, GetModuleHandleW(nullptr), nullptr, gate.thread, gate.committed, gate.witness.generation};
}
uint32_t __cdecl native_claim(const EsmDedicatedNativeStartV1* args, void* host, EsmDedicatedNativeViewV1* out) noexcept {
    if (!out || !native_current(args) || gate.claimed || !host || host != gate.host) return 6;
    gate.claimed = true; native_view(out); return 0;
}
uint32_t __cdecl native_validate(const EsmDedicatedNativeStartV1* args, EsmDedicatedNativeViewV1* out) noexcept {
    if (!out || !native_current(args) || !gate.claimed) return 8;
    if (!commitments_valid()) return 6;
    native_view(out); return 0;
}
uint32_t __cdecl native_commit(const EsmDedicatedNativeStartV1* args, void* owner, void* site,
    const void* replacement, const void* expected, size_t size) noexcept {
    if (!native_current(args) || !gate.claimed || gate.committed == 64) return 8;
    if (!site || !owner || !replacement || !expected || size != 5 ||
        !readable(replacement, 5) || !readable(expected, 5)) return 2;
    if (!commitments_valid() || !image_code(site, 5, GetModuleHandleW(nullptr)) ||
        overlaps(site, 5, gate.entry, 18)) return 6;
    for (unsigned i = 0; i < gate.committed; ++i)
        if (overlaps(site, 5, gate.calls[i].site, 5)) return 6;
    unsigned char bytes[5]{}, prior[5]{};
    __movsb(bytes, static_cast<const unsigned char*>(replacement), 5);
    __movsb(prior, static_cast<const unsigned char*>(expected), 5);
    if (bytes[0] != 0xe8 || prior[0] != 0xe8) return 2;
    bool changed = false;
    for (unsigned i = 0; i < 5; ++i) {
        if (static_cast<const unsigned char*>(site)[i] != prior[i]) return 6;
        if (bytes[i] != prior[i]) changed = true;
    }
    if (!changed) return 2;
    if (!image_code(call_target(site, prior), 1, GetModuleHandleW(nullptr))) return 6;
    const auto* target = static_cast<const unsigned char*>(call_target(site, bytes));
    const void* callback = target;
    const unsigned char* relay{};
    if (!image_code(target, 1, owner)) {
        if (!relay_destination(target, callback)) return 6;
        relay = target;
    }
    if (!owned_callback(callback, owner, true)) return 6;
    DWORD protection{}, previous{};
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protection)) return 6;
    bool unchanged = protection == PAGE_EXECUTE_READ;
    for (unsigned i = 0; i < 5; ++i)
        if (static_cast<const unsigned char*>(site)[i] != prior[i]) unchanged = false;
    if (!unchanged) {
        if (!VirtualProtect(site, 5, protection, &previous)) {
            TerminateProcess(GetCurrentProcess(), 0xE5340001); __fastfail(7);
        }
        return 6;
    }
    // Holding entry is not an all-worker suspension or an atomic write.
    for (unsigned i = 0; i < 5; ++i) static_cast<volatile unsigned char*>(site)[i] = bytes[i];
    auto& record = gate.calls[gate.committed];
    record.site = static_cast<unsigned char*>(site);
    record.owner = owner; record.relay = relay; record.callback = callback;
    __movsb(record.replacement, bytes, 5);
    if (relay) __movsb(record.relay_bytes, relay, 14);
    ++gate.committed;
    if (!FlushInstructionCache(GetCurrentProcess(), site, 5) || !VirtualProtect(site, 5, protection, &previous)) {
        TerminateProcess(GetCurrentProcess(), 0xE5340001); __fastfail(7);
    }
    return 0;
}
#if defined(ESM_DEDICATED_NATIVE_BRIDGE)
DWORD WINAPI watchdog(void*) {
    DWORD timeout=30000;
#if defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
    timeout=2000;
#endif
    if(WaitForSingleObject(gate.completion,timeout)==WAIT_TIMEOUT&&InterlockedCompareExchange(&gate.active,2,1)==1)
        TerminateProcess(GetCurrentProcess(),0xE5340001);
    return 0;
}
#endif

[[noreturn]] void refuse() noexcept {
    OutputDebugStringW(L"ESM Shroudweave Server: dedicated startup refused.\n");
    TerminateProcess(GetCurrentProcess(), 0xE5340001);
    __fastfail(7);
}

bool range(DWORD rva, SIZE_T size, DWORD image_size) noexcept {
    return rva < image_size && size <= image_size - rva;
}

bool readable(const void* address, SIZE_T size) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(address, &region, sizeof region) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
        (region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
    const SIZE_T offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(region.BaseAddress);
    return offset < region.RegionSize && size <= region.RegionSize - offset;
}

bool inspect(HMODULE executable, unsigned char*& entry) noexcept {
    const auto* image = reinterpret_cast<const unsigned char*>(executable);
    MEMORY_BASIC_INFORMATION header{};
    if (!VirtualQuery(image, &header, sizeof header) || header.AllocationBase != executable ||
        header.State != MEM_COMMIT || header.RegionSize < sizeof(IMAGE_NT_HEADERS64) ||
        !readable(image, header.RegionSize)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<SIZE_T>(dos->e_lfanew) > header.RegionSize - sizeof(IMAGE_NT_HEADERS64)) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes != IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        !range(nt->OptionalHeader.AddressOfEntryPoint, 18, nt->OptionalHeader.SizeOfImage)) return false;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    const SIZE_T section_end = reinterpret_cast<const unsigned char*>(section) - image +
        static_cast<SIZE_T>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (section_end > header.RegionSize) return false;
    bool executable_section = false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD rva = nt->OptionalHeader.AddressOfEntryPoint;
        if (rva >= section[i].VirtualAddress && rva - section[i].VirtualAddress < section[i].Misc.VirtualSize &&
            (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) executable_section = true;
    }
    if (!executable_section) return false;
    const auto& tls = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls.VirtualAddress || tls.Size) {
        if (tls.Size < sizeof(IMAGE_TLS_DIRECTORY64) || !range(tls.VirtualAddress, tls.Size, nt->OptionalHeader.SizeOfImage) ||
            !readable(image + tls.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64))) return false;
        const auto* directory = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(image + tls.VirtualAddress);
        if (directory->AddressOfCallBacks) {
            const ULONGLONG base = reinterpret_cast<ULONGLONG>(image);
            if (directory->AddressOfCallBacks < base || directory->AddressOfCallBacks - base > MAXDWORD ||
                !range(static_cast<DWORD>(directory->AddressOfCallBacks - base), sizeof(ULONGLONG), nt->OptionalHeader.SizeOfImage) ||
                !readable(reinterpret_cast<const void*>(directory->AddressOfCallBacks), sizeof(ULONGLONG)) ||
                *reinterpret_cast<const ULONGLONG*>(directory->AddressOfCallBacks) != 0) return false;
        }
    }
    entry = const_cast<unsigned char*>(image) + nt->OptionalHeader.AddressOfEntryPoint;
    if (!readable(entry, 18)) return false;
#ifdef ESM_DEDICATED_GATE_FIXTURES
    const auto* fixture_entry = reinterpret_cast<const ULONG_PTR*>(GetProcAddress(executable, "EsmFixtureEntryAddress"));
    if (!fixture_entry || *fixture_entry != reinterpret_cast<ULONG_PTR>(entry) || *entry != 0x9c) return false;
#else
    // This host bootstrap pins its entry profile; feature hook profiles belong to modules.
    constexpr unsigned char expected[18] = {0x48,0x83,0xec,0x28,0xe8,0xe3,0x06,0x00,0x00,0x48,0x83,0xc4,0x28,0xe9,0xe2,0xfd,0xff,0xff};
    if (nt->FileHeader.TimeDateStamp != 0x6AD77F14 || nt->OptionalHeader.SizeOfImage != 0x1DD1000 ||
        nt->OptionalHeader.AddressOfEntryPoint != 0x00B47358) return false;
    for (unsigned i = 0; i < sizeof expected; ++i) if (entry[i] != expected[i]) return false;
#endif
    MEMORY_BASIC_INFORMATION code{};
    return VirtualQuery(entry, &code, sizeof code) && code.AllocationBase == executable &&
        code.State == MEM_COMMIT && code.Protect == PAGE_EXECUTE_READ;
}

LONG CALLBACK exception_redirect(PEXCEPTION_POINTERS exception) noexcept {
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        exception->ExceptionRecord->ExceptionAddress != gate.entry) return EXCEPTION_CONTINUE_SEARCH;
    auto* context = exception->ContextRecord;
    if (GetCurrentThreadId() != gate.thread || gate.phase != Phase::armed ||
        context->Rip != reinterpret_cast<DWORD64>(gate.entry) ||
        (context->ContextFlags & CONTEXT_FULL) != CONTEXT_FULL) {
        context->Rip = reinterpret_cast<DWORD64>(&EsmDedicatedRefusalThunk);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#ifdef ESM_DEDICATED_GATE_FIXTURES
    // A nonzero incoming AVX state makes dropping the extended area observable.
    if (gate.seed_xstate) {
        DWORD length{};
        DWORD64 features{};
        auto* avx = static_cast<unsigned char*>(LocateXStateFeature(context, 2, &length));
        if (!avx || length != 256 || !GetXStateFeaturesMask(context, &features) ||
            !SetXStateFeaturesMask(context, features | 4)) {
            context->Rip = reinterpret_cast<DWORD64>(&EsmDedicatedRefusalThunk);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        __stosb(avx + 240, 0x5a, 16);
    }
#endif
    // CopyContext preserves the extended XSAVE area in the preallocated context.
    if (!CopyContext(gate.context, context->ContextFlags, context)) {
        context->Rip = reinterpret_cast<DWORD64>(&EsmDedicatedRefusalThunk);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    gate.context->Rip = reinterpret_cast<DWORD64>(gate.entry);
    gate.phase = Phase::redirected;
    context->Rip = reinterpret_cast<DWORD64>(&EsmDedicatedEntryThunk);
    return EXCEPTION_CONTINUE_EXECUTION;
}

#if defined(ESM_DEDICATED_GATE_FIXTURES) && !defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
bool fixture_initialize(const CONTEXT* context, DWORD thread) {
    using Initialize = BOOL (*)(const CONTEXT*, DWORD);
    const auto initialize = reinterpret_cast<Initialize>(GetProcAddress(GetModuleHandleW(nullptr), "EsmFixtureInitialize"));
    return initialize && initialize(context, thread);
}
#endif
}

bool esm::host::DedicatedEntryGate::arm(HMODULE executable, bool process_start_attach) noexcept {
    if (!process_start_attach || !executable || gate.phase != Phase::empty || IsDebuggerPresent()) return false;
    unsigned char* entry{};
    if (!inspect(executable, entry)) return false;
    DWORD context_bytes = sizeof context_storage;
    if (!InitializeContext(context_storage, CONTEXT_ALL | CONTEXT_XSTATE, &gate.context, &context_bytes) ||
        !SetXStateFeaturesMask(gate.context, GetEnabledXStateFeatures())) return false;
#ifdef ESM_DEDICATED_GATE_FIXTURES
    wchar_t mode[64]{};
    GetEnvironmentVariableW(L"ESM_ENTRY_FIXTURE", mode, 64);
    gate.seed_xstate = lstrcmpW(mode, L"xstate") == 0;
#endif
    gate.thread = GetCurrentThreadId();
    gate.entry = entry;
    gate.original = *entry;
    gate.handler = AddVectoredExceptionHandler(1, exception_redirect);
    if (!gate.handler) return false;
    if (!VirtualProtect(entry, 1, PAGE_EXECUTE_READWRITE, &gate.protection)) return false;
    *static_cast<volatile unsigned char*>(entry) = 0xcc;
    DWORD previous{};
    if (!FlushInstructionCache(GetCurrentProcess(), entry, 1) ||
        !VirtualProtect(entry, 1, gate.protection, &previous)) refuse();
    gate.phase = Phase::armed;
    return true;
}

extern "C" [[noreturn]] void EsmDedicatedStartup() {
    if (gate.phase != Phase::redirected || GetCurrentThreadId() != gate.thread || IsDebuggerPresent()) refuse();
    bool ready = false;
#if defined(ESM_DEDICATED_GATE_FIXTURES) && !defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
    wchar_t mode[64]{};
    GetEnvironmentVariableW(L"ESM_ENTRY_FIXTURE", mode, 64);
    if (lstrcmpW(mode, L"missing") != 0) {
        ready = fixture_initialize(gate.context, gate.thread);
    }
#endif
#if defined(ESM_DEDICATED_NATIVE_BRIDGE)
    HMODULE proxy{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&EsmDedicatedStartup),&proxy))refuse();
    gate.witness={sizeof(gate.witness),1,proxy,{0,0},1};
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(gate.witness.witness),sizeof gate.witness.witness,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)refuse();
    gate.completion=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!gate.completion)refuse();
    InterlockedExchange(&gate.active,1);
    gate.deadline=GetTickCount64()+30000;
#if defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
    gate.deadline=GetTickCount64()+2000;
#endif
    HANDLE guard=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(!guard)refuse();CloseHandle(guard);
    ready=esm_dedicated_run_host(&gate.witness)&&gate.claimed&&GetTickCount64()<gate.deadline;
    EsmDedicatedNativeViewV1 verified{};
    if(ready&&native_validate(&gate.witness,&verified)!=0)ready=false;
#if defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
    wchar_t release_probe[64]{};GetEnvironmentVariableW(L"ESM_DEDICATED_CASE",release_probe,64);
    if(lstrcmpW(release_probe,L"timeout-release-race")==0)InterlockedExchange(&gate.active,2);
#endif
#endif
    // No fixture or environment input can authorize a production server.
    if (!ready || gate.phase != Phase::redirected || *gate.entry != 0xcc) refuse();
    DWORD previous{};
    if (!VirtualProtect(gate.entry, 1, PAGE_EXECUTE_READWRITE, &previous) || previous != gate.protection) refuse();
    *static_cast<volatile unsigned char*>(gate.entry) = gate.original;
    if (!FlushInstructionCache(GetCurrentProcess(), gate.entry, 1) ||
        !VirtualProtect(gate.entry, 1, gate.protection, &previous) || !RemoveVectoredExceptionHandler(gate.handler)) refuse();
#if defined(ESM_DEDICATED_NATIVE_BRIDGE)
    // Exactly one side owns the timeout/release handoff. A timeout winner can
    // never release entry, and a release winner cannot be killed by a stale read.
    if(GetTickCount64()>=gate.deadline||InterlockedCompareExchange(&gate.active,0,1)!=1)refuse();
#endif
    gate.phase = Phase::released;
    if(gate.completion)SetEvent(gate.completion);
#if defined(ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
    DWORD written{};WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"READY\n",6,&written,nullptr);
#endif
    RtlRestoreContext(gate.context, nullptr);
    refuse();
}

void esm_dedicated_set_host(void* host) { gate.host=host; }
extern "C" uint32_t __cdecl EsmDedicatedGate_GetApiV1(uint32_t major,uint32_t size,EsmDedicatedNativeApiV1* out) {
    if(!out||size!=sizeof(*out))return 2;
    __stosb(reinterpret_cast<unsigned char*>(out),0,size);
    if(major!=1)return 3;
    *out={sizeof(*out),1,native_claim,native_validate,native_commit};return 0;
}

extern "C" [[noreturn]] void EsmDedicatedRefused() noexcept {
    refuse();
}
