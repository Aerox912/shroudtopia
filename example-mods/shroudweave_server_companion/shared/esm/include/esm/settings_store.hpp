#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace esm::settings {

using Json = nlohmann::json;

inline constexpr std::size_t kMaxSettingsIdBytes = 64U;
inline constexpr std::size_t kMaxStoredDocumentBytes = 64U * 1024U;

// Matches the loader manifest's canonical lowercase ASCII ID grammar:
// [a-z0-9][a-z0-9._-]{0,63}. IDs are stored and compared byte-for-byte.
[[nodiscard]] bool is_canonical_settings_id(std::string_view value) noexcept;

struct SettingsKey final {
    std::string module_id;
    std::string page_id;
};

// Declarative schema metadata and JSON field values only. The registry owns
// declaration/type/migration validation; this store validates storage shape.
struct StoredSettings final {
    std::uint32_t schema_version = 1U;
    std::uint64_t revision = 0U;
    Json values = Json::object();
};

enum class StoreStatus : std::uint32_t {
    ok = 0U,
    not_found,
    conflict,
    invalid_key,
    invalid_value,
    corrupt_data,
    unsupported_format,
    revision_overflow,
    unsafe_path,
    io_error
};

struct ReadResult final {
    StoreStatus status = StoreStatus::io_error;
    std::optional<StoredSettings> value;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return status == StoreStatus::ok; }
};

struct StoreResult final {
    StoreStatus status = StoreStatus::io_error;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return status == StoreStatus::ok; }
};

// Loader-internal storage bound to a host-selected profile or authority-world
// directory. Keys contain IDs only; module callers never select filesystem paths.
class SettingsStore final {
public:
    explicit SettingsStore(std::filesystem::path host_root);

    [[nodiscard]] ReadResult read(const SettingsKey& key) const;
    // Atomically creates a validated positive-revision document only when the
    // canonical target does not exist. An occupied target is never replaced.
    [[nodiscard]] StoreResult create_if_absent(const SettingsKey& key,
                                               const StoredSettings& initial) const;
    [[nodiscard]] StoreResult compare_exchange(const SettingsKey& key,
                                               std::uint64_t expected_revision,
                                               const StoredSettings& next) const;

private:
    std::filesystem::path host_root_;
};

}  // namespace esm::settings
