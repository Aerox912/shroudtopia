#include "server_services_runtime.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <thread>
#include <vector>

using namespace shroudweave::server;
using namespace std::string_literals;

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Peers final : ConnectedPeerProvider {
    std::map<std::uint64_t, PeerAssociation> rows;
    std::optional<PeerAssociation> current(std::uint64_t sender) override {
        const auto it = rows.find(sender);
        return it == rows.end() ? std::nullopt : std::optional(it->second);
    }
};
struct BoundaryPeers final : ConnectedPeerProvider {
    std::map<std::uint64_t, PeerAssociation> rows;
    bool revoke_at_boundary{};
    std::optional<PeerAssociation> current(std::uint64_t sender) override {
        const auto it = rows.find(sender);
        return it == rows.end() ? std::nullopt : std::optional(it->second);
    }
    bool with_current(std::uint64_t sender,
        const std::function<void(const PeerAssociation&)>& operation) override {
        if (revoke_at_boundary) {
            rows.erase(sender);
            revoke_at_boundary = false;
            return false;
        }
        const auto it = rows.find(sender);
        if (it == rows.end()) return false;
        operation(it->second);
        return true;
    }
};
struct CommitBarrierPeers final : ConnectedPeerProvider {
    std::map<std::uint64_t, PeerAssociation> rows;
    std::mutex lifecycle_mutex;
    std::mutex event_mutex;
    std::condition_variable event_condition;
    bool commit_entered{};
    bool release_commit{};
    std::atomic<bool> guard_active{};
    std::atomic<bool> revoke_attempted{};
    std::atomic<bool> revoke_completed{};

    std::optional<PeerAssociation> current(std::uint64_t sender) override {
        std::lock_guard lock(lifecycle_mutex);
        const auto it = rows.find(sender);
        return it == rows.end() ? std::nullopt : std::optional(it->second);
    }
    bool with_current(std::uint64_t sender,
        const std::function<void(const PeerAssociation&)>& operation) override {
        std::unique_lock lifecycle_lock(lifecycle_mutex);
        const auto it = rows.find(sender);
        if (it == rows.end()) return false;
        {
            std::unique_lock event_lock(event_mutex);
            commit_entered = true;
            event_condition.notify_all();
            if (!event_condition.wait_for(event_lock, std::chrono::seconds(5),
                    [&] { return release_commit; })) return false;
        }
        guard_active.store(true, std::memory_order_release);
        operation(it->second);
        guard_active.store(false, std::memory_order_release);
        return true;
    }
    void revoke(std::uint64_t sender) {
        revoke_attempted.store(true, std::memory_order_release);
        std::lock_guard lock(lifecycle_mutex);
        rows.erase(sender);
        revoke_completed.store(true, std::memory_order_release);
    }
    bool lifecycle_locked() {
        if (!lifecycle_mutex.try_lock()) return true;
        lifecycle_mutex.unlock();
        return false;
    }
};
struct CleanupRacePeers final : ConnectedPeerProvider {
    std::map<std::uint64_t, PeerAssociation> rows;
    std::mutex rows_mutex;
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    std::uint64_t paused_sender{};
    bool pause_next{};
    bool observation_entered{};
    bool release_observation{};

    std::optional<PeerAssociation> current(std::uint64_t sender) override {
        std::optional<PeerAssociation> snapshot;
        {
            std::lock_guard lock(rows_mutex);
            const auto it = rows.find(sender);
            if (it != rows.end()) snapshot = it->second;
        }
        {
            std::unique_lock lock(gate_mutex);
            if (pause_next && sender == paused_sender) {
                pause_next = false;
                observation_entered = true;
                gate_condition.notify_all();
                gate_condition.wait_for(lock, std::chrono::seconds(5),
                    [&] { return release_observation; });
            }
        }
        return snapshot;
    }
    void set(PeerAssociation peer) {
        std::lock_guard lock(rows_mutex);
        rows[peer.steam] = std::move(peer);
    }
    void remove(std::uint64_t sender) {
        std::lock_guard lock(rows_mutex);
        rows.erase(sender);
    }
    void pause_next_current(std::uint64_t sender) {
        std::lock_guard lock(gate_mutex);
        paused_sender = sender;
        pause_next = true;
        observation_entered = false;
        release_observation = false;
    }
    bool wait_for_observation() {
        std::unique_lock lock(gate_mutex);
        return gate_condition.wait_for(lock, std::chrono::seconds(2),
            [&] { return observation_entered; });
    }
    void release() {
        {
            std::lock_guard lock(gate_mutex);
            release_observation = true;
        }
        gate_condition.notify_all();
    }
};
struct SwitchingPeers final : ConnectedPeerProvider {
    std::mutex mutex;
    PeerAssociation initial;
    PeerAssociation replacement;
    bool armed{};
    unsigned calls{};

    std::optional<PeerAssociation> current(std::uint64_t sender) override {
        std::lock_guard lock(mutex);
        if (!armed) return sender == initial.steam ? std::optional(initial) : std::nullopt;
        ++calls;
        if (sender != initial.steam) return std::nullopt;
        return calls == 1 ? std::optional(initial) : std::optional(replacement);
    }
    void arm(PeerAssociation old_peer, PeerAssociation new_peer) {
        std::lock_guard lock(mutex);
        initial = std::move(old_peer);
        replacement = std::move(new_peer);
        calls = 0;
        armed = true;
    }
};
struct CallbackState {
    const ShroudweaveServerServicesV1* services{};
    void* services_context{};
    std::thread::id expected_thread{};
    std::thread::id authorize_thread{};
    std::thread::id execute_thread{};
    unsigned authorize_calls{}, execute_calls{};
    bool revoke_on_second{};
    bool current_reentry{}, recursive_pump_rejected{}, mutation_rejected{};
    std::uint64_t last_steam{}, last_connection{}, last_world{};
} callbacks;

struct ValidationCounter {
    std::atomic<unsigned> calls{};
};

struct AuthorizationGate {
    std::mutex mutex;
    std::condition_variable condition;
    unsigned calls{};
    bool first_entered{};
    bool release_first{};
    std::atomic<unsigned> executions{};
};

uint32_t SHROUDWEAVE_CALL validate_settings(void*, const char* bytes, uint32_t length) {
    try {
        const auto value = SettingsJson::parse(bytes, bytes + length);
        return value.is_object() && value.contains("enabled") && value["enabled"].is_boolean() &&
            value.contains("count") && value["count"].is_number_unsigned() ?
            SHROUDWEAVE_SERVER_STATUS_OK_V1 : SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
    } catch (...) { return SHROUDWEAVE_SERVER_STATUS_INVALID_V1; }
}
uint32_t SHROUDWEAVE_CALL validate_settings_counted(void* opaque, const char* bytes, uint32_t length) {
    static_cast<ValidationCounter*>(opaque)->calls.fetch_add(1, std::memory_order_relaxed);
    return validate_settings(nullptr, bytes, length);
}
uint32_t SHROUDWEAVE_CALL validate_action(void*, const char* action, uint32_t action_length,
    const char* bytes, uint32_t length, uint32_t* required) {
    callbacks.authorize_thread = std::this_thread::get_id();
    ++callbacks.authorize_calls;
    if (callbacks.authorize_thread != callbacks.expected_thread || !required) return SHROUDWEAVE_SERVER_STATUS_FAILED_V1;
    const std::string name(action, action_length);
    try {
        const auto values = SettingsJson::parse(bytes, bytes + length);
        if (!values.is_object()) return SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
        if (name == "outside_mask") { *required = 8; return SHROUDWEAVE_SERVER_STATUS_OK_V1; }
        if ((name != "increment" && name != "revalidate") || values.size() != 1 ||
            !values.contains("amount") || !values["amount"].is_number_unsigned() || values["amount"] == 0)
            return SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
        *required = 4;
        if (name == "revalidate" && callbacks.revoke_on_second && callbacks.authorize_calls > 1)
            return SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
        return SHROUDWEAVE_SERVER_STATUS_OK_V1;
    } catch (...) { return SHROUDWEAVE_SERVER_STATUS_INVALID_V1; }
}
uint32_t SHROUDWEAVE_CALL execute_action(void*, uint64_t steam, uint64_t connection,
    uint64_t world, const char*, uint32_t, const char*, uint32_t) {
    callbacks.execute_thread = std::this_thread::get_id();
    ++callbacks.execute_calls;
    callbacks.last_steam = steam; callbacks.last_connection = connection; callbacks.last_world = world;
    if (callbacks.execute_thread != callbacks.expected_thread) return SHROUDWEAVE_SERVER_STATUS_FAILED_V1;
    callbacks.current_reentry = callbacks.services->is_peer_current(callbacks.services_context,
        steam, connection, world) == SHROUDWEAVE_SERVER_STATUS_OK_V1;
    callbacks.recursive_pump_rejected = callbacks.services->pump_actions(callbacks.services_context,
        "alpha_actions", 13, 4) != SHROUDWEAVE_SERVER_STATUS_OK_V1;
    ShroudweaveServerSettingsPageV1 forbidden{};
    callbacks.mutation_rejected = callbacks.services->register_settings_page(
        callbacks.services_context, &forbidden) != SHROUDWEAVE_SERVER_STATUS_OK_V1;
    return SHROUDWEAVE_SERVER_STATUS_OK_V1;
}
uint32_t SHROUDWEAVE_CALL authorize_with_gate(void* opaque, const char* action,
    uint32_t action_length, const char* bytes, uint32_t length, uint32_t* required) {
    auto& gate = *static_cast<AuthorizationGate*>(opaque);
    {
        std::unique_lock lock(gate.mutex);
        ++gate.calls;
        if (gate.calls == 1) {
            gate.first_entered = true;
            gate.condition.notify_all();
            if (!gate.condition.wait_for(lock, std::chrono::seconds(10), [&] { return gate.release_first; }))
                return SHROUDWEAVE_SERVER_STATUS_FAILED_V1;
        }
    }
    if (!required || std::string_view(action, action_length) != "increment")
        return SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
    try {
        const auto values = SettingsJson::parse(bytes, bytes + length);
        if (!values.is_object() || values.size() != 1 || !values.contains("amount") ||
            !values["amount"].is_number_unsigned() || values["amount"] != 1)
            return SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
        *required = 4;
        return SHROUDWEAVE_SERVER_STATUS_OK_V1;
    } catch (...) { return SHROUDWEAVE_SERVER_STATUS_INVALID_V1; }
}
uint32_t SHROUDWEAVE_CALL execute_with_gate(void* opaque, uint64_t, uint64_t,
    uint64_t, const char*, uint32_t, const char*, uint32_t) {
    static_cast<AuthorizationGate*>(opaque)->executions.fetch_add(1, std::memory_order_relaxed);
    return SHROUDWEAVE_SERVER_STATUS_OK_V1;
}
ShroudweaveServerAuthorizationPolicyV1 open_policy() {
    ShroudweaveServerAuthorizationPolicyV1 p{};
    p.struct_size = sizeof(p); p.mode = SHROUDWEAVE_SERVER_AUTH_OPEN_CONNECTED_V1; p.revision = 1;
    return p;
}
ShroudweaveServerSettingsPageV1 page_descriptor(const char* id, const char* defaults) {
    ShroudweaveServerSettingsPageV1 p{};
    p.struct_size = sizeof(p); p.schema_version = 1; p.authorization = open_policy();
    p.page_id = id; p.page_id_length = static_cast<uint32_t>(std::strlen(id));
    p.defaults_json = defaults; p.defaults_json_length = static_cast<uint32_t>(std::strlen(defaults));
    p.validate = validate_settings;
    return p;
}
std::string read_page(const ShroudweaveServerServicesV1* s, void* ctx,
    const char* id, uint64_t* revision = nullptr) {
    char output[1024]{}; uint32_t length = 0; uint64_t rev = 0;
    const auto status = s->read_settings_page(ctx, id, static_cast<uint32_t>(std::strlen(id)),
        output, sizeof(output), &length, &rev);
    check(status == SHROUDWEAVE_SERVER_STATUS_OK_V1 && length <= sizeof(output), "settings read succeeds");
    if (revision) *revision = rev;
    return std::string(output, length);
}
}
struct MigrationProbe {
    unsigned calls{};
    uint32_t source_schema{};
    uint64_t source_revision{};
    SettingsJson source_values;
    uint64_t output_revision{20};
    bool reject{};
};
uint32_t SHROUDWEAVE_CALL migrate_settings(void* opaque, uint32_t schema, uint64_t revision,
    const char* source, uint32_t source_length, char* output, uint32_t capacity,
    uint32_t* output_length, uint64_t* output_revision) {
    auto& probe = *static_cast<MigrationProbe*>(opaque);
    ++probe.calls; probe.source_schema = schema; probe.source_revision = revision;
    try { probe.source_values = SettingsJson::parse(source, source + source_length); }
    catch (...) { probe.reject = true; }
    if (probe.reject || !output_length || !output_revision) return SHROUDWEAVE_SERVER_STATUS_FAILED_V1;
    const auto transformed = SettingsJson{{"enabled", true}, {"count", 9}}.dump();
    if (transformed.size() > capacity || transformed.size() > UINT32_MAX)
        return SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
    std::memcpy(output, transformed.data(), transformed.size());
    *output_length = static_cast<uint32_t>(transformed.size());
    *output_revision = probe.output_revision;
    return SHROUDWEAVE_SERVER_STATUS_OK_V1;
}
void migration_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    using esm::settings::SettingsKey;
    using esm::settings::SettingsStore;
    using esm::settings::StoredSettings;
    const auto valid_values = SettingsJson{{"enabled", true}, {"count", 9}};

    const auto legacy_root = root / "migration_legacy";
    SettingsStore legacy_store(legacy_root);
    check(static_cast<bool>(legacy_store.create_if_absent(SettingsKey{"shared", "legacy_alpha"},
        StoredSettings{1, 19, SettingsJson{{"legacy", 42}}})), "seed legacy document for transform");
    Peers peers; peers.rows.emplace(42, PeerAssociation{42, 5, 20, 30, false, {}, 0});
    MigrationProbe probe;
    ServerServicesRuntime migrated(peers, legacy_root, "migration-boot", 20, 30);
    check(migrated.begin_module("alpha", native_module), "migration module enters startup");
    const auto* services = migrated.services_for_current_module();
    check(services != nullptr, "migration receives startup services");
    auto page = page_descriptor("migrated_settings", R"({"enabled":false,"count":0})");
    page.schema_version = 2; page.callback_context = &probe;
    page.legacy_page_id = "legacy_alpha"; page.legacy_page_id_length = 12; page.migrate = migrate_settings;
    check(services->register_settings_page(services->services_context, &page) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "declared legacy document transforms into canonical page");
    uint64_t revision = 0;
    check(SettingsJson::parse(read_page(services, services->services_context, "migrated_settings", &revision)) == valid_values &&
        revision == 20, "migration result and explicit positive revision are persisted synchronously");
    check(probe.calls == 1 && probe.source_schema == 1 && probe.source_revision == 19 &&
        probe.source_values == SettingsJson{{"legacy", 42}}, "transform receives exact legacy metadata and values");
    const auto preserved = legacy_store.read(SettingsKey{"shared", "legacy_alpha"});
    check(preserved && preserved.value->schema_version == 1 && preserved.value->revision == 19 &&
        preserved.value->values == SettingsJson{{"legacy", 42}}, "migration preserves the legacy record");
    check(migrated.finish_module(true), "migration module startup finishes");

    const auto canonical_root = root / "migration_canonical_wins";
    SettingsStore canonical_store(canonical_root);
    check(static_cast<bool>(canonical_store.create_if_absent(SettingsKey{"shared", "current_alpha"},
        StoredSettings{2, 37, valid_values})), "seed current canonical record");
    check(static_cast<bool>(canonical_store.create_if_absent(SettingsKey{"shared", "legacy_beta"},
        StoredSettings{1, 4, SettingsJson{{"legacy", 1}}})), "seed competing legacy record");
    MigrationProbe should_not_run;
    ServerServicesRuntime canonical(peers, canonical_root, "migration-boot", 20, 30);
    check(canonical.begin_module("alpha", native_module), "canonical module enters startup");
    const auto* canonical_services = canonical.services_for_current_module();
    auto current_page = page_descriptor("current_alpha", R"({"enabled":false,"count":0})");
    current_page.schema_version = 2; current_page.legacy_page_id = "legacy_beta";
    current_page.legacy_page_id_length = 11; current_page.callback_context = &should_not_run;
    current_page.migrate = migrate_settings;
    check(canonical_services->register_settings_page(canonical_services->services_context, &current_page) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "valid canonical page takes priority over legacy source");
    revision = 0;
    check(SettingsJson::parse(read_page(canonical_services, canonical_services->services_context,
        "current_alpha", &revision)) == valid_values && revision == 37 && should_not_run.calls == 0,
        "canonical state remains unchanged and migration callback is skipped");
    check(canonical.finish_module(true), "canonical module startup finishes");
}
std::string response_status(std::optional<SettingsJson> response) {
    check(response.has_value(), "registered transport route returns a response");
    return response->value("status", "");
}
void migration_failure_contracts(const std::filesystem::path&, std::uintptr_t);
void stop_lifecycle_contracts(const std::filesystem::path&, std::uintptr_t);

const ShroudweaveServerServicesV1* start_action_provider(ServerServicesRuntime& runtime,
    std::uintptr_t native_module, const char* module_id, const char* route,
    void* callback_context, ShroudweaveServerActionAuthorizeFnV1 authorize,
    ShroudweaveServerActionExecuteFnV1 execute) {
    check(runtime.begin_module(module_id, native_module), "action fixture enters module startup");
    const auto* services = runtime.services_for_current_module();
    check(services != nullptr, "action fixture receives startup services");
    ShroudweaveServerActionProviderV1 provider{};
    provider.struct_size = sizeof(provider);
    provider.authorization = open_policy();
    provider.route_id = route;
    provider.route_id_length = static_cast<std::uint32_t>(std::strlen(route));
    provider.capability_mask = 4;
    provider.callback_context = callback_context;
    provider.authorize = authorize;
    provider.execute = execute;
    check(services->register_action_provider(services->services_context, &provider) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "action fixture registers its provider");
    check(runtime.finish_module(true) && runtime.freeze_registrations() && runtime.start_routing(),
        "action fixture freezes and starts routing");
    check(services->pump_actions(services->services_context, route,
        static_cast<std::uint32_t>(std::strlen(route)), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "action fixture records its capability heartbeat");
    return services;
}

std::string action_packet(const char* route, const char* request, const SettingsJson& session,
    std::uint64_t sequence) {
    return SettingsJson{{"v", 1}, {"request", request}, {"page", route}, {"op", "act"},
        {"session", session.at("session")}, {"sequence", sequence}, {"action", "increment"},
        {"values", {{"amount", 1}}}}.dump();
}

void reservation_guard_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    Peers peers;
    ServerServicesRuntime runtime(peers, root / "reservation_guard", "reservation-boot", 20, 30);
    check(runtime.begin_module("alpha", native_module), "reservation owner starts");
    const auto* alpha = runtime.services_for_current_module();
    check(alpha != nullptr, "reservation owner receives services");
    std::string page_id = "reserved_page";
    std::string legacy_id = "protected_legacy";
    auto page = page_descriptor(page_id.c_str(), R"({"enabled":false,"count":0})");
    MigrationProbe migration;
    page.callback_context = &migration;
    page.legacy_page_id = legacy_id.c_str();
    page.legacy_page_id_length = static_cast<std::uint32_t>(legacy_id.size());
    page.migrate = migrate_settings;
    check(alpha->register_settings_page(alpha->services_context, &page) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "page reserves its canonical and protected legacy IDs");
    auto duplicate = page_descriptor(page_id.c_str(), R"({"enabled":true,"count":1})");
    duplicate.callback_context = &migration;
    duplicate.legacy_page_id = legacy_id.c_str();
    duplicate.legacy_page_id_length = static_cast<std::uint32_t>(legacy_id.size());
    duplicate.migrate = migrate_settings;
    check(alpha->register_settings_page(alpha->services_context, &duplicate) !=
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "same-owner duplicate page registration is rejected");
    check(runtime.finish_module(true), "reservation owner finishes startup");
    check(runtime.begin_module("beta", native_module), "second module enters startup");
    const auto* beta = runtime.services_for_current_module();
    check(beta != nullptr, "second module receives services");
    ShroudweaveServerActionProviderV1 page_collision{};
    page_collision.struct_size = sizeof(page_collision);
    page_collision.authorization = open_policy();
    page_collision.route_id = page_id.c_str();
    page_collision.route_id_length = static_cast<std::uint32_t>(page_id.size());
    page_collision.capability_mask = 4;
    page_collision.authorize = validate_action;
    page_collision.execute = execute_action;
    ShroudweaveServerActionProviderV1 legacy_collision = page_collision;
    legacy_collision.route_id = legacy_id.c_str();
    legacy_collision.route_id_length = static_cast<std::uint32_t>(legacy_id.size());
    const auto page_status = beta->register_action_provider(beta->services_context, &page_collision);
    const auto legacy_status = beta->register_action_provider(beta->services_context, &legacy_collision);
    check(page_status != SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
          legacy_status != SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "failed duplicate cannot release canonical or protected legacy reservations across route kinds");
}

void startup_read_thread_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    Peers peers;
    ServerServicesRuntime starting(peers, root / "startup_read", "read-boot", 20, 30);
    check(starting.begin_module("reader", native_module), "startup-read module enters Start");
    const auto* starting_services = starting.services_for_current_module();
    check(starting_services != nullptr, "startup-read module receives services");
    ValidationCounter start_validator;
    auto page = page_descriptor("startup_page", R"({"enabled":true,"count":3})");
    page.callback_context = &start_validator;
    page.validate = validate_settings_counted;
    check(starting_services->register_settings_page(starting_services->services_context, &page) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "startup-read page initializes synchronously");
    char output[1024]{};
    std::uint32_t output_length = 0;
    std::uint64_t revision = 0;
    const auto before_same_thread = start_validator.calls.load(std::memory_order_relaxed);
    const auto same_thread_status = starting_services->read_settings_page(
        starting_services->services_context, "startup_page", 12,
        output, sizeof(output), &output_length, &revision);
    check(same_thread_status == SHROUDWEAVE_SERVER_STATUS_OK_V1 && revision != 0 &&
        start_validator.calls.load(std::memory_order_relaxed) > before_same_thread,
        "module Start can read its already-persisted page on the startup thread");
    const auto before_foreign_read = start_validator.calls.load(std::memory_order_relaxed);
    std::uint32_t foreign_status = SHROUDWEAVE_SERVER_STATUS_OK_V1;
    std::thread foreign_reader([&] {
        char foreign_output[1024]{};
        std::uint32_t foreign_length = 0;
        std::uint64_t foreign_revision = 0;
        foreign_status = starting_services->read_settings_page(
            starting_services->services_context, "startup_page", 12,
            foreign_output, sizeof(foreign_output), &foreign_length, &foreign_revision);
    });
    foreign_reader.join();
    check(foreign_status != SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        start_validator.calls.load(std::memory_order_relaxed) == before_foreign_read,
        "Start-phase read from a foreign thread is denied before page validation");
    check(starting.finish_module(false) && !starting.freeze_registrations() && !starting.start_routing(),
        "failed Start safely removes its page and blocks routing after foreign read refusal");

    ServerServicesRuntime ready(peers, root / "ready_read", "ready-read-boot", 20, 30);
    check(ready.begin_module("reader", native_module), "ready-read module enters Start");
    const auto* ready_services = ready.services_for_current_module();
    ValidationCounter ready_validator;
    auto ready_page = page_descriptor("ready_page", R"({"enabled":false,"count":5})");
    ready_page.callback_context = &ready_validator;
    ready_page.validate = validate_settings_counted;
    check(ready_services->register_settings_page(ready_services->services_context, &ready_page) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1 && ready.finish_module(true) &&
        ready.freeze_registrations() && ready.start_routing(),
        "ready-read page transitions through successful startup");
    std::uint32_t ready_status = SHROUDWEAVE_SERVER_STATUS_FAILED_V1;
    SettingsJson ready_values;
    std::thread ready_reader([&] {
        char ready_output[1024]{};
        std::uint32_t ready_length = 0;
        std::uint64_t ready_revision = 0;
        ready_status = ready_services->read_settings_page(ready_services->services_context,
            "ready_page", 10, ready_output, sizeof(ready_output), &ready_length, &ready_revision);
        if (ready_status == SHROUDWEAVE_SERVER_STATUS_OK_V1 && ready_length <= sizeof(ready_output))
            ready_values = SettingsJson::parse(ready_output, ready_output + ready_length);
    });
    ready_reader.join();
    check(ready_status == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        ready_values == SettingsJson{{"enabled", false}, {"count", 5}} &&
        ready_validator.calls.load(std::memory_order_relaxed) >= 2,
        "ready-state page reads remain valid from a foreign thread");
}

void concurrent_duplicate_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    constexpr char route[] = "race_actions";
    constexpr std::uint64_t sender = 501;
    Peers peers;
    peers.rows.emplace(sender, PeerAssociation{sender, 9001, 20, 30, false, {}, 0});
    ServerServicesRuntime runtime(peers, root / "duplicate_race", "race-boot", 20, 30);
    AuthorizationGate gate;
    const auto* services = start_action_provider(runtime, native_module, "race_module", route,
        &gate, authorize_with_gate, execute_with_gate);
    const auto read = runtime.handle(sender, SettingsJson{{"v", 1}, {"request", "race-read"},
        {"page", route}, {"op", "read"}}.dump());
    check(read && read->value("status", "") == "ok", "duplicate-race peer opens its session");
    const auto action = action_packet(route, "race-duplicate", *read, 1);
    std::optional<SettingsJson> delayed_response;
    std::thread delayed([&] { delayed_response = runtime.handle(sender, action); });
    {
        std::unique_lock lock(gate.mutex);
        check(gate.condition.wait_for(lock, std::chrono::seconds(5), [&] { return gate.first_entered; }),
            "first concurrent authorization reaches the deterministic barrier");
    }
    check(response_status(runtime.handle(sender, action)) == "queued",
        "second same-sequence request queues while the first authorization is paused");
    check(services->pump_actions(services->services_context, route,
        static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        gate.executions.load(std::memory_order_relaxed) == 1,
        "queued action completes before delayed duplicate authorization resumes");
    {
        std::lock_guard lock(gate.mutex);
        gate.release_first = true;
    }
    gate.condition.notify_all();
    delayed.join();
    const auto after_race = runtime.handle(sender, action);
    const auto race_pump = services->pump_actions(services->services_context, route,
        static_cast<std::uint32_t>(sizeof(route) - 1), 4);
    check(delayed_response && response_status(after_race) == "applied" &&
        race_pump == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        gate.executions.load(std::memory_order_relaxed) == 1,
        "late duplicate cannot overwrite the applied receipt or cause a second effect");
}

void receipt_reclamation_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    constexpr char route[] = "capacity_actions";
    constexpr std::uint64_t first_sender = 1000;
    constexpr std::uint64_t last_retired_sender = first_sender + 15;
    constexpr std::uint64_t newcomer = first_sender + 16;
    Peers peers;
    for (auto sender = first_sender; sender <= newcomer; ++sender)
        peers.rows.emplace(sender, PeerAssociation{sender, 7000 + sender, 20, 30, false, {}, 0});
    ServerServicesRuntime runtime(peers, root / "receipt_reclamation", "capacity-boot", 20, 30);
    callbacks = {};
    callbacks.expected_thread = std::this_thread::get_id();
    const auto* services = start_action_provider(runtime, native_module, "capacity_module", route,
        &callbacks, validate_action, execute_action);
    callbacks.services = services;
    callbacks.services_context = services->services_context;
    auto read_for = [&](std::uint64_t sender, const char* request) {
        return runtime.handle(sender, SettingsJson{{"v", 1}, {"request", request},
            {"page", route}, {"op", "read"}}.dump());
    };
    auto submit_and_complete = [&](std::uint64_t sender, const char* request) {
        const auto read = read_for(sender, request);
        check(read && read->value("status", "") == "ok", "connected sender receives action session");
        const auto action = action_packet(route, request, *read, 1);
        check(response_status(runtime.handle(sender, action)) == "queued",
            "session's first action queues");
        check(services->pump_actions(services->services_context, route,
            static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
            "session's queued action is pumped");
        return std::pair{*read, action};
    };
    const auto first = submit_and_complete(first_sender, "capacity-first");
    for (auto sender = first_sender + 1; sender <= last_retired_sender; ++sender) {
        const auto name = std::string("capacity-") + std::to_string(sender);
        (void)submit_and_complete(sender, name.c_str());
        peers.rows.erase(sender);
        const auto stale_read = read_for(sender, "retired-sender");
        check(!stale_read, "disconnected completed sender cannot obtain another session");
    }
    const auto effect_count = callbacks.execute_calls;
    check(response_status(runtime.handle(first_sender, first.second)) == "applied" &&
        callbacks.execute_calls == effect_count,
        "active session retains its completed sequence watermark through sender churn");
    const auto new_read = read_for(newcomer, "capacity-newcomer");
    check(new_read && new_read->value("status", "") == "ok",
        "new current sender can open a session after disconnected receipt reclamation");
    const auto new_action = action_packet(route, "capacity-new-action", *new_read, 1);
    check(response_status(runtime.handle(newcomer, new_action)) == "queued" &&
        services->pump_actions(services->services_context, route,
            static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        callbacks.execute_calls == effect_count + 1,
        "reclaimed route capacity admits and executes the newcomer without replaying history");
}

void session_replacement_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    constexpr char route[] = "session_actions";
    constexpr std::uint64_t sender = 620;
    Peers peers;
    peers.rows.emplace(sender, PeerAssociation{sender, 6200, 20, 30, false, {}, 0});
    ServerServicesRuntime runtime(peers, root / "session_replacement", "session-boot", 20, 30);
    callbacks = {};
    callbacks.expected_thread = std::this_thread::get_id();
    const auto* services = start_action_provider(runtime, native_module, "session_module", route,
        &callbacks, validate_action, execute_action);
    callbacks.services = services;
    callbacks.services_context = services->services_context;
    auto read = [&](const char* request) {
        return runtime.handle(sender, SettingsJson{{"v", 1}, {"request", request},
            {"page", route}, {"op", "read"}}.dump());
    };
    const auto old_read = read("old-session");
    check(old_read && old_read->value("status", "") == "ok", "original connection opens session");
    const auto old_action = action_packet(route, "old-action", *old_read, 1);
    check(response_status(runtime.handle(sender, old_action)) == "queued" &&
        services->pump_actions(services->services_context, route,
            static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "original connection completes sequence one");
    peers.rows[sender].connection = 6201;
    const auto new_read = read("new-session");
    check(new_read && new_read->value("status", "") == "ok" &&
        new_read->at("session") != old_read->at("session"),
        "replacement connection receives a distinct current session");
    const auto new_action = action_packet(route, "new-action", *new_read, 1);
    const auto new_reply = runtime.handle(sender, new_action);
    const auto before = callbacks.execute_calls;
    const auto replacement_pump = services->pump_actions(services->services_context, route,
        static_cast<std::uint32_t>(sizeof(route) - 1), 4);
    check(new_read->value("last_sequence", 0U) == 0U &&
        response_status(new_reply) == "queued" &&
        replacement_pump == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        callbacks.execute_calls == before + 1,
        "replacement connection does not inherit the retired session sequence or receipt");
    check(response_status(runtime.handle(sender, new_action)) == "applied",
        "replacement session sequence one executes once and retains its own receipt");
    peers.rows[sender].world = 21;
    check(!runtime.handle(sender, action_packet(route, "wrong-world", *new_read, 2)),
        "a sender moved to another world cannot read or replay this runtime's receipt");
}

void stale_cleanup_receipt_race_contracts(const std::filesystem::path& root,
                                          std::uintptr_t native_module) {
    constexpr char route[] = "cleanup_race_actions";
    constexpr std::uint64_t caller = 630;
    constexpr std::uint64_t target = 631;
    CleanupRacePeers peers;
    peers.set(PeerAssociation{caller, 6300, 20, 30, false, {}, 0});
    peers.set(PeerAssociation{target, 6310, 20, 30, false, {}, 0});
    ServerServicesRuntime runtime(peers, root / "stale_cleanup_receipt_race",
        "cleanup-race-boot", 20, 30);
    callbacks = {};
    callbacks.expected_thread = std::this_thread::get_id();
    const auto* services = start_action_provider(runtime, native_module,
        "cleanup_race_module", route, &callbacks, validate_action, execute_action);
    callbacks.services = services;
    callbacks.services_context = services->services_context;
    const auto read = [&](std::uint64_t sender, const char* request) {
        return runtime.handle(sender, SettingsJson{{"v", 1}, {"request", request},
            {"page", route}, {"op", "read"}}.dump());
    };

    const auto old_session = read(target, "cleanup-old-session");
    check(old_session && old_session->value("status", "") == "ok",
        "target's original connection receives an action session");
    const auto old_action = action_packet(route, "cleanup-old-action", *old_session, 1);
    check(response_status(runtime.handle(target, old_action)) == "queued" &&
        services->pump_actions(services->services_context, route,
            static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "target's original sequence completes before disconnect");

    peers.remove(target);
    peers.pause_next_current(target);
    std::optional<SettingsJson> stale_cleanup_response;
    std::thread cleanup([&] {
        stale_cleanup_response = read(caller, "cleanup-trigger");
    });
    check(peers.wait_for_observation(),
        "cleanup pauses after observing the target as disconnected");

    peers.set(PeerAssociation{target, 6311, 20, 30, false, {}, 0});
    const auto new_session = read(target, "cleanup-new-session");
    check(new_session && new_session->value("status", "") == "ok" &&
        new_session->value("last_sequence", 0U) == 0U,
        "reconnected target starts a new sequence space");
    const auto new_action = action_packet(route, "cleanup-new-action", *new_session, 1);
    const auto before_new_execution = callbacks.execute_calls;
    check(response_status(runtime.handle(target, new_action)) == "queued" &&
        services->pump_actions(services->services_context, route,
            static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        callbacks.execute_calls == before_new_execution + 1 && callbacks.last_connection == 6311,
        "reconnected target's sequence one completes under its new connection");

    peers.release();
    cleanup.join();
    check(stale_cleanup_response && stale_cleanup_response->value("status", "") == "ok",
        "the delayed cleanup request completes");
    check(response_status(runtime.handle(target, new_action)) == "applied" &&
        callbacks.execute_calls == before_new_execution + 1,
        "stale cleanup cannot erase a replacement receipt and make sequence one executable again");
}

void cached_sender_cleanup_race_contracts(const std::filesystem::path& root,
                                          std::uintptr_t native_module) {
    constexpr char route[] = "cached_sender_cleanup_actions";
    constexpr std::uint64_t target = 632;
    CleanupRacePeers peers;
    peers.set(PeerAssociation{target, 6320, 20, 30, false, {}, 0});
    ServerServicesRuntime runtime(peers, root / "cached_sender_cleanup_race",
        "cached-cleanup-boot", 20, 30);
    callbacks = {};
    callbacks.expected_thread = std::this_thread::get_id();
    const auto* services = start_action_provider(runtime, native_module,
        "cached_sender_cleanup_module", route, &callbacks, validate_action, execute_action);
    callbacks.services = services;
    callbacks.services_context = services->services_context;
    const auto read = [&](const char* request) {
        return runtime.handle(target, SettingsJson{{"v", 1}, {"request", request},
            {"page", route}, {"op", "read"}}.dump());
    };

    const auto old_session = read("cached-old-session");
    check(old_session && old_session->value("status", "") == "ok",
        "target's original connection receives an action session");
    const auto old_action = action_packet(route, "cached-old-action", *old_session, 1);
    check(response_status(runtime.handle(target, old_action)) == "queued" &&
        services->pump_actions(services->services_context, route,
            static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "target's original sequence completes before replacement");

    peers.pause_next_current(target);
    std::optional<SettingsJson> stale_handle_response;
    std::thread stale_handle([&] {
        stale_handle_response = read("cached-stale-read");
    });
    check(peers.wait_for_observation(),
        "old peer snapshot pauses before receipt cleanup takes its snapshot");

    peers.set(PeerAssociation{target, 6321, 20, 30, false, {}, 0});
    const auto new_session = read("cached-new-session");
    check(new_session && new_session->value("status", "") == "ok" &&
        new_session->value("last_sequence", 0U) == 0U,
        "replacement connection starts a new sequence space");
    const auto new_action = action_packet(route, "cached-new-action", *new_session, 1);
    const auto before_new_execution = callbacks.execute_calls;
    check(response_status(runtime.handle(target, new_action)) == "queued" &&
        services->pump_actions(services->services_context, route,
            static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        callbacks.execute_calls == before_new_execution + 1 && callbacks.last_connection == 6321,
        "replacement sequence one completes while the older handle is paused");

    peers.release();
    stale_handle.join();
    check(stale_handle_response && stale_handle_response->value("status", "") == "ok",
        "older cached-peer request resumes after replacement completes");
    check(response_status(runtime.handle(target, new_action)) == "applied" &&
        callbacks.execute_calls == before_new_execution + 1,
        "cleanup refreshes sender authority after receipt snapshot and preserves the replacement sequence watermark");
}

void replaced_peer_admission_contracts(const std::filesystem::path& root,
                                       std::uintptr_t native_module) {
    constexpr char route[] = "replaced_peer_actions";
    constexpr std::uint64_t sender = 640;
    SwitchingPeers peers;
    peers.initial = PeerAssociation{sender, 6400, 20, 30, false, {}, 0};
    ServerServicesRuntime runtime(peers, root / "replaced_peer_admission",
        "replaced-peer-boot", 20, 30);
    callbacks = {};
    callbacks.expected_thread = std::this_thread::get_id();
    const auto* services = start_action_provider(runtime, native_module,
        "replaced_peer_module", route, &callbacks, validate_action, execute_action);
    callbacks.services = services;
    callbacks.services_context = services->services_context;
    const auto session = runtime.handle(sender, SettingsJson{{"v", 1},
        {"request", "replaced-peer-read"}, {"page", route}, {"op", "read"}}.dump());
    check(session && session->value("status", "") == "ok",
        "original peer connection receives a session");

    peers.arm(PeerAssociation{sender, 6400, 20, 30, false, {}, 0},
        PeerAssociation{sender, 6401, 20, 30, false, {}, 0});
    const auto stale_action = action_packet(route, "replaced-peer-action", *session, 1);
    const auto reply = runtime.handle(sender, stale_action);
    check(reply && response_status(reply) != "queued",
        "action admission refuses a ticket from a replacement connection when the request captured the old connection");
    check(services->pump_actions(services->services_context, route,
        static_cast<std::uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        callbacks.execute_calls == 0,
        "no action with mismatched captured peer metadata reaches execution");
}

void atomic_commit_authority_contracts(const std::filesystem::path& root,
                                      std::string_view only = "all") {
    constexpr std::uint64_t world = 20;
    constexpr std::uint64_t configuration = 30;
    const auto schema = [](const SettingsJson& values) {
        return values.is_object() && values.size() == 1 && values.contains("enabled") &&
            values["enabled"].is_boolean();
    };
    EditPolicy policy{};
    policy.mode = EditMode::open_connected;
    policy.revision = 1;
    policy.configuration = configuration;

    if (only == "all" || only == "unsupported_provider") {
        // A legacy provider can still answer reads and initial authorization, but
        // mutation must fail closed unless it can serialize lifecycle with commit.
        Peers unsupported_peers;
        unsupported_peers.rows.emplace(71, PeerAssociation{71, 710, world, configuration, false, {}, 0});
        ConnectedAuthority unsupported_authority(unsupported_peers);
        SharedSettings unsupported(unsupported_authority, root / "unsupported",
            {"commit_test", "unsupported"}, world, policy, 1, schema);
        check(unsupported.initialize({{"enabled", false}}) == SettingsStatus::ok,
            "unsupported-provider fixture initializes canonical state");
        const auto unsupported_pending = unsupported.receive(71, 1, R"({"enabled":true})");
        check(unsupported_pending.has_value(), "unsupported provider can authorize a bounded pending edit");
        const auto unsupported_result = unsupported.execute(*unsupported_pending);
        const auto unsupported_state = unsupported.read_host();
        check(unsupported_result.status == SettingsStatus::denied && unsupported_state.snapshot &&
            unsupported_state.snapshot->revision == 1 && unsupported_state.snapshot->values["enabled"] == false,
            "provider without lifecycle serialization cannot persist a mutation");
    }

    if (only == "all" || only == "revoked_boundary") {
        // Inject a disconnect at the final serialized boundary. The earlier ticket
        // checks remain true, so only an atomic provider-plus-store boundary refuses it.
        BoundaryPeers boundary_peers;
        boundary_peers.rows.emplace(72, PeerAssociation{72, 720, world, configuration, false, {}, 0});
        ConnectedAuthority boundary_authority(boundary_peers);
        SharedSettings boundary(boundary_authority, root / "revoked_boundary",
            {"commit_test", "revoked_boundary"}, world, policy, 1, schema);
        check(boundary.initialize({{"enabled", false}}) == SettingsStatus::ok,
            "boundary fixture initializes canonical state");
        const auto boundary_pending = boundary.receive(72, 1, R"({"enabled":true})");
        check(boundary_pending.has_value(), "boundary fixture admits edit before revocation");
        boundary_peers.revoke_at_boundary = true;
        const auto boundary_result = boundary.execute(*boundary_pending);
        const auto boundary_state = boundary.read_host();
        check(boundary_result.status == SettingsStatus::denied && boundary_state.snapshot &&
            boundary_state.snapshot->revision == 1 && boundary_state.snapshot->values["enabled"] == false &&
            !boundary_peers.current(72),
            "revocation immediately before the commit boundary prevents persistence");
    }

    if (only == "all" || only == "serialized_commit") {
    // Hold the provider lifecycle lock at the final commit boundary. A concurrent
    // revocation must wait until the atomic settings replacement finishes.
    constexpr std::uint64_t guarded_sender = 73;
    CommitBarrierPeers guarded_peers;
    guarded_peers.rows.emplace(guarded_sender,
        PeerAssociation{guarded_sender, 730, world, configuration, false, {}, 0});
    ConnectedAuthority guarded_authority(guarded_peers);
    std::atomic<unsigned> validations_inside_guard{};
    const auto guarded_schema = [&](const SettingsJson& values) {
        if (guarded_peers.guard_active.load(std::memory_order_acquire))
            validations_inside_guard.fetch_add(1, std::memory_order_relaxed);
        return schema(values);
    };
    SharedSettings guarded(guarded_authority, root / "serialized_commit",
        {"commit_test", "serialized_commit"}, world, policy, 1, guarded_schema);
    check(guarded.initialize({{"enabled", false}}) == SettingsStatus::ok,
        "serialized fixture initializes canonical state");
    const auto guarded_pending = guarded.receive(guarded_sender, 1, R"({"enabled":true})");
    check(guarded_pending.has_value(), "serialized fixture admits edit before concurrent revocation");
    SettingsReply result;
    std::thread writer([&] { result = guarded.execute(*guarded_pending); });
    bool entered = false;
    {
        std::unique_lock lock(guarded_peers.event_mutex);
        entered = guarded_peers.event_condition.wait_for(lock, std::chrono::seconds(2),
            [&] { return guarded_peers.commit_entered; });
        if (!entered) guarded_peers.release_commit = true;
    }
    if (!entered) {
        guarded_peers.event_condition.notify_all();
        writer.join();
        check(false, "settings commit enters provider lifecycle serialization");
        return;
    }
    std::thread revoker([&] { guarded_peers.revoke(guarded_sender); });
    while (!guarded_peers.revoke_attempted.load(std::memory_order_acquire))
        std::this_thread::yield();
    check(guarded_peers.lifecycle_locked() &&
        !guarded_peers.revoke_completed.load(std::memory_order_acquire),
        "concurrent lifecycle revocation waits while commit holds provider serialization");
    {
        std::lock_guard lock(guarded_peers.event_mutex);
        guarded_peers.release_commit = true;
    }
    guarded_peers.event_condition.notify_all();
    writer.join();
    revoker.join();
    const auto committed = guarded.read_host();
    check(result.status == SettingsStatus::ok && committed.snapshot &&
        committed.snapshot->revision == 2 && committed.snapshot->values["enabled"] == true &&
        guarded_peers.revoke_completed.load(std::memory_order_acquire) &&
        !guarded_peers.current(guarded_sender),
        "serialized commit completes before the waiting revocation takes effect");
    check(validations_inside_guard.load(std::memory_order_relaxed) == 0,
        "module schema validation stays outside the provider lifecycle lock");
    }
}

void run_bounded_case(std::string_view name, const std::filesystem::path& root,
                      std::uintptr_t native_module) {
    if (name == "reservation_guard") reservation_guard_contracts(root, native_module);
    else if (name == "startup_read_threads") startup_read_thread_contracts(root, native_module);
    else if (name == "concurrent_duplicate") concurrent_duplicate_contracts(root, native_module);
    else if (name == "receipt_reclamation") receipt_reclamation_contracts(root, native_module);
    else if (name == "session_replacement") session_replacement_contracts(root, native_module);
    else if (name == "stale_cleanup_receipt_race") stale_cleanup_receipt_race_contracts(root, native_module);
    else if (name == "cached_sender_cleanup_race") cached_sender_cleanup_race_contracts(root, native_module);
    else if (name == "replaced_peer_admission") replaced_peer_admission_contracts(root, native_module);
    else if (name == "atomic_commit_authority") atomic_commit_authority_contracts(root);
    else if (name == "unsupported_provider") atomic_commit_authority_contracts(root, "unsupported_provider");
    else if (name == "revoked_boundary") atomic_commit_authority_contracts(root, "revoked_boundary");
    else if (name == "serialized_commit") atomic_commit_authority_contracts(root, "serialized_commit");
    else check(false, "unknown bounded runtime test case");
}

int main(int argc, char** argv) {
    check(argc == 2 || argc == 3, "requires a disposable settings root and optional case name");
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    check(!std::filesystem::exists(root), "settings root must be new");
    const auto native_module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    check(native_module != 0, "test executable has a real module handle");
    if (argc == 3) {
        run_bounded_case(argv[2], root, native_module);
        std::puts("PASS: bounded generic runtime regression");
        return 0;
    }
    Peers peers;
    peers.rows.emplace(42, PeerAssociation{42, 100, 20, 30, false, {}, 0});
    peers.rows.emplace(43, PeerAssociation{43, 200, 20, 30, false, {}, 0});
    peers.rows.emplace(44, PeerAssociation{44, 300, 20, 30, false, {}, 0});
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(100);
    uintptr_t observed_manager = 0, observed_caller = 0;
    unsigned observer_calls = 0;
    ServerServicesRuntime runtime(peers, root, "runtime-test-boot", 20, 30, [&] { return now; },
        [&](uintptr_t manager, uintptr_t caller) {
            ++observer_calls; observed_manager = manager; observed_caller = caller; return SHROUDWEAVE_SERVER_STATUS_OK_V1;
        });
    check(runtime.begin_module("alpha", native_module), "first module enters startup scope");
    const auto* alpha = runtime.services_for_current_module();
    check(alpha && alpha->abi_major == SHROUDWEAVE_SERVER_SERVICES_ABI_MAJOR_V1,
          "active module receives generic services table");
    callbacks.services = alpha; callbacks.services_context = alpha->services_context;

    auto bad_policy = page_descriptor("bad_policy", R"({"enabled":false,"count":0})");
    bad_policy.authorization.mode = 99;
    check(alpha->register_settings_page(alpha->services_context, &bad_policy) != SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "unknown authorization policy is rejected");
    auto bad_defaults = page_descriptor("bad_defaults", R"({"enabled":false})");
    check(alpha->register_settings_page(alpha->services_context, &bad_defaults) != SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "validator-rejected defaults are refused at registration");
    auto bad_id = page_descriptor("Uppercase", R"({"enabled":false,"count":0})");
    check(alpha->register_settings_page(alpha->services_context, &bad_id) != SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "noncanonical IDs are rejected");
    std::string page_id = "alpha_settings";
    std::string defaults = R"({"enabled":false,"count":0})";
    auto page = page_descriptor(page_id.c_str(), defaults.c_str());
    check(alpha->register_settings_page(alpha->services_context, &page) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
          "page registration initializes settings synchronously");
    page_id.assign("changed_after_call"); defaults.assign("{}");
    uint64_t revision = 0;
    const auto copied = read_page(alpha, alpha->services_context, "alpha_settings", &revision);
    check(SettingsJson::parse(copied) == SettingsJson{{"enabled", false}, {"count", 0}} && revision == 1,
          "registered IDs/defaults are copied and page is readable during Start");
    char short_buffer[2] = {'x', 'y'}; uint32_t required = 0; uint64_t short_revision = 0;
    const auto short_status = alpha->read_settings_page(alpha->services_context, "alpha_settings", 14,
        short_buffer, sizeof(short_buffer), &required, &short_revision);
    check(short_status == SHROUDWEAVE_SERVER_STATUS_INVALID_V1 && required > sizeof(short_buffer) &&
          short_buffer[0] == 'x' && short_buffer[1] == 'y',
          "short settings buffer reports required length without partial output");

    ShroudweaveServerActionProviderV1 provider{};
    provider.struct_size = sizeof(provider); provider.authorization = open_policy();
    std::string route_id = "alpha_actions";
    provider.route_id = route_id.c_str(); provider.route_id_length = static_cast<uint32_t>(route_id.size());
    provider.capability_mask = 4; provider.callback_context = &callbacks;
    provider.authorize = validate_action; provider.execute = execute_action;
    check(alpha->register_action_provider(alpha->services_context, &provider) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
          "generic action provider registers during module startup");
    route_id.assign("changed_after_registration");
    check(alpha->pump_actions(alpha->services_context, "alpha_actions", 13, 4) ==
          SHROUDWEAVE_SERVER_STATUS_UNAVAILABLE_V1, "action pump unavailable before routing starts");

    auto foreign_thread_page = page_descriptor("foreign_thread", R"({"enabled":false,"count":0})");
    uint32_t foreign_thread_status = SHROUDWEAVE_SERVER_STATUS_OK_V1;
    std::thread foreign_thread([&] { foreign_thread_status = alpha->register_settings_page(
        alpha->services_context, &foreign_thread_page); });
    foreign_thread.join();
    check(foreign_thread_status != SHROUDWEAVE_SERVER_STATUS_OK_V1,
          "startup registration from a foreign thread is rejected");
    check(runtime.finish_module(true), "first module startup succeeds");
    check(runtime.services_for_current_module() == nullptr, "services table is scoped to active Start");

    check(runtime.begin_module("beta", native_module), "second module enters startup scope");
    const auto* beta = runtime.services_for_current_module();
    check(beta && beta != alpha, "each module receives its own service context");
    auto beta_page = page_descriptor("beta_settings", R"({"enabled":true,"count":7})");
    check(beta->register_settings_page(beta->services_context, &beta_page) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
          "second module registers its own page");
    char beta_output[1024]{};
    check(beta->read_settings_page(beta->services_context, "alpha_settings", 14,
        beta_output, sizeof(beta_output), &required, &short_revision) != SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "second module cannot read a page owned by the first");
    auto duplicate = page_descriptor("alpha_settings", R"({"enabled":false,"count":0})");
    check(beta->register_settings_page(beta->services_context, &duplicate) != SHROUDWEAVE_SERVER_STATUS_OK_V1,
          "global page ID collision is rejected");
    auto stale_context_page = page_descriptor("stale_context", R"({"enabled":false,"count":0})");
    check(alpha->register_settings_page(alpha->services_context, &stale_context_page) !=
          SHROUDWEAVE_SERVER_STATUS_OK_V1, "prior module context cannot register during another Start");
    check(runtime.finish_module(true), "second module startup succeeds");
    check(runtime.freeze_registrations(), "registration set freezes after successful module starts");
    check(!runtime.begin_module("late", native_module), "module start cannot begin after freeze");
    check(runtime.start_routing(), "routing starts only after registrations are frozen");
    check(runtime.routing_ready(), "routing becomes ready after freeze");
    check(beta->pump_actions(beta->services_context, "alpha_actions", 13, 4) != SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "one module cannot pump another module action route");
    check(beta->register_settings_page(beta->services_context, &duplicate) !=
          SHROUDWEAVE_SERVER_STATUS_OK_V1, "late registration remains rejected after freeze");

    callbacks.expected_thread = std::this_thread::get_id();
    check(alpha->observe_creation_context != nullptr,
        "runtime exposes the optional generic creation observer tail");
    check(alpha->observe_creation_context(alpha->services_context, 0x1234, 0x5678) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1 && observer_calls == 1 &&
        observed_manager == 0x1234 && observed_caller == 0x5678,
        "creation observer forwards opaque values without authorizing an action");
    check(alpha->pump_actions(alpha->services_context, "alpha_actions", 13, 4) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "initial capability heartbeat is recorded on the caller thread");
    auto read_request = [](const char* request, const char* route) {
        return SettingsJson{{"v", 1}, {"request", request}, {"page", route}, {"op", "read"}}.dump();
    };
    auto session42 = runtime.handle(42, read_request("read42", "alpha_actions"));
    auto session43 = runtime.handle(43, read_request("read43", "alpha_actions"));
    check(session42 && session43 && session42->value("status", "") == "ok" &&
          session43->value("status", "") == "ok", "connected peers receive independent action sessions");
    const auto make_action = [](const char* request, const SettingsJson& read, uint64_t seq,
                                const char* action) {
        return SettingsJson{{"v", 1}, {"request", request}, {"page", "alpha_actions"}, {"op", "act"},
            {"session", read.at("session")}, {"sequence", seq}, {"action", action},
            {"values", {{"amount", 1}}}}.dump();
    };
    const auto action42 = make_action("act42", *session42, 1, "increment");
    const auto action43 = make_action("act43", *session43, 1, "increment");
    check(response_status(runtime.handle(42, action42)) == "queued" &&
          response_status(runtime.handle(43, action43)) == "queued",
          "current sessions queue module-authorized actions");
    check(response_status(runtime.handle(42, action42)) == "queued",
        "duplicate sequence returns its queued receipt without replay");
    auto changed_sequence = SettingsJson::parse(action42);
    changed_sequence["values"]["amount"] = 2;
    check(response_status(runtime.handle(42, changed_sequence.dump())) != "queued",
        "same sequence with a different payload is rejected");
    const auto before_pump = callbacks.execute_calls;
    check(alpha->pump_actions(alpha->services_context, "alpha_actions", 13, 4) ==
          SHROUDWEAVE_SERVER_STATUS_OK_V1, "registered provider can be pumped when routing is ready");
    check(callbacks.execute_calls == before_pump + 1, "one pump executes at most one pending action");
    check(callbacks.authorize_thread == callbacks.expected_thread && callbacks.execute_thread == callbacks.expected_thread,
          "validation and synchronous native callback run on the pump/transport caller thread");
    check(callbacks.last_world == 20 && callbacks.current_reentry,
          "effect receives host world and read-only current-peer reentry succeeds without deadlock");
    check(callbacks.recursive_pump_rejected && callbacks.mutation_rejected,
          "recursive pump and callback-time registration mutation are rejected");
    check(alpha->pump_actions(alpha->services_context, "alpha_actions", 13, 4) ==
          SHROUDWEAVE_SERVER_STATUS_OK_V1 && callbacks.execute_calls == before_pump + 2,
          "a second call pumps the next pending action");
    check(response_status(runtime.handle(42, action42)) == "applied" && callbacks.execute_calls == before_pump + 2,
        "completed duplicate returns its receipt without repeating the effect");
    const auto result42 = SettingsJson{{"v",1},{"request","result42"},{"page","alpha_actions"},{"op","result"},
        {"session",session42->at("session")},{"sequence",1u}};
    check(response_status(runtime.handle(42, result42.dump())) == "applied" &&
        response_status(runtime.handle(43, result42.dump())) != "applied",
        "result receipt is visible only to its bound peer session");

    auto outside = SettingsJson{{"v",1},{"request","outside"},{"page","alpha_actions"},{"op","act"},
        {"session",session42->at("session")},{"sequence",2u},{"action","outside_mask"},
        {"values",SettingsJson::object()}};
    check(response_status(runtime.handle(42, outside.dump())) != "queued",
          "action requiring bits outside provider mask is rejected");
    callbacks.authorize_calls = 0; callbacks.revoke_on_second = true;
    const auto revalidate = make_action("revalidate", *session42, 2, "revalidate");
    check(response_status(runtime.handle(42, revalidate)) == "queued",
          "valid action may queue before second validation revokes it");
    const auto before_revalidation = callbacks.execute_calls;
    check(alpha->pump_actions(alpha->services_context, "alpha_actions", 13, 4) ==
          SHROUDWEAVE_SERVER_STATUS_OK_V1 && callbacks.execute_calls == before_revalidation,
          "module authorization is rechecked immediately before effect");
    auto session44 = runtime.handle(44, read_request("read44", "alpha_actions"));
    check(session44 && session44->value("status", "") == "ok", "additional connected peer receives a session");
    const auto expiring_action = make_action("expire44", *session44, 1, "increment");
    check(response_status(runtime.handle(44, expiring_action)) == "queued", "pending action queues before expiry");
    const auto before_expiry = callbacks.execute_calls;
    now += std::chrono::hours(24);
    check(alpha->pump_actions(alpha->services_context, "alpha_actions", 13, 4) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1 && callbacks.execute_calls == before_expiry,
        "expired pending work is never executed");
    const auto result44 = SettingsJson{{"v",1},{"request","result44"},{"page","alpha_actions"},{"op","result"},
        {"session",session44->at("session")},{"sequence",1u}};
    check(response_status(runtime.handle(44, result44.dump())) == "expired",
        "expired work retains an explicit receipt");

    auto fresh42 = runtime.handle(42, read_request("fresh42", "alpha_actions"));
    check(fresh42 && fresh42->value("status", "") == "ok", "current peer can open a fresh session");
    const auto stale_action = make_action("stale", *fresh42, 3, "increment");
    check(response_status(runtime.handle(42, stale_action)) == "queued", "fresh peer action queues");
    const auto before_disconnect = callbacks.execute_calls;
    peers.rows[42].connection = 101;
    check(alpha->pump_actions(alpha->services_context, "alpha_actions", 13, 4) ==
          SHROUDWEAVE_SERVER_STATUS_OK_V1 && callbacks.execute_calls == before_disconnect,
          "connection replacement before execution revokes queued work");
    check(alpha->is_peer_current(alpha->services_context, 42, 100, 20) ==
          SHROUDWEAVE_SERVER_STATUS_UNAVAILABLE_V1, "read-only peer check refuses stale connection");
    migration_contracts(root / "migrations", native_module);
    migration_failure_contracts(root / "migration_failures", native_module);
    stop_lifecycle_contracts(root, native_module);
    reservation_guard_contracts(root, native_module);
    startup_read_thread_contracts(root, native_module);
    concurrent_duplicate_contracts(root, native_module);
    receipt_reclamation_contracts(root, native_module);
    session_replacement_contracts(root, native_module);
    stale_cleanup_receipt_race_contracts(root, native_module);
    cached_sender_cleanup_race_contracts(root, native_module);
    replaced_peer_admission_contracts(root, native_module);
    atomic_commit_authority_contracts(root / "atomic_commit_authority");
    std::puts("PASS: generic settings and action runtime integration");
}
void migration_failure_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    using esm::settings::SettingsKey;
    using esm::settings::SettingsStore;
    using esm::settings::StoreStatus;
    using esm::settings::StoredSettings;
    const auto valid_values = SettingsJson{{"enabled", true}, {"count", 9}};
    Peers peers; peers.rows.emplace(42, PeerAssociation{42, 5, 20, 30, false, {}, 0});

    const auto bad_canonical_root = root / "invalid_canonical";
    SettingsStore bad_canonical_store(bad_canonical_root);
    check(static_cast<bool>(bad_canonical_store.create_if_absent(SettingsKey{"shared", "broken_alpha"},
        StoredSettings{99, 7, valid_values})), "seed unsupported canonical schema");
    check(static_cast<bool>(bad_canonical_store.create_if_absent(SettingsKey{"shared", "legacy_broken"},
        StoredSettings{1, 5, SettingsJson{{"legacy", 5}}})), "seed valid fallback legacy record");
    MigrationProbe skipped;
    ServerServicesRuntime invalid_canonical(peers, bad_canonical_root, "migration-boot", 20, 30);
    check(invalid_canonical.begin_module("alpha", native_module), "invalid-canonical module enters startup");
    const auto* invalid_services = invalid_canonical.services_for_current_module();
    auto invalid_page = page_descriptor("broken_alpha", R"({"enabled":false,"count":0})");
    invalid_page.schema_version = 2; invalid_page.legacy_page_id = "legacy_broken";
    invalid_page.legacy_page_id_length = 13; invalid_page.callback_context = &skipped;
    invalid_page.migrate = migrate_settings;
    check(invalid_services->register_settings_page(invalid_services->services_context, &invalid_page) !=
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "invalid present canonical page fails closed without legacy fallback");
    const auto unchanged = bad_canonical_store.read(SettingsKey{"shared", "broken_alpha"});
    check(unchanged && unchanged.value->schema_version == 99 && unchanged.value->revision == 7 &&
        unchanged.value->values == valid_values && skipped.calls == 0,
        "invalid canonical remains unchanged and legacy transform is never called");
    (void)invalid_canonical.finish_module(false);

    const auto bad_transform_root = root / "invalid_transform";
    SettingsStore bad_transform_store(bad_transform_root);
    check(static_cast<bool>(bad_transform_store.create_if_absent(SettingsKey{"shared", "legacy_gamma"},
        StoredSettings{1, 5, SettingsJson{{"legacy", 5}}})), "seed legacy record for invalid transform");
    MigrationProbe bad_revision; bad_revision.output_revision = 0;
    ServerServicesRuntime invalid_transform(peers, bad_transform_root, "migration-boot", 20, 30);
    check(invalid_transform.begin_module("gamma", native_module), "invalid-transform module enters startup");
    const auto* transform_services = invalid_transform.services_for_current_module();
    auto transform_page = page_descriptor("transform_gamma", R"({"enabled":false,"count":0})");
    transform_page.schema_version = 2; transform_page.legacy_page_id = "legacy_gamma";
    transform_page.legacy_page_id_length = 12; transform_page.callback_context = &bad_revision;
    transform_page.migrate = migrate_settings;
    check(transform_services->register_settings_page(transform_services->services_context, &transform_page) !=
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "zero migration revision is refused");
    check(bad_transform_store.read(SettingsKey{"shared", "transform_gamma"}).status == StoreStatus::not_found &&
        bad_transform_store.read(SettingsKey{"shared", "legacy_gamma"}).value->revision == 5,
        "invalid transform creates no canonical record and preserves legacy source");
    (void)invalid_transform.finish_module(false);

    const auto failed_start_root = root / "failed_start_persistence";
    ServerServicesRuntime failed_start(peers, failed_start_root, "migration-boot", 20, 30);
    check(failed_start.begin_module("gamma", native_module), "failed-start module enters startup");
    const auto* failed_services = failed_start.services_for_current_module();
    auto persisted_page = page_descriptor("persisted_gamma", R"({"enabled":true,"count":9})");
    check(failed_services->register_settings_page(failed_services->services_context, &persisted_page) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "settings initialize during module Start");
    (void)failed_start.finish_module(false);
    const bool failed_start_froze = failed_start.freeze_registrations();
    const bool failed_start_routed = failed_start.start_routing();
    check(!failed_start_froze && !failed_start_routed && !failed_start.routing_ready(),
        "failed required module permanently blocks freeze and routing");
    check(!failed_start.routing_ready(), "failed required module cannot enable transport routing");
    ServerServicesRuntime retry(peers, failed_start_root, "retry-boot", 20, 30);
    check(retry.begin_module("gamma", native_module), "retry runtime starts the same module identity");
    const auto* retry_services = retry.services_for_current_module();
    auto retry_page = page_descriptor("persisted_gamma", R"({"enabled":false,"count":0})");
    check(retry_services->register_settings_page(retry_services->services_context, &retry_page) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "retry registers persisted canonical page");
    uint64_t revision = 0;
    check(SettingsJson::parse(read_page(retry_services, retry_services->services_context,
        "persisted_gamma", &revision)) == SettingsJson{{"enabled", true}, {"count", 9}} && revision == 1,
        "failed Start does not erase or reset settings already persisted by synchronous initialization");
    check(retry.finish_module(true), "retry module startup finishes");
}

void stop_lifecycle_contracts(const std::filesystem::path& root, std::uintptr_t native_module) {
    Peers peers;
    peers.rows.emplace(45, PeerAssociation{45, 500, 20, 30, false, {}, 0});
    ServerServicesRuntime stopped(peers, root / "stop_lifecycle", "stop-boot", 20, 30);
    check(stopped.begin_module("stopper", native_module), "shutdown module enters startup");
    const auto* services = stopped.services_for_current_module();
    check(services != nullptr, "shutdown module receives services");
    callbacks.services = services;
    callbacks.services_context = services->services_context;
    callbacks.expected_thread = std::this_thread::get_id();
    callbacks.revoke_on_second = false;
    callbacks.authorize_calls = 0;

    ShroudweaveServerActionProviderV1 provider{};
    provider.struct_size = sizeof(provider);
    provider.authorization = open_policy();
    constexpr char route[] = "stop_actions";
    provider.route_id = route;
    provider.route_id_length = static_cast<uint32_t>(sizeof(route) - 1);
    provider.capability_mask = 4;
    provider.callback_context = &callbacks;
    provider.authorize = validate_action;
    provider.execute = execute_action;
    check(services->register_action_provider(services->services_context, &provider) ==
        SHROUDWEAVE_SERVER_STATUS_OK_V1, "shutdown provider registers before routing");
    check(stopped.finish_module(true) && stopped.freeze_registrations() && stopped.start_routing(),
        "shutdown runtime becomes routable after successful startup");
    check(services->pump_actions(services->services_context, route,
        static_cast<uint32_t>(sizeof(route) - 1), 4) == SHROUDWEAVE_SERVER_STATUS_OK_V1,
        "shutdown provider records its capability heartbeat");

    const auto read = stopped.handle(45,
        SettingsJson{{"v", 1}, {"request", "stop-read"}, {"page", route}, {"op", "read"}}.dump());
    check(read && read->value("status", "") == "ok", "connected peer opens a shutdown-test session");
    const auto action = SettingsJson{{"v", 1}, {"request", "stop-action"}, {"page", route},
        {"op", "act"}, {"session", read->at("session")}, {"sequence", 1u},
        {"action", "increment"}, {"values", {{"amount", 1}}}}.dump();
    check(response_status(stopped.handle(45, action)) == "queued",
        "shutdown fixture queues an action before stop");

    const auto before_stop = callbacks.execute_calls;
    stopped.stop_routing();
    check(!stopped.routing_ready(), "stop permanently clears routing readiness");
    check(services->pump_actions(services->services_context, route,
        static_cast<uint32_t>(sizeof(route) - 1), 4) != SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        callbacks.execute_calls == before_stop,
        "stopped runtime discards queued work without executing it");
    const auto after_stop = SettingsJson{{"v", 1}, {"request", "after-stop"},
        {"page", route}, {"op", "read"}}.dump();
    check(!stopped.handle(45, after_stop), "stopped runtime rejects new ingress");
    check(!stopped.start_routing(), "stopped runtime cannot restart routing");
}
