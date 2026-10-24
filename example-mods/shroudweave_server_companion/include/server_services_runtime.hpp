#pragma once

#include "connected_authority.hpp"
#include "server_services_v1.h"
#include "shared_settings.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace shroudweave::server {

// Production host for module-owned settings and action services. The host
// supplies the startup world/configuration epochs; modules cannot select them.
class ServerServicesRuntime final {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Observer = std::function<std::uint32_t(std::uintptr_t manager,
                                                std::uintptr_t native_caller)>;

    ServerServicesRuntime(ConnectedPeerProvider& peers,
        std::filesystem::path settings_root, std::string boot,
        std::uint64_t host_world, std::uint64_t host_configuration,
        Clock clock = {}, Observer observer = {});
    ~ServerServicesRuntime();

    ServerServicesRuntime(const ServerServicesRuntime&) = delete;
    ServerServicesRuntime& operator=(const ServerServicesRuntime&) = delete;
    ServerServicesRuntime(ServerServicesRuntime&&) = delete;
    ServerServicesRuntime& operator=(ServerServicesRuntime&&) = delete;

    // These lifecycle calls are made on the startup thread. While a module is
    // active, its services table is available only for that module's Start.
    bool begin_module(std::string_view module_id, std::uintptr_t native_module) noexcept;
    const ShroudweaveServerServicesV1* services_for_current_module() const noexcept;
    bool finish_module(bool start_succeeded) noexcept;
    bool freeze_registrations() noexcept;

    // Transport routing and action pumping are unavailable until this succeeds.
    bool start_routing() noexcept;
    bool routing_ready() const noexcept;
    // Permanently disables ingress and pumping. A callback already entered on
    // its caller thread is allowed to return; queued work is discarded.
    void stop_routing() noexcept;
    std::optional<SettingsJson> handle(std::uint64_t transport_sender,
                                       std::string_view packet);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shroudweave::server
