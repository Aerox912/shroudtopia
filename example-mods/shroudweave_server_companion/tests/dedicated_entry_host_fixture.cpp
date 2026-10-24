#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include "dedicated_native_v1.h"

#pragma function(memset)
extern "C" void* memset(void* target, int value, size_t count) {
    __stosb(static_cast<unsigned char*>(target), static_cast<unsigned char>(value), count);
    return target;
}

extern void esm_dedicated_set_host(void* host);
extern "C" uint32_t __cdecl EsmDedicatedGate_GetApiV1(
    uint32_t major, uint32_t size, EsmDedicatedNativeApiV1* out);

namespace {
using GetSites = void* const* (__cdecl*)();
using GetFunction = void* (__cdecl*)();
using OwnerReplacement = void (__cdecl*)();
using OwnerCount = LONG (__cdecl*)();
using OwnerReset = void (__cdecl*)();

void marker(const char* text, DWORD size) {
    DWORD written{};
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, size, &written, nullptr) || written != size)
        ExitProcess(90);
}

bool fixture_case(wchar_t* output, DWORD size) {
    output[0] = L'\0';
    return GetEnvironmentVariableW(L"ESM_ENTRY_FIXTURE", output, size) != 0;
}

bool call_bytes(const void* site, const void* destination, unsigned char output[5]) {
    if (!site || !destination) return false;
    const uintptr_t next = reinterpret_cast<uintptr_t>(site) + 5;
    const uintptr_t target = reinterpret_cast<uintptr_t>(destination);
    int64_t distance{};
    if (target >= next) {
        const uint64_t positive = target - next;
        if (positive > 0x7fffffffULL) return false;
        distance = static_cast<int64_t>(positive);
    } else {
        const uint64_t negative = next - target;
        if (negative > 0x80000000ULL) return false;
        distance = -static_cast<int64_t>(negative);
    }
    const int32_t displacement = static_cast<int32_t>(distance);
    output[0] = 0xe8;
    __movsb(output + 1, reinterpret_cast<const unsigned char*>(&displacement), 4);
    return true;
}

void copy_call(const void* site, unsigned char output[5]) {
    __movsb(output, static_cast<const unsigned char*>(site), 5);
}

bool executable_rx(void* address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(address, &info, sizeof info) && info.State == MEM_COMMIT &&
        info.Protect == PAGE_EXECUTE_READ && info.Type == MEM_PRIVATE && size <= info.RegionSize;
}

void* private_page_near(const void* site) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t page_bytes = granularity ? granularity : 0x10000;
    const uintptr_t site_address = reinterpret_cast<uintptr_t>(site);
    const uintptr_t base = site_address & ~(page_bytes - 1);
    void* allocation = VirtualAlloc(nullptr, page_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned char probe[5]{};
    if (allocation && call_bytes(site, allocation, probe)) return allocation;
    if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);

    constexpr uintptr_t maximum_distance = 0x70000000;
    constexpr uintptr_t step = 0x01000000;
    for (uintptr_t distance = page_bytes; distance < maximum_distance; distance += step) {
        const uintptr_t candidates[2] = {
            base <= UINTPTR_MAX - distance ? base + distance : 0,
            base >= distance ? base - distance : 0};
        for (const uintptr_t candidate : candidates) {
            if (!candidate) continue;
            allocation = VirtualAlloc(reinterpret_cast<void*>(candidate), page_bytes,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (allocation && call_bytes(site, allocation, probe)) return allocation;
            if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);
        }
    }
    return nullptr;
}

bool write_relay(void* page, const void* callback) {
    if (!page || !callback) return false;
    const unsigned char opcodes[6] = {0xff, 0x25, 0, 0, 0, 0};
    __movsb(static_cast<unsigned char*>(page), opcodes, sizeof opcodes);
    __movsb(static_cast<unsigned char*>(page) + 6,
        reinterpret_cast<const unsigned char*>(&callback), sizeof callback);
    return true;
}

void* load_symbol(HMODULE module, const char* name) {
    return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}

bool get_fixture_sites(HMODULE executable, void* const*& sites, void*& original, void*& alternate, void*& entry) {
    const auto get_sites = reinterpret_cast<GetSites>(load_symbol(executable, "EsmFixtureGetCallSites"));
    const auto get_original = reinterpret_cast<GetFunction>(load_symbol(executable, "EsmFixtureGetOriginalCallAddress"));
    sites = get_sites ? get_sites() : nullptr;
    original = get_original ? get_original() : nullptr;
    alternate = load_symbol(executable, "FixtureAlternate");
    auto** entry_pointer = reinterpret_cast<void**>(load_symbol(executable, "EsmFixtureEntryAddress"));
    entry = entry_pointer ? *entry_pointer : nullptr;
    return sites && original && alternate && entry;
}

bool rejected(const EsmDedicatedNativeApiV1& api, const EsmDedicatedNativeStartV1* start,
    void* owner, void* site, const unsigned char replacement[5], const unsigned char expected[5]) {
    return api.commit_call(start, owner, site, replacement, expected, 5) != 0;
}

bool tamper(void* address, unsigned char* bytes, SIZE_T size) {
    DWORD previous{};
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    __movsb(static_cast<unsigned char*>(address), bytes, size);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, size) != FALSE;
    DWORD ignored{};
    const bool restored = VirtualProtect(address, size, previous, &ignored) != FALSE;
    return flushed && restored;
}
}

bool esm_dedicated_run_host(const EsmDedicatedNativeStartV1* start) {
    if (!start || !start->proxy) return false;
    esm_dedicated_set_host(start->proxy);
    EsmDedicatedNativeApiV1 api{};
    if (EsmDedicatedGate_GetApiV1(1, sizeof api, &api) != 0 ||
        api.struct_size != sizeof api || api.abi_major != 1 || !api.claim || !api.validate || !api.commit_call)
        return false;
    EsmDedicatedNativeViewV1 view{};
    if (api.claim(start, start->proxy, &view) != 0 || view.struct_size != sizeof view ||
        view.abi_major != 1 || view.executable != GetModuleHandleW(nullptr) || view.call_site ||
        view.thread != GetCurrentThreadId() || view.committed != 0 || view.generation != start->generation)
        return false;

    wchar_t selected[96]{};
    if (!fixture_case(selected, static_cast<DWORD>(sizeof selected / sizeof selected[0]))) return false;
    if (lstrcmpW(selected, L"native-no-commit") == 0) {
        return api.validate(start, &view) == 0 && view.committed == 0 && !view.call_site;
    }

    HMODULE owner = LoadLibraryW(L"shroudweave_startup_owner.dll");
    if (!owner) return false;
    const auto callback = reinterpret_cast<OwnerReplacement>(load_symbol(owner, "EsmStartupOwnerReplacement"));
    const auto count = reinterpret_cast<OwnerCount>(load_symbol(owner, "EsmStartupOwnerCount"));
    const auto reset = reinterpret_cast<OwnerReset>(load_symbol(owner, "EsmStartupOwnerReset"));
    if (!callback || !count || !reset) return false;
    reset();

    void* const* sites{};
    void* original{};
    void* alternate{};
    void* entry{};
    const HMODULE executable = GetModuleHandleW(nullptr);
    if (!get_fixture_sites(executable, sites, original, alternate, entry)) return false;

    if (lstrcmpW(selected, L"native-foreign-owner") == 0) {
        unsigned char expected[5]{}, replacement[5]{};
        copy_call(sites[0], expected);
        if (!call_bytes(sites[0], reinterpret_cast<void*>(callback), replacement)) return false;
        const bool refused = rejected(api, start, executable, sites[0], replacement, expected);
        if (refused) marker("NATIVE_REJECTED\n", 16);
        return false;
    }

    if (lstrcmpW(selected, L"native-private-site") == 0) {
        void* private_site = private_page_near(sites[0]);
        if (!private_site) return false;
        unsigned char expected[5]{}, replacement[5]{};
        if (!call_bytes(private_site, original, expected)) return false;
        __movsb(static_cast<unsigned char*>(private_site), expected, sizeof expected);
        DWORD previous{};
        if (!VirtualProtect(private_site, 1, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), private_site, sizeof expected)) return false;
        if (!call_bytes(private_site, reinterpret_cast<void*>(callback), replacement)) return false;
        const bool refused = rejected(api, start, owner, private_site, replacement, expected);
        if (refused) marker("NATIVE_REJECTED\n", 16);
        return false;
    }

    if (lstrcmpW(selected, L"native-invalid-target") == 0) {
        void* private_target = private_page_near(sites[0]);
        if (!private_target) return false;
        auto* target = static_cast<unsigned char*>(private_target) + 128;
        *target = 0xc3;
        DWORD previous{};
        if (!VirtualProtect(private_target, 1, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), private_target, 256)) return false;
        unsigned char expected[5]{}, replacement[5]{};
        copy_call(sites[0], expected);
        if (!call_bytes(sites[0], target, replacement)) return false;
        const bool refused = rejected(api, start, owner, sites[0], replacement, expected);
        if (refused) marker("NATIVE_REJECTED\n", 16);
        return false;
    }

    if (lstrcmpW(selected, L"native-bad-relay") == 0) {
        void* relay = private_page_near(sites[0]);
        if (!relay || !write_relay(relay, alternate)) return false;
        DWORD previous{};
        if (!VirtualProtect(relay, 1, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), relay, 14)) return false;
        unsigned char expected[5]{}, replacement[5]{};
        copy_call(sites[0], expected);
        if (!call_bytes(sites[0], relay, replacement)) return false;
        const bool refused = rejected(api, start, owner, sites[0], replacement, expected);
        if (refused) marker("NATIVE_REJECTED\n", 16);
        return false;
    }

    if (lstrcmpW(selected, L"native-site-tamper") == 0) {
        unsigned char expected[5]{}, replacement[5]{};
        copy_call(sites[0], expected);
        if (!call_bytes(sites[0], reinterpret_cast<void*>(callback), replacement) ||
            api.commit_call(start, owner, sites[0], replacement, expected, 5) != 0) return false;
        unsigned char changed[5]{};
        copy_call(sites[0], changed);
        changed[1] ^= 0x40;
        if (!tamper(sites[0], changed, sizeof changed)) return false;
        marker("NATIVE_SITE_TAMPER\n", 19);
        return true;
    }

    if (lstrcmpW(selected, L"native-relay-tamper") == 0) {
        void* relay = private_page_near(sites[0]);
        if (!relay || !write_relay(relay, reinterpret_cast<void*>(callback))) return false;
        DWORD previous{};
        if (!VirtualProtect(relay, 1, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), relay, 14)) return false;
        unsigned char expected[5]{}, replacement[5]{};
        copy_call(sites[0], expected);
        if (!call_bytes(sites[0], relay, replacement) ||
            api.commit_call(start, owner, sites[0], replacement, expected, 5) != 0) return false;
        void* altered = alternate;
        if (!tamper(static_cast<unsigned char*>(relay) + 6,
                reinterpret_cast<unsigned char*>(&altered), sizeof altered)) return false;
        marker("NATIVE_RELAY_TAMPER\n", 20);
        return true;
    }

    if (lstrcmpW(selected, L"native-overlap") == 0) {
        unsigned char expected[5]{}, replacement[5]{};
        copy_call(sites[0], expected);
        if (!call_bytes(sites[0], reinterpret_cast<void*>(callback), replacement) ||
            api.commit_call(start, owner, sites[0], replacement, expected, 5) != 0) return false;
        const uint32_t overlap_result = api.commit_call(start, owner, sites[0], replacement,
            replacement, 5);
        if (overlap_result == 6) marker("NATIVE_REJECTED\n", 16);
        return false;
    }

    if (lstrcmpW(selected, L"native-capacity") == 0) {
        unsigned committed{};
        for (; committed != 65; ++committed) {
            unsigned char expected[5]{}, replacement[5]{};
            copy_call(sites[committed], expected);
            if (!call_bytes(sites[committed], reinterpret_cast<void*>(callback), replacement)) return false;
            const uint32_t result = api.commit_call(start, owner, sites[committed], replacement, expected, 5);
            if (committed < 64 && result != 0) return false;
            if (committed == 64 && result == 0) return false;
        }
        marker("NATIVE_CAPACITY_64\n", 19);
        return false;
    }

    void* site = sites[0];
    if (lstrcmpW(selected, L"native-bootstrap-site") == 0) site = entry;
    unsigned char expected[5]{}, replacement[5]{};
    copy_call(site, expected);
    if (lstrcmpW(selected, L"native-mismatch") == 0) expected[2] ^= 0x40;

    void* destination = reinterpret_cast<void*>(callback);
    void* relay{};
    if (lstrcmpW(selected, L"native-relay") == 0) {
        relay = private_page_near(site);
        if (!relay || !write_relay(relay, destination)) return false;
        DWORD previous{};
        if (!VirtualProtect(relay, 1, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), relay, 14)) return false;
        if (!executable_rx(relay, 14)) return false;
        destination = relay;
    }
    if (!call_bytes(site, destination, replacement)) return false;
    const bool refused = rejected(api, start, owner, site, replacement, expected);
    if (lstrcmpW(selected, L"native-call") == 0 || lstrcmpW(selected, L"native-relay") == 0) {
        if (refused || api.validate(start, &view) != 0 || view.committed != 1 || view.call_site) return false;
        FreeLibrary(owner); // A successful commitment pins owner code for process lifetime.
        return true;
    }
    if (lstrcmpW(selected, L"native-bootstrap-site") == 0 ||
        lstrcmpW(selected, L"native-mismatch") == 0) {
        if (refused) marker("NATIVE_REJECTED\n", 16);
        return false;
    }
    return false;
}
