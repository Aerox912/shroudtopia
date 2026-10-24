#include "connections.hpp"
#include "connection_profile.hpp"
#include <MinHook.h>
#include <mutex>
#include <cstring>
namespace shroudweave::server {
namespace {
std::recursive_mutex guard;
template<class T> bool read(std::uintptr_t at, T& value) {
    SIZE_T copied{};
    return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}
std::optional<NativePeer> read_peer(std::uintptr_t context, std::uint16_t handle) {
    if (!context || context > UINTPTR_MAX - 0x21c4 || !handle || (handle & 63) >= 16) return {};
    const auto row = context + 0x68 + (handle & 63) * 0x1d8;
    NativePeer peer{context, handle};
    std::uint16_t actual{}, again{}; std::uint8_t auth{}, hosting{}, state{};
    if (!read(row, actual) || actual != handle || !read(row + 2, peer.steam) ||
        !read(row + 0x98, peer.state) || !read(context + 0x21c0, auth) ||
        !read(context + 0x21c2, hosting) || auth > 1 || hosting > 1 ||
        !read(row, again) || again != actual || !read(row + 0x98, state) || state != peer.state) return {};
    peer.authenticated_transport = auth != 0; peer.hosting = hosting != 0;
    return peer;
}
ConnectionRegistry registry(read_peer);
using Event = std::uintptr_t (*)(void*, void*);
using Remove = std::uintptr_t (*)(void*, std::uint16_t);
using Reset = std::uintptr_t (*)(void*);
Event original_event{}; Remove original_remove{}; Reset original_reset{};
NativePeer find_peer(std::uintptr_t context, std::uint64_t steam) {
    for (std::size_t i = 0; i < 16; ++i) {
        std::uint16_t handle{};
        if (!read(context + 0x68 + i * 0x1d8, handle)) return {};
        const auto peer = read_peer(context, handle);
        if (peer && peer->steam == steam) return *peer;
    }
    return {};
}
std::uintptr_t on_event(void* backend, void* event) {
    std::uint8_t kind{};
    const auto at = reinterpret_cast<std::uintptr_t>(event);
    if (!read(at, kind) || kind != 19) return original_event(backend, event);
    const auto context = reinterpret_cast<std::uintptr_t>(backend) + 0x348fc0;
    std::uint64_t steam{}; std::uint32_t response{};
    ConnectionRegistry::Pending pending;
    const bool captured = read(at + 8, steam) && read(at + 16, response);
    {
        std::lock_guard lock(guard);
        const auto before = find_peer(context, steam);
        if (captured && !response) pending = registry.begin(before);
        else registry.remove(context, before.handle);
    }
    const auto result = original_event(backend, event);
    {
        std::lock_guard lock(guard);
        registry.finish(pending, find_peer(context, steam), captured && response == 0);
    }
    return result;
}
std::uintptr_t on_remove(void* context, std::uint16_t handle) {
    { std::lock_guard lock(guard); registry.remove(reinterpret_cast<std::uintptr_t>(context), handle); }
    return original_remove(context, handle);
}
std::uintptr_t on_reset(void* context) {
    { std::lock_guard lock(guard); registry.reset(reinterpret_cast<std::uintptr_t>(context)); }
    return original_reset(context);
}
}
void with_connections(const std::function<void(ConnectionRegistry&)>& work) {
    std::lock_guard lock(guard); work(registry);
}
bool install_connections(HMODULE game) {
    static bool attempted = false;
    if (attempted) return false;
    attempted = true;
    auto* base = reinterpret_cast<unsigned char*>(game);
    if (!connection_profile_matches(base)) return false;
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    struct Hook { unsigned rva; void* callback; void** original; };
    const Hook hooks[] = {
        {0x9d60f0, reinterpret_cast<void*>(on_event), reinterpret_cast<void**>(&original_event)},
        {0x9c5060, reinterpret_cast<void*>(on_remove), reinterpret_cast<void**>(&original_remove)},
        {0x9c4e00, reinterpret_cast<void*>(on_reset), reinterpret_cast<void**>(&original_reset)}
    };
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(on_event), &pinned)) return false;
    std::size_t made = 0;
    for (const auto& hook : hooks) {
        if (MH_CreateHook(base + hook.rva, hook.callback, hook.original) != MH_OK) break;
        ++made;
    }
    if (made != std::size(hooks)) {
        for (std::size_t i = 0; i < made; ++i) MH_RemoveHook(base + hooks[i].rva);
        return false;
    }
    // Enable only our sites. Never apply a process-wide pending hook queue.
    for (const auto& hook : hooks) if (MH_EnableHook(base + hook.rva) != MH_OK) {
        for (const auto& undo : hooks) MH_DisableHook(base + undo.rva);
        return false;
    }
    return true;
}
}
