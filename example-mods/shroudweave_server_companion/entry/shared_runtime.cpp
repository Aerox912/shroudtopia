#include "shared_runtime.hpp"
#include "connections.hpp"
#include "server_services_runtime.hpp"
#if !defined(SHROUDWEAVE_HOST_TEST_SEAMS)
#include "steam_channel.hpp"
#include <thread>
#endif
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace shroudweave::server {
namespace {
struct Configuration {
    std::optional<SettingsJson> document;
    std::string state_id = "server";
};
bool missing(DWORD error) noexcept { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
bool directory(const std::filesystem::path& path, bool allow_missing) noexcept {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return allow_missing && missing(GetLastError());
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
std::optional<Configuration> read_configuration(const std::filesystem::path& file) {
    const auto attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return missing(GetLastError()) ? std::optional<Configuration>{Configuration{}} : std::nullopt;
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || !size || size > SHROUDWEAVE_SERVER_MAX_JSON_BYTES_V1) return {};
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    const std::string bytes(std::istreambuf_iterator<char>{input}, {});
    if (input.bad() || bytes.size() != size) return {};
    auto json = parse_settings_json(bytes);
    if (!json || !json->is_object() || json->size() != 2 || !json->contains("schema") ||
        !(*json)["schema"].is_number_integer() || (*json)["schema"] != 1 ||
        !json->contains("state_id") || !(*json)["state_id"].is_string()) return {};
    auto id = (*json)["state_id"].get<std::string>();
    if (!esm::settings::is_canonical_settings_id(id)) return {};
    return Configuration{std::move(json), std::move(id)};
}

// Query the actual registry under its owned lock. The final SDK-only authority
// check/store commit can retain that lock; runtime.handle and all module
// validators, migrations, authorization and native effects remain outside it.
class RegistryProvider final : public ConnectedPeerProvider {
    const std::uint64_t world_;
    std::atomic<bool> available_{true};
public:
    explicit RegistryProvider(std::uint64_t world) : world_(world) {}
    void disable() noexcept { available_.store(false, std::memory_order_release); }
    bool healthy() noexcept {
        if (!available_.load(std::memory_order_acquire)) return false;
        try {
            bool same = false;
            with_connections([&](ConnectionRegistry& peers) { same = peers.epoch() == world_; });
            if (!same) disable();
            return same && available_.load(std::memory_order_acquire);
        } catch (...) { disable(); return false; }
    }
    std::optional<PeerAssociation> current(std::uint64_t sender) override {
        if (!sender || !available_.load(std::memory_order_acquire)) return {};
        try {
            std::optional<PeerAssociation> result;
            with_connections([&](ConnectionRegistry& peers) {
                if (peers.epoch() != world_) { disable(); return; }
                result = peers.current(sender);
            });
            if (!available_.load(std::memory_order_acquire)) return {};
            return result;
        } catch (...) { disable(); return {}; }
    }
    bool with_current(std::uint64_t sender,
        const std::function<void(const PeerAssociation&)>& work) override {
        if (!sender || !work || !available_.load(std::memory_order_acquire)) return false;
        try {
            bool completed = false;
            with_connections([&](ConnectionRegistry& peers) {
                if (!available_.load(std::memory_order_acquire)) return;
                if (peers.epoch() != world_) { disable(); return; }
                const auto peer = peers.current(sender);
                if (!peer || peer->steam != sender || !peer->connection || peer->world != world_ ||
                    peer->configuration != 1 || !available_.load(std::memory_order_acquire)) return;
                // Private SDK work only: compare the supplied snapshot with
                // the ticket/policy and perform the final store CAS. Never
                // invoke module callbacks or recursively query this provider.
                work(*peer);
                completed = true;
            });
            return completed;
        } catch (...) {
            // A throwing SDK commit may have had a storage side effect. This
            // disables further authority without claiming rollback/no write.
            disable();
            return false;
        }
    }
};
struct SharedState {
    std::filesystem::path file;
    Configuration configuration;
    RegistryProvider peers;
    ServerServicesRuntime runtime;
    void (*audit)(const char*);
    std::atomic<bool> stopped{}, routing_attempted{};
    SharedState(std::filesystem::path config_file, Configuration config, std::filesystem::path store_root,
        std::string boot, std::uint64_t world, void (*log)(const char*))
        : file(std::move(config_file)), configuration(std::move(config)), peers(world),
          runtime(peers, std::move(store_root), std::move(boot), world, 1), audit(log) {}
};
std::atomic<SharedState*> shared{};
std::atomic<bool> preparation_attempted{};

void stop(SharedState& state) noexcept {
    state.peers.disable();
    state.runtime.stop_routing();
    state.stopped.store(true, std::memory_order_release);
    // A native effect callback already entered may return normally. Stop
    // prevents future admission; it is not cancellation or native exclusion.
}

#if !defined(SHROUDWEAVE_HOST_TEST_SEAMS)
void transport_worker(SharedState* state) noexcept {
    try {
        SteamChannel channel;
        while (!state->stopped.load(std::memory_order_acquire)) {
            const auto config = read_configuration(state->file);
            if (!config || config->document != state->configuration.document || !state->peers.healthy())
                throw std::runtime_error("configuration or native connection epoch changed; restart required");
            // Steam may not be initialized until after native entry resumes.
            // The worker uses the game's context and never creates a callback pump.
            channel.initialize(true, [state](std::uint64_t sender) {
                return !state->stopped.load(std::memory_order_acquire) && state->peers.current(sender).has_value();
            });
            channel.poll([state, &channel](std::uint64_t sender, std::string_view packet) {
                if (state->stopped.load(std::memory_order_acquire)) return;
                const auto reply = state->runtime.handle(sender, packet);
                if (reply && !state->stopped.load(std::memory_order_acquire)) channel.send(sender, reply->dump());
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (...) {
        stop(*state);
        if (state->audit) state->audit("SHROUDWEAVE SERVER: transport unavailable; routing stopped, restart required");
    }
}
#endif
}

ServerServicesRuntime* prepare_shared_runtime(HMODULE game, const std::filesystem::path& root,
    void (*audit)(const char*)) noexcept {
    try {
        if (preparation_attempted.exchange(true, std::memory_order_acq_rel) || !game || !audit ||
            !root.is_absolute() || !directory(root, false)) return nullptr;
        const auto file = root / L"shroudweave-services.json";
        auto config = read_configuration(file);
        if (!config) return nullptr;
        const auto state_base = root / L"shroudweave-state";
        const auto store_root = state_base / config->state_id;
        if (!directory(state_base, true) || !directory(store_root, true)) return nullptr;
        if (!install_connections(game)) return nullptr;
        std::uint64_t world{};
        with_connections([&](ConnectionRegistry& peers) { world = peers.epoch(); });
        if (!world) return nullptr;
        unsigned char random[16]{};
        if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return nullptr;
        std::string boot;
        constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : random) { boot += hex[byte >> 4]; boot += hex[byte & 15]; }
        auto state = std::make_unique<SharedState>(file, std::move(*config), store_root, std::move(boot), world, audit);
        auto* result = state.release(); // Runtime/provider/callback context are process-long, including failure.
        shared.store(result, std::memory_order_release);
        return &result->runtime;
    } catch (...) { return nullptr; }
}

bool start_shared_runtime_routing(ServerServicesRuntime& services) noexcept {
    auto* state = shared.load(std::memory_order_acquire);
    if (!state || &state->runtime != &services || state->stopped.load(std::memory_order_acquire) ||
        state->routing_attempted.exchange(true, std::memory_order_acq_rel)) return false;
    try {
        if (!state->peers.healthy() || !services.start_routing()) { stop(*state); return false; }
#if defined(SHROUDWEAVE_HOST_TEST_SEAMS)
        if (!host_test::start_transport(services, state->audit)) { stop(*state); return false; }
#else
        std::thread worker(transport_worker, state);
        try { worker.detach(); }
        catch (...) {
            stop(*state);
            if (worker.joinable()) worker.join();
            return false;
        }
#endif
        return !state->stopped.load(std::memory_order_acquire) && services.routing_ready();
    } catch (...) { stop(*state); return false; }
}

void stop_shared_runtime() noexcept {
    if (auto* state = shared.load(std::memory_order_acquire)) stop(*state);
}
}
