#include "connection_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>

using namespace shroudweave::server;

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

int main() {
    std::map<std::uint16_t, NativePeer> native;
    ConnectionRegistry registry([&](std::uintptr_t context, std::uint16_t handle)
        -> std::optional<NativePeer> {
        const auto it = native.find(handle);
        return it != native.end() && it->second.context == context ?
            std::optional(it->second) : std::nullopt;
    });

    const auto startup_epoch = registry.epoch();
    check(startup_epoch != 0, "registry starts with a usable host epoch");

    native[65] = {100, 65, 42, 2, true, true};
    check(!registry.current(42), "state-2 memory sample cannot create an authentication proof");
    native[65].state = 1;
    const auto first_pending = registry.begin(native[65]);
    const auto first_context_epoch = registry.epoch();
    check(first_pending.epoch == startup_epoch && first_context_epoch == startup_epoch,
        "first nonzero native context preserves the host startup epoch");
    native[65].state = 2;
    check(registry.finish(first_pending, native[65], true),
        "observed successful native transition mints a proof");
    const auto first = registry.current(42);
    check(first && first->steam == 42 && first->world == startup_epoch &&
        first->configuration == 1 && first->connection != 0,
        "first peer association matches startup world and host configuration");
    ConnectedAuthority startup_authority(registry);
    check(startup_authority.can_read(42, startup_epoch, 1),
        "host authority accepts the first peer under its startup epoch/configuration");

    native[66] = {100, 66, 43, 1, true, true};
    const auto second_pending = registry.begin(native[66]);
    native[66].state = 2;
    check(registry.finish(second_pending, native[66], true),
        "a second peer authenticates in the same native context");
    const auto second = registry.current(43);
    check(second && second->world == startup_epoch && second->configuration == 1,
        "same-context peer retains the startup epoch");

    // 65 and 129 share a registry slot, but the full native handle remains
    // part of the proof. A delayed removal for 65 must not revoke 129.
    native[129] = {100, 129, 44, 1, true, true};
    const auto replacement_pending = registry.begin(native[129]);
    native[129].state = 2;
    check(registry.finish(replacement_pending, native[129], true),
        "a replacement native handle can reuse the same slot");
    const auto replacement = registry.current(44);
    check(replacement && replacement->connection != first->connection,
        "slot reuse creates a distinct authentication proof");
    registry.remove(100, 65);
    check(registry.current(44).has_value(),
        "delayed removal for the old full handle cannot revoke its slot replacement");
    check(!registry.current(42), "slot reuse revokes the prior peer proof");

    native[130] = {100, 130, 45, 1, true, true};
    const auto failed_pending = registry.begin(native[130]);
    native[130].state = 2;
    check(!registry.finish(failed_pending, native[130], false) && !registry.current(45),
        "unsuccessful native transition never mints an authentication proof");

    native[129].state = 0;
    check(!registry.current(44), "native state loss revokes the current proof");
    native[129].state = 2;
    check(!registry.current(44), "restoring state-2 memory cannot revive a retired proof");

    native[129] = {200, 129, 44, 1, true, true};
    const auto changed_context_pending = registry.begin(native[129]);
    native[129].state = 2;
    check(registry.finish(changed_context_pending, native[129], true),
        "peer authenticates after native context replacement");
    const auto changed_epoch = registry.epoch();
    const auto changed = registry.current(44);
    check(changed_epoch == first_context_epoch + 1 && changed &&
        changed->world == changed_epoch && changed->world != startup_epoch &&
        changed->configuration == 1,
        "later context replacement advances the world epoch and invalidates startup-bound authority");
    check(!startup_authority.can_read(44, startup_epoch, 1),
        "a host runtime bound to the retired startup epoch rejects the new-context peer");
    check(!registry.current(43), "context replacement retires all prior-context peers");

    registry.reset(200);
    check(registry.epoch() == changed_epoch + 1 && !registry.current(44),
        "explicit host reset advances the epoch and revokes all peers");

    std::map<std::uint16_t, NativePeer> duplicate_native;
    ConnectionRegistry duplicate_registry([&](std::uintptr_t context, std::uint16_t handle)
        -> std::optional<NativePeer> {
        const auto it = duplicate_native.find(handle);
        return it != duplicate_native.end() && it->second.context == context ?
            std::optional(it->second) : std::nullopt;
    });
    duplicate_native[4] = {300, 4, 46, 1, true, true};
    const auto initial_pending = duplicate_registry.begin(duplicate_native[4]);
    duplicate_native[4].state = 2;
    check(duplicate_registry.finish(initial_pending, duplicate_native[4], true),
        "initial unique connection receives an authentication proof");
    const auto initial_proof = duplicate_registry.current(46);
    check(initial_proof.has_value(), "initial proof is readable while its sender is unique");
    duplicate_native[5] = {300, 5, 46, 1, true, true};
    const auto duplicate_pending = duplicate_registry.begin(duplicate_native[5]);
    duplicate_native[5].state = 2;
    check(duplicate_registry.finish(duplicate_pending, duplicate_native[5], true),
        "a second authenticated connection with the same Steam identity is observed");
    check(!duplicate_registry.current(46),
        "multiple current connections for one sender revoke authenticated authority as ambiguous");
    duplicate_registry.remove(300, 5);
    duplicate_native.erase(5);
    check(!duplicate_registry.current(46),
        "removing one ambiguous connection cannot revive the remaining old proof");
    duplicate_native[4].state = 1;
    const auto recovered_pending = duplicate_registry.begin(duplicate_native[4]);
    duplicate_native[4].state = 2;
    check(duplicate_registry.finish(recovered_pending, duplicate_native[4], true),
        "a fresh successful authentication transition can restore authority");
    const auto recovered = duplicate_registry.current(46);
    check(recovered && recovered->connection != initial_proof->connection,
        "only a new authentication proof restores current authority after ambiguity");
    std::puts("PASS: generic connection registry proof and epoch boundaries");
}
