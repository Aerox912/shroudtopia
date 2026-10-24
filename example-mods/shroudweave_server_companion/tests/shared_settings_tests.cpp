#include "shared_settings.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <barrier>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <mutex>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
using namespace shroudweave::server;
void check(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
struct Peers final : ConnectedPeerProvider {
    std::map<std::uint64_t, PeerAssociation> peers;
    std::mutex lifecycle_mutex;
    std::optional<PeerAssociation> current(std::uint64_t sender) override {
        std::lock_guard lock(lifecycle_mutex);
        const auto p = peers.find(sender);
        return p == peers.end() ? std::nullopt : std::optional(p->second);
    }
    bool with_current(std::uint64_t sender,
        const std::function<void(const PeerAssociation&)>& operation) override {
        std::lock_guard lock(lifecycle_mutex);
        const auto p = peers.find(sender);
        if (p == peers.end()) return false;
        operation(p->second);
        return true;
    }
};
std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
std::filesystem::path find_settings_record(const std::filesystem::path& root,
    const esm::settings::SettingsKey& key) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const auto bytes = read_bytes(entry.path());
        const auto document = SettingsJson::parse(bytes, nullptr, false);
        if (!document.is_discarded() && document.is_object() &&
            document.contains("module_id") && document.contains("page_id") &&
            document["module_id"] == key.module_id && document["page_id"] == key.page_id) {
            return entry.path();
        }
    }
    return {};
}
void settings_store_contracts(const std::filesystem::path& root) {
    using esm::settings::SettingsKey;
    using esm::settings::SettingsStore;
    using esm::settings::StoredSettings;
    using esm::settings::StoreStatus;

    const SettingsKey initial_key{"settings_test", "revision_seed"};
    SettingsStore initial_store(root / "initial");
    check(initial_store.read(initial_key).status == StoreStatus::not_found,
          "new settings key is absent before create-if-absent");
    const StoredSettings initial{2, 17, {{"count", 6}, {"label", "legacy"}}};
    check(initial_store.create_if_absent(initial_key, initial).status == StoreStatus::ok,
          "create-if-absent accepts arbitrary positive starting revision");
    auto stored = initial_store.read(initial_key);
    check(stored && stored.value->schema_version == 2 && stored.value->revision == 17 &&
          stored.value->values == initial.values,
          "create-if-absent persists full supplied record at exact revision");
    const auto initial_path = find_settings_record(root / "initial", initial_key);
    check(!initial_path.empty(), "created record is present on disk");
    const auto original_bytes = read_bytes(initial_path);
    auto alternate = initial;
    alternate.revision = 88;
    alternate.values["label"] = "replacement";
    check(initial_store.create_if_absent(initial_key, alternate).status == StoreStatus::conflict,
          "valid occupied target reports conflict");
    check(read_bytes(initial_path) == original_bytes &&
          initial_store.read(initial_key).value->values == initial.values &&
          initial_store.read(initial_key).value->revision == 17,
          "existing canonical bytes and value win without replacement");

    auto advanced = initial;
    advanced.revision = 18;
    advanced.values["count"] = 7;
    check(initial_store.compare_exchange(initial_key, 17, advanced).status == StoreStatus::ok,
          "normal compare-exchange advances one revision after arbitrary seed");
    auto skipped = advanced;
    skipped.revision = 20;
    const auto advanced_bytes = read_bytes(initial_path);
    check(!initial_store.compare_exchange(initial_key, 18, skipped),
          "normal compare-exchange still rejects revision jumps");
    check(read_bytes(initial_path) == advanced_bytes &&
          initial_store.read(initial_key).value->revision == 18,
          "rejected revision jump preserves persisted record");

    const SettingsKey malformed_key{"settings_test", "malformed_target"};
    SettingsStore malformed_store(root / "malformed");
    check(malformed_store.create_if_absent(malformed_key, StoredSettings{1, 9, {{"ok", true}}}).status == StoreStatus::ok,
          "seed target before malformed-target test");
    const auto malformed_path = find_settings_record(root / "malformed", malformed_key);
    check(!malformed_path.empty(), "locate target before corruption fixture");
    {
        std::ofstream output(malformed_path, std::ios::binary | std::ios::trunc);
        output << "{";
        check(static_cast<bool>(output), "write malformed existing target fixture");
    }
    const auto malformed_bytes = read_bytes(malformed_path);
    check(malformed_store.create_if_absent(malformed_key, StoredSettings{1, 1, {{"replacement", true}}}).status == StoreStatus::conflict,
          "malformed occupied target reports conflict rather than replacing it");
    check(read_bytes(malformed_path) == malformed_bytes &&
          malformed_store.read(malformed_key).status == StoreStatus::corrupt_data,
          "malformed target bytes remain unchanged and visible");

    const SettingsKey invalid_schema_key{"settings_test", "zero_schema"};
    check(!initial_store.create_if_absent(invalid_schema_key, StoredSettings{0, 1, SettingsJson::object()}) &&
          initial_store.read(invalid_schema_key).status == StoreStatus::not_found,
          "zero schema version is rejected without creating a record");
    const SettingsKey invalid_revision_key{"settings_test", "zero_revision"};
    check(!initial_store.create_if_absent(invalid_revision_key, StoredSettings{1, 0, SettingsJson::object()}) &&
          initial_store.read(invalid_revision_key).status == StoreStatus::not_found,
          "zero initial revision is rejected without creating a record");
    const SettingsKey invalid_values_key{"settings_test", "non_object_values"};
    check(!initial_store.create_if_absent(invalid_values_key, StoredSettings{1, 1, SettingsJson::array()}) &&
          initial_store.read(invalid_values_key).status == StoreStatus::not_found,
          "non-object values are rejected without creating a record");

    const SettingsKey race_key{"settings_test", "race"};
    SettingsStore concurrent_store(root / "concurrent");
    const StoredSettings left{3, 31, {{"writer", "left"}}};
    const StoredSettings right{4, 47, {{"writer", "right"}}};
    std::barrier start(3);
    esm::settings::StoreResult left_result;
    esm::settings::StoreResult right_result;
    std::thread left_thread([&] { start.arrive_and_wait(); left_result = concurrent_store.create_if_absent(race_key, left); });
    std::thread right_thread([&] { start.arrive_and_wait(); right_result = concurrent_store.create_if_absent(race_key, right); });
    start.arrive_and_wait();
    left_thread.join();
    right_thread.join();
    const bool left_won = left_result.status == StoreStatus::ok && right_result.status == StoreStatus::conflict;
    const bool right_won = right_result.status == StoreStatus::ok && left_result.status == StoreStatus::conflict;
    check(left_won || right_won, "concurrent create-if-absent has exactly one winner");
    const auto winner = concurrent_store.read(race_key);
    check(winner && ((winner.value->schema_version == left.schema_version && winner.value->revision == left.revision && winner.value->values == left.values) ||
                     (winner.value->schema_version == right.schema_version && winner.value->revision == right.revision && winner.value->values == right.values)),
          "concurrent winner is the complete record written by one caller");
}
int main(int argc, char** argv) {
    check(argc == 2, "requires disposable test root");
    const auto root = std::filesystem::absolute(argv[1]);
    check(!std::filesystem::exists(root), "test root must be new");
    std::filesystem::create_directories(root);
    settings_store_contracts(root / "store-contracts");
    Peers peers;
    peers.peers[42] = {42, 1, 20, 30, false, "", 0};
    peers.peers[43] = {43, 2, 20, 30, false, "", 0};
    ConnectedAuthority authority(peers);
    EditPolicy open{};
    open.mode = EditMode::open_connected;
    open.revision = 1;
    open.configuration = 30;
    const auto schema = [](const SettingsJson& j) {
        return j.size() == 1 && j.contains("enabled") && j["enabled"].is_boolean();
    };
    SharedSettings settings(authority, root, {"settings_test", "test"}, 20, open, 1, schema);
    check(settings.read(42).status == SettingsStatus::unavailable, "no service before host initialization");
    check(settings.initialize({{"enabled", false}}) == SettingsStatus::ok, "server defaults persist");
    check(settings.read(999).status == SettingsStatus::denied, "unknown transport sender cannot read");
    check(settings.read(42).snapshot->revision == 1, "first snapshot has server revision");
    auto first = settings.receive(42, 1, R"({"enabled":true})");
    auto concurrent = settings.receive(43, 1, R"({"enabled":false})");
    check(first && concurrent, "two connected players may submit in open mode");
    auto accepted = settings.execute(*first);
    check(accepted.status == SettingsStatus::ok && accepted.snapshot->revision == 2,
          "write acknowledged after persistence");
    auto conflict = settings.execute(*concurrent);
    check(conflict.status == SettingsStatus::conflict && conflict.snapshot->values["enabled"] == true &&
          conflict.snapshot->revision == 2, "stale edit returns current server snapshot");
    check(settings.execute(*first).status == SettingsStatus::conflict, "replay cannot apply twice");
    check(settings.read(43).snapshot->values == accepted.snapshot->values, "second reader sees server state");
    SharedSettings restarted(authority, root, {"settings_test", "test"}, 20, open, 1, schema);
    check(restarted.initialize({{"enabled", false}}) == SettingsStatus::ok &&
          restarted.read(42).snapshot->values["enabled"] == true &&
          restarted.read(42).snapshot->revision == 2, "restart preserves data and revision over defaults");
    check(restarted.execute(*first).status == SettingsStatus::denied, "ticket cannot cross service instance");
    auto queued = settings.receive(42, 2, R"({"enabled":false})");
    peers.peers.erase(42);
    check(settings.execute(*queued).status == SettingsStatus::denied, "disconnect cancels queued edit");
    peers.peers[42] = {42, 3, 20, 30, false, "", 0};
    check(settings.execute(*queued).status == SettingsStatus::denied, "reconnect cannot revive queue");
    check(settings.read(42).snapshot->revision == 2, "reconnect reads without uploading local data");
    queued = settings.receive(42, 2, R"({"enabled":false})");
    auto protected_policy = open;
    protected_policy.mode = EditMode::exact_groups; // Even an omitted revision bump revokes.
    settings.replace_policy(protected_policy);
    check(settings.execute(*queued).status == SettingsStatus::denied, "policy change cancels pending edit");
    check(!settings.receive(42, 2, R"({"enabled":false})"), "protected mode never falls back to open");
    check(settings.read(42).status == SettingsStatus::ok, "connected reader need not have edit role");
    settings.replace_policy(open);
    check(!settings.receive(999, 2, R"({"enabled":false})"), "unknown sender cannot edit");
    check(!settings.receive(42, 0, R"({"enabled":false})"), "clients cannot initialize state");
    check(!settings.receive(42, 2, R"({"enabled":false,"enabled":true})"), "duplicate keys rejected");
    check(!settings.receive(42, 2, R"({"enabled":"true"})"), "schema type rejected");
    check(!settings.receive(42, 2, R"({"enabled":true,"steam":42})"), "client authority field rejected by schema");
    check(!settings.receive(42, 2, std::string(50000, ' ')), "oversized input rejected");
    check(!settings.receive(42, 2, "{"), "malformed JSON rejected");
    const auto nested = std::string("{\"x\":") + std::string(30, '[') + "0" + std::string(30, ']') + "}";
    check(!settings.receive(42, 2, nested), "deep JSON rejected before module validation");
    peers.peers[42].world = 21;
    check(!settings.receive(42, 2, R"({"enabled":false})") &&
          settings.read(42).status == SettingsStatus::denied, "different world cannot access this service");
    peers.peers[42].world = 20;
    SharedSettings wrong_schema(authority, root, {"settings_test", "test"}, 20, open, 2, schema);
    check(wrong_schema.initialize({{"enabled", false}}) == SettingsStatus::unavailable,
          "schema migration never silently resets server state");
    // A real filesystem failure must never produce an accepted revision.
    const auto invalid_root = root / "regular-file";
    std::ofstream(invalid_root) << "not a directory";
    SharedSettings broken(authority, invalid_root, {"settings_test", "test"}, 20, open, 1, schema);
    check(broken.initialize({{"enabled", false}}) == SettingsStatus::unavailable &&
          !broken.receive(42, 1, R"({"enabled":true})"), "storage failure prevents edits");
    // Hold the actual record open without delete sharing to block atomic replacement.
    const auto record = find_settings_record(root,
        esm::settings::SettingsKey{"settings_test", "test"});
    check(!record.empty(), "persisted document exists");
    queued = settings.receive(42, 2, R"({"enabled":false})");
    const HANDLE lock = CreateFileW(record.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "lock real settings record");
    check(settings.execute(*queued).status == SettingsStatus::unavailable,
          "atomic replace failure cannot acknowledge edit");
    CloseHandle(lock);
    check(settings.read(42).snapshot->revision == 2 && settings.read(42).snapshot->values["enabled"] == true,
          "failed write preserves revision and previous server value");
    std::puts("PASS: shared settings admission, conflicts, persistence, reconnect and limits");
}
