#pragma once
#include <cstdint>
namespace esm::host::detours {
using CreateAndEnable = bool (*)(void*, void*, void*, void**) noexcept;
struct OwnerV1 final {
    std::uint32_t struct_size;
    std::uint16_t abi_major;
    std::uint16_t reserved;
    void* context;
    CreateAndEnable create_and_enable;
};
// One process-resident repaired MinHook owner. Acquisition grants no game/session
// authority; the actual native adapter still owns and validates its hook sites.
const OwnerV1& process_owner() noexcept;
}
