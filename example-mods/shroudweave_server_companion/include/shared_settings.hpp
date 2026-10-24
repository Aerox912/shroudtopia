#pragma once
#include "connected_authority.hpp"
#include <esm/settings_store.hpp>
#include <functional>
#include <string_view>

namespace shroudweave::server {
using SettingsJson = esm::settings::Json;
std::optional<SettingsJson> parse_settings_json(std::string_view bytes);
enum class SettingsStatus { ok, denied, invalid, conflict, unavailable };
struct SettingsReply {
    SettingsStatus status = SettingsStatus::unavailable;
    std::optional<esm::settings::StoredSettings> snapshot;
};

// One instance per registered module/page in a host-selected world directory.
// The host must serialize this service AND native peer/policy lifecycle changes
// on its authority dispatcher. No API here starts a worker or network session.
// Provider callbacks, authority and service must outlive all pending requests.
class SharedSettings final {
public:
    using Validator = std::function<bool(const SettingsJson&)>;
    class Pending {
        friend class SharedSettings;
        const SharedSettings* owner_;
        ConnectedAuthority::Ticket ticket_;
        std::uint64_t revision_;
        SettingsJson values_;
        Pending(const SharedSettings* owner, ConnectedAuthority::Ticket ticket,
                std::uint64_t revision, SettingsJson values)
            : owner_(owner), ticket_(std::move(ticket)), revision_(revision), values_(std::move(values)) {}
    public:
        Pending(const Pending&) = default;
    };
    SharedSettings(ConnectedAuthority&, std::filesystem::path world_root,
                   esm::settings::SettingsKey, std::uint64_t world, EditPolicy,
                   std::uint32_t schema, Validator);
    SharedSettings(const SharedSettings&) = delete;
    SharedSettings& operator=(const SharedSettings&) = delete;
    // Host registration only. Existing state is validated, never overwritten
    // with defaults. This method is not exposed as a client message operation.
    SettingsStatus initialize(const SettingsJson& server_defaults);
    // Module-owned host API read. The services manager enforces page ownership;
    // this bypasses transport-sender authorization only for that trusted path.
    SettingsReply read_host();
    void replace_policy(EditPolicy policy) { policy_ = policy; }
    SettingsReply read(std::uint64_t transport_sender);
    std::optional<Pending> receive(std::uint64_t transport_sender,
                                  std::uint64_t expected_revision, std::string_view values_json);
    SettingsReply execute(const Pending&);
private:
    ConnectedAuthority& authority_;
    esm::settings::SettingsStore store_;
    esm::settings::SettingsKey key_;
    std::uint64_t world_;
    EditPolicy policy_;
    std::uint32_t schema_;
    Validator validator_;
    bool ready_ = false;
    bool valid(const SettingsJson&) const;
    SettingsReply snapshot();
};
}
