#pragma once
#include "connected_authority.hpp"
#include <array>
#include <functional>

namespace shroudweave::server {
struct NativePeer {
    std::uintptr_t context{};
    std::uint16_t handle{};
    std::uint64_t steam{};
    std::uint8_t state{};
    bool hosting{}, authenticated_transport{};
    bool same(const NativePeer& p) const {
        return context && context == p.context && handle && handle == p.handle && steam && steam == p.steam;
    }
};
// Serialized by the shared runtime, including native removal and reset hooks.
// A state=2 memory sample alone cannot mint a proof: the native successful
// authentication callback must be observed across the pending->accepted transition.
class ConnectionRegistry final : public ConnectedPeerProvider {
public:
    using Reader = std::function<std::optional<NativePeer>(std::uintptr_t, std::uint16_t)>;
    struct Pending { NativePeer peer; std::uint64_t epoch{}, slot_epoch{}; };
    explicit ConnectionRegistry(Reader reader) : reader_(std::move(reader)) {}
    Pending begin(const NativePeer&);
    bool finish(const Pending&, const NativePeer&, bool success);
    void remove(std::uintptr_t, std::uint16_t);
    void reset(std::uintptr_t);
    std::optional<PeerAssociation> current(std::uint64_t sender) override;
    std::uint64_t epoch() const { return epoch_; }
private:
    struct Slot { NativePeer peer; std::uint64_t epoch = 1, proof{}; };
    std::array<Slot,16> slots_{};
    Reader reader_;
    std::uintptr_t context_{};
    std::uint64_t epoch_ = 1, serial_{};
    bool exhausted_{};
};
}
