#include "connected_authority.hpp"
#include <cstdlib>
#include <cstdio>
using namespace shroudweave::server;
struct Provider : ConnectedPeerProvider {
    std::optional<PeerAssociation> peer;
    std::optional<PeerAssociation> current(std::uint64_t) override { return peer; }
};
void check(bool result, const char* label) { if (!result) { std::fprintf(stderr, "%s\n", label); std::exit(1); } }
int main() {
    Provider provider;
    ConnectedAuthority authority(provider);
    EditPolicy policy{};
    policy.mode = EditMode::open_connected;
    policy.revision = 1;
    policy.configuration = 10;
    check(!authority.receive(42, policy), "missing provider denies");
    provider.peer = PeerAssociation{42, 5, 6, 10, false, "", 0};
    check(!authority.receive(99, policy), "payload or mismatched sender denies");
    check(authority.can_read(42, 6, 10), "current connected peer can read matching world");
    check(!authority.can_read(42, 7, 10), "read from another world is denied");
    check(!authority.can_read(42, 6, 11), "read from another configuration is denied");
    check(!authority.can_read(99, 6, 10), "read by a different transport sender is denied");
    auto ticket = authority.receive(42, policy);
    check(ticket && authority.revalidate(*ticket, policy), "explicit open current connection accepted");
    ConnectedAuthority other(provider);
    check(!other.revalidate(*ticket, policy), "ticket cannot cross authority instance");
    auto changed = policy; changed.selected_group = "build_operators";
    check(!authority.revalidate(*ticket, changed), "policy content change without revision bump revokes");
    provider.peer.reset();
    check(!authority.revalidate(*ticket, policy), "disconnect revokes queued work");
    provider.peer = PeerAssociation{42, 7, 6, 10, false, "", 0};
    check(!authority.revalidate(*ticket, policy), "same SteamID reconnect revokes old work");
    provider.peer->connection = 5; provider.peer->world = 9;
    check(!authority.revalidate(*ticket, policy), "world replacement revokes");
    provider.peer->world = 6; ++policy.revision;
    check(!authority.revalidate(*ticket, policy), "policy replacement revokes");

    policy.mode = EditMode::exact_groups;
    policy.groups_verified = false;
    policy.selected_group = "build_operators";
    check(!authority.receive(42, policy), "exact-group policy requires verified group configuration");
    policy.groups_verified = true;
    provider.peer->group_verified = true;
    provider.peer->group = "auditors";
    check(!authority.receive(42, policy), "different verified group is refused");
    provider.peer->group = "build_operators";
    auto selected = authority.receive(42, policy);
    check(selected.has_value(), "module-selected arbitrary group is accepted");

    provider.peer->group = "builders";
    provider.peer->permissions = 7;
    policy.selected_group_permissions = 7;
    check(!authority.receive(42, policy), "permission overlap cannot replace exact-group equality");
    check(!authority.revalidate(*selected, policy), "changing exact selected group revokes prior work");

    policy.mode = EditMode::equivalent_permissions;
    policy.selected_group.clear();
    check(authority.receive(42, policy).has_value(), "explicit equivalent-permissions policy accepts verified mask");
    provider.peer->group_verified = false;
    check(!authority.receive(42, policy), "permission mask without a verified group is refused");

    policy.mode = EditMode::exact_groups;
    policy.groups_verified = true;
    policy.selected_group = "release_coordinators";
    provider.peer->group_verified = true;
    provider.peer->group = "release_coordinators";
    selected = authority.receive(42, policy);
    check(selected.has_value(), "second arbitrary selected group is accepted");
    provider.peer->group = "visitors";
    check(!authority.revalidate(*selected, policy), "verified group replacement revokes");
    provider.peer->group = "release_coordinators";
    policy.selected_group = "operators";
    check(!authority.revalidate(*selected, policy), "selected-group policy change revokes prior work");
    policy.selected_group = "release_coordinators";
    provider.peer->configuration = 11;
    check(!authority.receive(42, policy), "stale host configuration denies");
    std::puts("PASS: generic connected authority and revocation");
}