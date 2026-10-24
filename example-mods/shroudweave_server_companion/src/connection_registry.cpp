#include "connection_registry.hpp"
#include <limits>
namespace shroudweave::server {
namespace { bool valid(const NativePeer& p) {
    return p.context && p.handle && (p.handle & 63) < 16 && p.steam && p.hosting && p.authenticated_transport;
} }
ConnectionRegistry::Pending ConnectionRegistry::begin(const NativePeer& p) {
    if (exhausted_ || !valid(p) || p.state != 1) return {};
    if (context_ != p.context) {
        // The first observed native context establishes the registry binding;
        // it does not replace an already-bound world epoch. Later context
        // changes retire the previous peer generation before adopting the new
        // context. reset() already advances the epoch and clears every slot.
        if (context_) reset(context_);
        context_ = p.context;
    }
    auto& slot = slots_[p.handle & 63];
    slot.peer = {}; slot.proof = 0;
    if (++slot.epoch == 0) { exhausted_ = true; return {}; }
    return {p, epoch_, slot.epoch};
}
bool ConnectionRegistry::finish(const Pending& pending, const NativePeer& after, bool success) {
    if (!success) { remove(after.context, after.handle); return false; }
    if (exhausted_ || !pending.epoch || pending.epoch != epoch_ || !valid(after) ||
        context_ != after.context || !pending.peer.same(after) || after.state != 2) return false;
    auto& slot = slots_[after.handle & 63];
    if (slot.epoch != pending.slot_epoch || serial_ == std::numeric_limits<std::uint64_t>::max()) return false;
    slot.peer = after; slot.proof = ++serial_; return true;
}
void ConnectionRegistry::remove(std::uintptr_t context, std::uint16_t handle) {
    if (context != context_ || !handle || (handle & 63) >= 16) return;
    auto& slot = slots_[handle & 63];
    // Never revoke a replacement connection on a delayed old-handle callback.
    if (slot.proof && slot.peer.handle != handle) return;
    slot.peer = {}; slot.proof = 0;
    if (++slot.epoch == 0) exhausted_ = true;
}
void ConnectionRegistry::reset(std::uintptr_t context) {
    if (context != context_) return;
    slots_ = {}; context_ = 0;
    if (++epoch_ == 0) exhausted_ = true;
}
std::optional<PeerAssociation> ConnectionRegistry::current(std::uint64_t sender) {
    if (!sender || exhausted_) return {};
    std::optional<PeerAssociation> result;
    for (auto& slot : slots_) {
        if (!slot.proof || slot.peer.steam != sender) continue;
        const auto now = reader_(context_, slot.peer.handle);
        if (!now || !valid(*now) || now->state != 2 || !slot.peer.same(*now)) {
            slot.proof = 0; continue;
        }
        if (result) {
            // Ambiguity retires every proof for this sender. Removing one of
            // the competing peers cannot revive the other proof.
            for (auto& duplicate : slots_) {
                if (duplicate.proof && duplicate.peer.steam == sender) duplicate.proof = 0;
            }
            return {};
        }

        result = PeerAssociation{sender, slot.proof, epoch_, 1, false, "", 0};
    }
    return result;
}
}
