#include "shared_settings.hpp"
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace shroudweave::server {
namespace {
constexpr std::size_t max_bytes = 48 * 1024;
constexpr std::size_t max_nodes = 4096;
constexpr int max_depth = 24;
}
std::optional<SettingsJson> parse_settings_json(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > max_bytes) return std::nullopt;
    std::size_t nodes = 0;
    bool rejected = false;
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, SettingsJson::parse_event_t event, SettingsJson& value) {
        if (depth > max_depth || ++nodes > max_nodes) throw std::runtime_error("settings limit");
        if (event == SettingsJson::parse_event_t::object_start) keys.emplace_back();
        if (event == SettingsJson::parse_event_t::key &&
            !keys.back().insert(value.get<std::string>()).second) rejected = true;
        if (event == SettingsJson::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    try {
        auto value = SettingsJson::parse(bytes.begin(), bytes.end(), callback);
        if (!rejected && value.is_object()) return value;
    } catch (...) {}
    return std::nullopt;
}
SharedSettings::SharedSettings(ConnectedAuthority& authority, std::filesystem::path root,
    esm::settings::SettingsKey key, std::uint64_t world, EditPolicy policy,
    std::uint32_t schema, Validator validator)
    : authority_(authority), store_(std::move(root)), key_(std::move(key)), world_(world),
      policy_(policy), schema_(schema), validator_(std::move(validator)) {}

bool SharedSettings::valid(const SettingsJson& values) const {
    // Defaults, persisted documents and requests pass the same bounded shape
    // and module-owned schema. Validators must be pure and may not mutate game state.
    try {
        return validator_ && parse_settings_json(values.dump()).has_value() && validator_(values);
    } catch (...) { return false; }
}
SettingsStatus SharedSettings::initialize(const SettingsJson& defaults) {
    ready_ = false;
    if (!world_ || !schema_ || !policy_.configuration || !policy_.revision || !valid(defaults))
        return SettingsStatus::invalid;
    auto current = store_.read(key_);
    if (current.status == esm::settings::StoreStatus::not_found) {
        const auto saved = store_.create_if_absent(key_, {schema_, 1, defaults});
        if (!saved && saved.status != esm::settings::StoreStatus::conflict)
            return SettingsStatus::unavailable;
        current = store_.read(key_);
    }
    if (!current || !current.value || current.value->schema_version != schema_ ||
        !current.value->revision || !valid(current.value->values)) return SettingsStatus::unavailable;
    ready_ = true;
    return SettingsStatus::ok;
}
SettingsReply SharedSettings::read_host() {
    if (!ready_) return {SettingsStatus::unavailable, {}};
    return snapshot();
}
SettingsReply SharedSettings::snapshot() {
    const auto current = store_.read(key_);
    if (!current || !current.value || current.value->schema_version != schema_ ||
        !current.value->revision || !valid(current.value->values))
        return {SettingsStatus::unavailable, {}};
    return {SettingsStatus::ok, current.value};
}
SettingsReply SharedSettings::read(std::uint64_t sender) {
    if (!ready_) return {SettingsStatus::unavailable, {}};
    if (!authority_.can_read(sender, world_, policy_.configuration)) return {SettingsStatus::denied, {}};
    return snapshot();
}
std::optional<SharedSettings::Pending> SharedSettings::receive(
    std::uint64_t sender, std::uint64_t revision, std::string_view bytes) {
    if (!ready_ || !revision) return std::nullopt;
    auto ticket = authority_.receive(sender, policy_);
    if (!ticket || ticket->world() != world_) return std::nullopt;
    auto values = parse_settings_json(bytes);
    if (!values || !valid(*values)) return std::nullopt;
    return Pending(this, *ticket, revision, std::move(*values));
}
SettingsReply SharedSettings::execute(const Pending& pending) {
    if (!ready_) return {SettingsStatus::unavailable, {}};
    if (pending.owner_ != this || pending.ticket_.world() != world_ ||
        !authority_.revalidate(pending.ticket_, policy_)) return {SettingsStatus::denied, {}};
    if (!valid(pending.values_)) return {SettingsStatus::invalid, {}};
    auto current = snapshot();
    if (current.status != SettingsStatus::ok) return current;
    if (current.snapshot->revision != pending.revision_) {
        current.status = SettingsStatus::conflict;
        return current;
    }
    if (pending.revision_ == std::numeric_limits<std::uint64_t>::max())
        return {SettingsStatus::unavailable, {}};
    // Keep the final atomic write inside the provider's connection lifecycle
    // serialization. Validation and the conflict read above stay outside it.
    esm::settings::StoredSettings next{schema_, pending.revision_ + 1, pending.values_};
    esm::settings::StoreResult saved;
    bool attempted = false;
    const bool authorized = authority_.with_current(pending.ticket_, policy_, [&] {
        attempted = true;
        try {
            saved = store_.compare_exchange(key_, pending.revision_, next);
        } catch (...) {
            saved.status = esm::settings::StoreStatus::io_error;
        }
    });
    if (!authorized || !attempted) return {SettingsStatus::denied, {}};
    if (saved.status == esm::settings::StoreStatus::conflict) {
        auto latest = snapshot();
        if (latest.status == SettingsStatus::ok) latest.status = SettingsStatus::conflict;
        return latest;
    }
    if (!saved) return {SettingsStatus::unavailable, {}};
    // Only persisted data is acknowledged. The transport adapter may now send
    // this revision/snapshot; reconnect always uses read(), never local uploads.
    return {SettingsStatus::ok, std::move(next)};
}
}
