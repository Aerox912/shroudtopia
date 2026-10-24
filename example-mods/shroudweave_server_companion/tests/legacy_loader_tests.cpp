#include "legacy_loader.hpp"
#include "dbghelp_forward.hpp"
#include <string>
#include <type_traits>
#include <cstdio>
namespace {
template <typename Fn>
bool invoke_loader(Fn loader, const std::filesystem::path& root, HMODULE outer,
    bool require_synchronous, bool& supports_requirement) {
    if constexpr (std::is_invocable_r_v<bool, Fn, const std::filesystem::path&, HMODULE, bool>) {
        supports_requirement = true;
        return loader(root, outer, require_synchronous);
    } else {
        supports_requirement = false;
        return loader(root, outer);
    }
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 2;
    const HMODULE outer = LoadLibraryW(argv[1]);
    if (!outer) return 3;
    const std::wstring mode(argv[3]);
    bool supports_requirement{};
    const bool ready = invoke_loader(&shroudweave::server::initialize_legacy_loader,
        std::filesystem::path(argv[2]), outer, mode == L"deferred-final", supports_requirement);
    if (mode == L"failure") return ready ? 4 : 0;
    if (mode == L"deferred-final") {
        Sleep(500);
        const bool forward_loaded = GetModuleHandleW(L"forward.dll") != nullptr;
        const bool a_loaded = GetModuleHandleW(L"a.dll") != nullptr;
        const bool b_loaded = GetModuleHandleW(L"b.dll") != nullptr;
        if (!supports_requirement)
            std::fprintf(stderr, "RED: initialize_legacy_loader lacks require_synchronous argument\n");
        if (ready || forward_loaded || a_loaded || b_loaded) {
            std::fprintf(stderr, "OBS: ready=%u forward=%u a=%u b=%u supports=%u\n",
                ready ? 1u : 0u, forward_loaded ? 1u : 0u, a_loaded ? 1u : 0u,
                b_loaded ? 1u : 0u, supports_requirement ? 1u : 0u);
            return 9;
        }
        return supports_requirement ? 0 : 10;
    }
    if (!ready) return 5;
    if (mode == L"deferred") {
        if (GetModuleHandleW(L"a.dll") || GetModuleHandleW(L"b.dll")) return 6;
        if (!GetModuleHandleW(L"forward.dll")) return 11;
        Sleep(500);
    }
    if (!GetModuleHandleW(L"a.dll") || !GetModuleHandleW(L"b.dll")) return 7;
    if (!shroudweave::server::forward_minidump(nullptr, 0, nullptr,
        static_cast<MINIDUMP_TYPE>(0), nullptr, nullptr, nullptr) || GetLastError() != 0x1234) return 8;
    return 0;
}
