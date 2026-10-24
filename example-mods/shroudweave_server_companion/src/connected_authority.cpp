#include "connected_authority.hpp"
namespace shroudweave::server {
bool ConnectedAuthority::permitted(const PeerAssociation& p, const EditPolicy& policy) {
    if (!p.steam || !p.connection || !p.world || !p.configuration ||
        !policy.revision || p.configuration != policy.configuration) return false;
    if (policy.mode == EditMode::open_connected) return true;
    if (!policy.groups_verified || !p.group_verified || p.group.empty()) return false;
    if (policy.mode == EditMode::exact_groups)
        return !policy.selected_group.empty() && p.group == policy.selected_group;
    if (policy.mode == EditMode::equivalent_permissions)
        return (p.permissions & policy.selected_group_permissions) == policy.selected_group_permissions;
    return false;
}
std::optional<ConnectedAuthority::Ticket> ConnectedAuthority::receive(
    std::uint64_t sender, const EditPolicy& policy) {
    if (!sender) return std::nullopt;
    const auto p = provider_.current(sender);
    if (!p || p->steam != sender || !permitted(*p, policy)) return std::nullopt;
    return Ticket(*p, policy, this);
}
bool ConnectedAuthority::revalidate(const Ticket& ticket, const PeerAssociation& current,
                                    const EditPolicy& policy) const {
    if (ticket.owner_ != this || ticket.policy_ != policy) return false;
    const auto& peer = ticket.peer_;
    return current.steam == peer.steam && current.connection == peer.connection &&
        current.world == peer.world && current.configuration == peer.configuration &&
        current.group_verified == peer.group_verified && current.group == peer.group &&
        current.permissions == peer.permissions && permitted(current, policy);
}
bool ConnectedAuthority::revalidate(const Ticket& ticket, const EditPolicy& policy) {
    if (ticket.owner_ != this || ticket.policy_ != policy) return false;
    try {
        const auto peer = provider_.current(ticket.peer_.steam);
        return peer && revalidate(ticket, *peer, policy);
    } catch (...) {
        return false;
    }
}
bool ConnectedAuthority::with_current(const Ticket& ticket, const EditPolicy& policy,
                                      const std::function<void()>& commit) const {
    if (ticket.owner_ != this || ticket.policy_ != policy || !commit) return false;
    bool callback_seen = false;
    bool invoked = false;
    try {
        const bool provider_completed = provider_.with_current(ticket.peer_.steam,
            [&](const PeerAssociation& current) {
                if (callback_seen) return;
                callback_seen = true;
                if (!revalidate(ticket, current, policy)) return;
                invoked = true;
                commit();
            });
        return provider_completed && invoked;
    } catch (...) {
        return false;
    }
}
bool ConnectedAuthority::can_read(std::uint64_t sender, std::uint64_t world,
                                  std::uint64_t configuration) {
    if (!sender || !world || !configuration) return false;
    const auto p = provider_.current(sender);
    return p && p->steam == sender && p->connection && p->world == world &&
        p->configuration == configuration;
}
}
