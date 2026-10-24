#include "server_services_runtime.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace shroudweave::server {
namespace {
constexpr std::uint32_t kOk = SHROUDWEAVE_SERVER_STATUS_OK_V1;
constexpr std::uint32_t kUnavailable = SHROUDWEAVE_SERVER_STATUS_UNAVAILABLE_V1;
constexpr std::uint32_t kInvalid = SHROUDWEAVE_SERVER_STATUS_INVALID_V1;
constexpr std::uint32_t kFailed = SHROUDWEAVE_SERVER_STATUS_FAILED_V1;
constexpr std::size_t kMaxRoutes = 32;
constexpr std::size_t kMaxReceiptsPerRoute = 16;
constexpr auto kReceiptLifetime = std::chrono::seconds(5);
constexpr auto kCapabilityHeartbeat = std::chrono::seconds(1);

enum class CallbackPhase { none, validation, migration, authorization, execution, observer };
struct CallbackFrame final {
    const void* runtime{};
    CallbackPhase phase = CallbackPhase::none;
};
thread_local CallbackFrame g_callback_frame;

class CallbackScope final {
public:
    CallbackScope(const void* runtime, CallbackPhase phase) noexcept
        : previous_(g_callback_frame) {
        g_callback_frame = {runtime, phase};
    }
    ~CallbackScope() { g_callback_frame = previous_; }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
private:
    CallbackFrame previous_;
};

bool valid_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > SHROUDWEAVE_SERVER_MAX_ID_BYTES_V1 ||
        value.front() < 'a' || value.front() > 'z') return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool copy_id(const char* source, std::uint32_t length, std::string& output) {
    if (!source || !length || length > SHROUDWEAVE_SERVER_MAX_ID_BYTES_V1) return false;
    output.assign(source, length);
    return valid_id(output);
}

bool callback_in_module(std::uintptr_t module, std::uintptr_t address) noexcept {
    if (!module || !address) return false;
    HMODULE actual{};
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    return GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &actual) != FALSE &&
        reinterpret_cast<std::uintptr_t>(actual) == module;
}

std::optional<std::string> values_bytes(const SettingsJson& values) {
    try {
        const auto bytes = values.dump(-1, ' ', false, SettingsJson::error_handler_t::strict);
        if (bytes.empty() || bytes.size() > SHROUDWEAVE_SERVER_MAX_JSON_BYTES_V1 ||
            !parse_settings_json(bytes)) return std::nullopt;
        return bytes;
    } catch (...) {
        return std::nullopt;
    }
}

bool valid_authorization(const ShroudweaveServerAuthorizationPolicyV1& source,
                         EditPolicy& output) {
    if (source.struct_size < sizeof(ShroudweaveServerAuthorizationPolicyV1) || !source.revision)
        return false;
    std::string group;
    if (source.selected_group_length > 256U) return false;
    if (source.selected_group_length != 0U) {
        if (!source.selected_group) return false;
        group.assign(source.selected_group, source.selected_group_length);
        if (group.find('\0') != std::string::npos) return false;
    } else if (source.selected_group) {
        return false;
    }

    output.revision = source.revision;
    output.groups_verified = false;
    output.selected_group_permissions = 0;
    output.selected_group.clear();
    switch (source.mode) {
    case SHROUDWEAVE_SERVER_AUTH_EXACT_GROUP_V1:
        if (group.empty() || source.selected_group_permissions) return false;
        output.mode = EditMode::exact_groups;
        output.groups_verified = true;
        output.selected_group = std::move(group);
        return true;
    case SHROUDWEAVE_SERVER_AUTH_EQUIVALENT_PERMISSIONS_V1:
        if (!group.empty() || !source.selected_group_permissions) return false;
        output.mode = EditMode::equivalent_permissions;
        output.groups_verified = true;
        output.selected_group_permissions = source.selected_group_permissions;
        return true;
    case SHROUDWEAVE_SERVER_AUTH_OPEN_CONNECTED_V1:
        if (!group.empty() || source.selected_group_permissions) return false;
        output.mode = EditMode::open_connected;
        return true;
    default:
        return false;
    }
}

const char* policy_name(EditMode mode) noexcept {
    switch (mode) {
    case EditMode::exact_groups: return "exact_group";
    case EditMode::equivalent_permissions: return "equivalent_permissions";
    case EditMode::open_connected: return "open_connected";
    }
    return "unavailable";
}

const char* settings_status_name(SettingsStatus status) noexcept {
    switch (status) {
    case SettingsStatus::ok: return "ok";
    case SettingsStatus::denied: return "denied";
    case SettingsStatus::invalid: return "invalid";
    case SettingsStatus::conflict: return "conflict";
    case SettingsStatus::unavailable: return "unavailable";
    }
    return "unavailable";
}

bool usable_peer(const std::optional<PeerAssociation>& peer, std::uint64_t sender,
                 std::uint64_t world, std::uint64_t configuration) noexcept {
    return peer && peer->steam == sender && sender && peer->connection &&
        peer->world == world && peer->configuration == configuration;
}
}

class ServerServicesRuntime::Impl final {
public:
    enum class ModuleState { starting, ready, failed };
    struct Module;
    struct Page final {
        Module* owner{};
        std::string id;
        std::uint32_t schema{};
        SettingsJson defaults;
        EditPolicy policy;
        std::unique_ptr<SharedSettings> service;
    };
    struct Provider final {
        Module* owner{};
        std::string id;
        EditPolicy policy;
        std::uint32_t capability_mask{};
        void* callback_context{};
        ShroudweaveServerActionAuthorizeFnV1 authorize{};
        ShroudweaveServerActionExecuteFnV1 execute{};
        std::uint32_t available_capabilities{};
        std::chrono::steady_clock::time_point heartbeat{};
    };
    struct Module final {
        Impl* runtime{};
        std::string id;
        std::uintptr_t native_module{};
        ModuleState state = ModuleState::starting;
        ShroudweaveServerServicesV1 services{};
        std::vector<std::string> owned_ids;
    };
    using ReceiptKey = std::tuple<std::string, std::uint64_t, std::uint64_t, std::uint64_t>;
    struct Receipt final {
        PeerAssociation peer;
        std::optional<ConnectedAuthority::Ticket> ticket;
        std::uint64_t sequence{};
        std::string command;
        std::string status;
        std::chrono::steady_clock::time_point expires{};
        std::uint32_t required_capabilities{};
        // Set under mutex_ immediately before entering the module's native
        // effect callback. stop_routing invalidates queued work before this
        // boundary and lets a callback beyond it finish.
        bool effect_started{};
    };
    struct ReservationGuard final {
        Impl* runtime{};
        Module* owner{};
        const std::string* first{};
        const std::string* second{};
        bool committed{};
        bool first_reserved{};
        bool second_reserved{};
        ~ReservationGuard() {
            if (runtime && !committed) runtime->release_reservations(
                owner, first, first_reserved, second, second_reserved);
        }
    };

    Impl(ConnectedPeerProvider& peers, std::filesystem::path root, std::string boot,
         std::uint64_t world, std::uint64_t configuration, Clock clock, Observer observer)
        : peers_(peers), authority_(peers), root_(std::move(root)), boot_(std::move(boot)),
          world_(world), configuration_(configuration), clock_(std::move(clock)),
          observer_(std::move(observer)), startup_thread_(std::this_thread::get_id()) {
        if (!clock_) clock_ = [] { return std::chrono::steady_clock::now(); };
    }

    bool begin_module(std::string_view id, std::uintptr_t native_module) noexcept {
        try {
            if (reentrant(ServiceOperation::mutation)) return false;
            if (!valid_id(id) || !native_module || !world_ || !configuration_) return false;
            std::lock_guard lock(mutex_);
            if (std::this_thread::get_id() != startup_thread_ || active_ || frozen_ || routing_ready_ ||
                stopped_ || failed_start_ ||
                module_ids_.contains(id)) return false;
            auto module = std::make_unique<Module>();
            module->runtime = this;
            module->id.assign(id);
            module->native_module = native_module;
            module->services = {sizeof(ShroudweaveServerServicesV1),
                SHROUDWEAVE_SERVER_SERVICES_ABI_MAJOR_V1, module.get(),
                &register_settings_page, &register_action_provider, &read_settings_page,
                &pump_actions, &is_peer_current, &observe_creation_context};
            auto* current = module.get();
            modules_.push_back(std::move(module));
            module_ids_.emplace(current->id, current);
            active_ = current;
            return true;
        } catch (...) {
            return false;
        }
    }

    const ShroudweaveServerServicesV1* services_for_current_module() const noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (std::this_thread::get_id() != startup_thread_ || !active_ ||
                active_->state != ModuleState::starting) return nullptr;
            return &active_->services;
        } catch (...) {
            return nullptr;
        }
    }

    bool finish_module(bool succeeded) noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (std::this_thread::get_id() != startup_thread_ || !active_ ||
                active_->state != ModuleState::starting) return false;
            Module* module = active_;
            active_ = nullptr;
            if (succeeded) {
                module->state = ModuleState::ready;
                return true;
            }
            module->state = ModuleState::failed;
            failed_start_ = true;
            for (const auto& id : module->owned_ids) {
                pages_.erase(id);
                providers_.erase(id);
                reserved_ids_.erase(id);
            }
            module->owned_ids.clear();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool freeze_registrations() noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (std::this_thread::get_id() != startup_thread_ || active_ || frozen_ || routing_ready_ ||
                stopped_ || failed_start_)
                return false;
            frozen_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool start_routing() noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (std::this_thread::get_id() != startup_thread_ || active_ || !frozen_ || routing_ready_ ||
                stopped_ || failed_start_)
                return false;
            routing_ready_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool routing_ready() const noexcept {
        try {
            std::lock_guard lock(mutex_);
            return routing_ready_;
        } catch (...) {
            return false;
        }
    }

    void stop_routing() noexcept {
        try {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            routing_ready_ = false;
            bool effect_in_progress = false;
            for (auto& [key, receipt] : receipts_) {
                (void)key;
                if (receipt.status == "queued" ||
                    (receipt.status == "executing" && !receipt.effect_started)) {
                    receipt.status = "unavailable";
                    receipt.ticket.reset();
                    receipt.required_capabilities = 0;
                } else if (receipt.status == "executing" && receipt.effect_started) {
                    effect_in_progress = true;
                }
            }
            if (!effect_in_progress) pumping_ = false;
        } catch (...) {
            // The routing flag is normally set under the mutex. Keep this
            // best-effort shutdown method noexcept if locking itself fails.
        }
    }

    std::optional<SettingsJson> handle(std::uint64_t sender, std::string_view packet) {
        try {
        if (reentrant(ServiceOperation::mutation) || !sender) return {};
        const auto request = parse_settings_json(packet);
        if (!request || !request->contains("v") || !(*request)["v"].is_number_integer() ||
            (*request)["v"] != 1 || !request->contains("request") ||
            !(*request)["request"].is_string() ||
            (*request)["request"].get_ref<const std::string&>().empty() ||
            (*request)["request"].get_ref<const std::string&>().size() > 64 ||
            !request->contains("page") || !(*request)["page"].is_string() ||
            !request->contains("op") || !(*request)["op"].is_string()) return {};

        const auto id = (*request)["page"].get<std::string>();
        const auto op = (*request)["op"].get<std::string>();
        Page* page{};
        Provider* provider{};
        {
            std::lock_guard lock(mutex_);
            if (!routing_ready_ || stopped_) return {};
            const auto page_it = pages_.find(id);
            if (page_it != pages_.end()) page = page_it->second.get();
            const auto provider_it = providers_.find(id);
            if (provider_it != providers_.end()) provider = provider_it->second.get();
        }
        if (!page && !provider) {
            return SettingsJson{{"v", 1}, {"request", (*request)["request"]},
                {"page", id}, {"status", "unavailable"}};
        }
        const auto peer = current_peer(sender);
        if (!usable_peer(peer, sender, world_, configuration_)) return {};
        return page ? handle_settings(*page, sender, *peer, *request, op, id)
                    : handle_actions(*provider, sender, *peer, *request, op, id);
        } catch (...) {
            return {};
        }
    }

private:
    enum class ServiceOperation { mutation, read, current_peer, pump, observer };
    ConnectedPeerProvider& peers_;
    ConnectedAuthority authority_;
    std::filesystem::path root_;
    std::string boot_;
    std::uint64_t world_{};
    std::uint64_t configuration_{};
    Clock clock_;
    Observer observer_;
    std::thread::id startup_thread_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::map<std::string, Module*, std::less<>> module_ids_;
    std::map<std::string, Module*, std::less<>> reserved_ids_;
    std::map<std::string, std::unique_ptr<Page>, std::less<>> pages_;
    std::map<std::string, std::unique_ptr<Provider>, std::less<>> providers_;
    std::map<ReceiptKey, Receipt> receipts_;
    Module* active_{};
    bool frozen_{};
    bool routing_ready_{};
    bool stopped_{};
    bool failed_start_{};
    bool pumping_{};

    bool reentrant(ServiceOperation operation) const noexcept {
        if (g_callback_frame.runtime != this) return false;
        return !(operation == ServiceOperation::current_peer &&
                 g_callback_frame.phase == CallbackPhase::execution);
    }

    bool module_starting(Module* module) const noexcept {
        try {
            std::lock_guard lock(mutex_);
            return module && module->runtime == this && active_ == module &&
                module->state == ModuleState::starting &&
                std::this_thread::get_id() == startup_thread_ && !frozen_ && !routing_ready_;
        } catch (...) {
            return false;
        }
    }

    bool module_readable(Module* module) const noexcept {
        try {
            std::lock_guard lock(mutex_);
            return module && module->runtime == this &&
                (module->state == ModuleState::ready ||
                 (module->state == ModuleState::starting && active_ == module &&
                  std::this_thread::get_id() == startup_thread_ && !frozen_ && !routing_ready_));
        } catch (...) {
            return false;
        }
    }

    void release_reservations(Module* owner, const std::string* first, bool first_reserved,
                              const std::string* second, bool second_reserved) noexcept {
        try {
            std::lock_guard lock(mutex_);
            const auto erase_if_owner = [this, owner](const std::string* id) {
                if (!id) return;
                const auto it = reserved_ids_.find(*id);
                if (it != reserved_ids_.end() && it->second == owner) reserved_ids_.erase(it);
            };
            if (first_reserved) erase_if_owner(first);
            if (second_reserved) erase_if_owner(second);
        } catch (...) {}
    }

    std::chrono::steady_clock::time_point now() const noexcept {
        try { return clock_(); } catch (...) { return std::chrono::steady_clock::now(); }
    }

    std::optional<PeerAssociation> current_peer(std::uint64_t sender) noexcept {
        try { return peers_.current(sender); } catch (...) { return {}; }
    }

    static std::uint32_t SHROUDWEAVE_CALL register_settings_page(
        void* context, const ShroudweaveServerSettingsPageV1* page) noexcept {
        auto* module = static_cast<Module*>(context);
        if (!module || !module->runtime) return kInvalid;
        return module->runtime->register_page(module, page);
    }

    static std::uint32_t SHROUDWEAVE_CALL register_action_provider(
        void* context, const ShroudweaveServerActionProviderV1* provider) noexcept {
        auto* module = static_cast<Module*>(context);
        if (!module || !module->runtime) return kInvalid;
        return module->runtime->register_provider(module, provider);
    }

    static std::uint32_t SHROUDWEAVE_CALL read_settings_page(
        void* context, const char* page_id, std::uint32_t page_id_length,
        char* output_values_json, std::uint32_t output_capacity,
        std::uint32_t* output_values_json_length, std::uint64_t* output_revision) noexcept {
        auto* module = static_cast<Module*>(context);
        if (!module || !module->runtime) return kInvalid;
        return module->runtime->read_page(module, page_id, page_id_length,
            output_values_json, output_capacity, output_values_json_length, output_revision);
    }

    static std::uint32_t SHROUDWEAVE_CALL pump_actions(
        void* context, const char* route_id, std::uint32_t route_id_length,
        std::uint32_t available_capabilities) noexcept {
        auto* module = static_cast<Module*>(context);
        if (!module || !module->runtime) return kInvalid;
        return module->runtime->pump(module, route_id, route_id_length, available_capabilities);
    }

    static std::uint32_t SHROUDWEAVE_CALL is_peer_current(
        void* context, std::uint64_t peer_steam, std::uint64_t connection,
        std::uint64_t world) noexcept {
        auto* module = static_cast<Module*>(context);
        if (!module || !module->runtime) return kInvalid;
        return module->runtime->peer_current(module, peer_steam, connection, world);
    }

    static std::uint32_t SHROUDWEAVE_CALL observe_creation_context(
        void* context, std::uintptr_t manager, std::uintptr_t native_caller) noexcept {
        auto* module = static_cast<Module*>(context);
        if (!module || !module->runtime) return kInvalid;
        return module->runtime->observe(module, manager, native_caller);
    }

    std::uint32_t register_page(Module* module,
        const ShroudweaveServerSettingsPageV1* descriptor) noexcept {
        if (reentrant(ServiceOperation::mutation) || !module_starting(module) || !descriptor)
            return kInvalid;
        try {
            if (descriptor->struct_size < sizeof(ShroudweaveServerSettingsPageV1) ||
                !descriptor->schema_version || !descriptor->validate ||
                descriptor->authorization.struct_size < sizeof(ShroudweaveServerAuthorizationPolicyV1) ||
                descriptor->defaults_json_length == 0 ||
                descriptor->defaults_json_length > SHROUDWEAVE_SERVER_MAX_JSON_BYTES_V1 ||
                !descriptor->defaults_json ||
                !callback_in_module(module->native_module,
                    reinterpret_cast<std::uintptr_t>(descriptor->validate))) return kInvalid;

            std::string id;
            std::string legacy_id;
            if (!copy_id(descriptor->page_id, descriptor->page_id_length, id)) return kInvalid;
            const bool has_legacy = descriptor->legacy_page_id_length != 0 ||
                descriptor->legacy_page_id != nullptr || descriptor->migrate != nullptr;
            if (has_legacy) {
                if (!descriptor->legacy_page_id || !descriptor->migrate ||
                    !copy_id(descriptor->legacy_page_id, descriptor->legacy_page_id_length, legacy_id) ||
                    legacy_id == id ||
                    !callback_in_module(module->native_module,
                        reinterpret_cast<std::uintptr_t>(descriptor->migrate))) return kInvalid;
            }

            EditPolicy policy;
            if (!valid_authorization(descriptor->authorization, policy)) return kInvalid;
            policy.configuration = configuration_;
            const std::string defaults_bytes(descriptor->defaults_json, descriptor->defaults_json_length);
            auto defaults = parse_settings_json(defaults_bytes);
            if (!defaults) return kInvalid;

            ReservationGuard reservation{this, module, &id,
                has_legacy ? &legacy_id : nullptr, false, false, false};
            {
                std::lock_guard lock(mutex_);
                if (std::this_thread::get_id() != startup_thread_ || active_ != module ||
                    module->state != ModuleState::starting || frozen_ || routing_ready_ ||
                    pages_.size() + providers_.size() >= kMaxRoutes ||
                    reserved_ids_.contains(id) || (has_legacy && reserved_ids_.contains(legacy_id)))
                    return kInvalid;
                const auto [canonical_it, canonical_inserted] = reserved_ids_.emplace(id, module);
                (void)canonical_it;
                if (!canonical_inserted) return kInvalid;
                reservation.first_reserved = true;
                if (has_legacy) {
                    const auto [legacy_it, legacy_inserted] = reserved_ids_.emplace(legacy_id, module);
                    (void)legacy_it;
                    if (!legacy_inserted) return kInvalid;
                    reservation.second_reserved = true;
                }
            }

            const auto validate_values = [this, callback = descriptor->validate,
                callback_context = descriptor->callback_context](const SettingsJson& values) {
                const auto bytes = values_bytes(values);
                if (!bytes) return false;
                CallbackScope scope(this, CallbackPhase::validation);
                try {
                    return callback(callback_context, bytes->data(),
                        static_cast<std::uint32_t>(bytes->size())) == kOk;
                } catch (...) {
                    return false;
                }
            };
            if (!validate_values(*defaults)) return kInvalid;

            esm::settings::SettingsStore store(root_);
            const esm::settings::SettingsKey key{"shared", id};
            const auto current = store.read(key);
            if (current.status == esm::settings::StoreStatus::ok) {
                if (!current.value || current.value->schema_version != descriptor->schema_version ||
                    !current.value->revision || !validate_values(current.value->values)) return kUnavailable;
            } else if (current.status == esm::settings::StoreStatus::not_found) {
                if (has_legacy) {
                    const auto old = store.read({"shared", legacy_id});
                    if (old.status == esm::settings::StoreStatus::ok) {
                        if (!old.value || !old.value->schema_version || !old.value->revision) {
                            return kUnavailable;
                        }
                        std::array<char, SHROUDWEAVE_SERVER_MAX_JSON_BYTES_V1> output{};
                        std::uint32_t output_length = 0;
                        std::uint64_t output_revision = 0;
                        const auto old_bytes = values_bytes(old.value->values);
                        if (!old_bytes) {
                            return kUnavailable;
                        }
                        CallbackScope scope(this, CallbackPhase::migration);
                        std::uint32_t migration_status = kFailed;
                        try {
                            migration_status = descriptor->migrate(descriptor->callback_context,
                                old.value->schema_version, old.value->revision,
                                old_bytes->data(), static_cast<std::uint32_t>(old_bytes->size()),
                                output.data(), static_cast<std::uint32_t>(output.size()),
                                &output_length, &output_revision);
                        } catch (...) {
                            migration_status = kFailed;
                        }
                        if (migration_status != kOk || !output_length ||
                            output_length > output.size() || !output_revision) {
                            return migration_status == kInvalid ? kInvalid : kUnavailable;
                        }
                        auto transformed = parse_settings_json(
                            std::string_view(output.data(), output_length));
                        if (!transformed || !validate_values(*transformed)) return kInvalid;
                        const auto created = store.create_if_absent(key,
                            {descriptor->schema_version, output_revision, *transformed});
                        if (!created && created.status != esm::settings::StoreStatus::conflict)
                            return kUnavailable;
                        const auto winner = store.read(key);
                        if (!winner || !winner.value ||
                            winner.value->schema_version != descriptor->schema_version ||
                            !winner.value->revision || !validate_values(winner.value->values))
                            return kUnavailable;
                    } else if (old.status != esm::settings::StoreStatus::not_found) {
                        return kUnavailable;
                    }
                }
            } else {
                return kUnavailable;
            }

            auto page = std::make_unique<Page>();
            page->owner = module;
            page->id = id;
            page->schema = descriptor->schema_version;
            page->defaults = *defaults;
            page->policy = policy;
            page->service = std::make_unique<SharedSettings>(authority_, root_, key, world_, policy,
                descriptor->schema_version, validate_values);
            if (page->service->initialize(page->defaults) != SettingsStatus::ok) return kUnavailable;
            {
                std::lock_guard lock(mutex_);
                if (active_ != module || module->state != ModuleState::starting || frozen_ || routing_ready_) {
                    return kInvalid;
                }
                std::vector<std::string> own_ids;
                own_ids.push_back(id);
                if (has_legacy) own_ids.push_back(legacy_id);
                module->owned_ids.reserve(module->owned_ids.size() + own_ids.size());
                const auto [page_it, inserted] = pages_.emplace(id, std::move(page));
                (void)page_it;
                if (!inserted) return kInvalid;
                for (auto& own_id : own_ids) module->owned_ids.push_back(std::move(own_id));
                reservation.committed = true;
            }
            return kOk;
        } catch (...) {
            return kFailed;
        }
    }

    std::uint32_t register_provider(Module* module,
        const ShroudweaveServerActionProviderV1* descriptor) noexcept {
        if (reentrant(ServiceOperation::mutation) || !module_starting(module) || !descriptor)
            return kInvalid;
        try {
            if (descriptor->struct_size < sizeof(ShroudweaveServerActionProviderV1) ||
                !descriptor->authorize || !descriptor->execute ||
                !callback_in_module(module->native_module,
                    reinterpret_cast<std::uintptr_t>(descriptor->authorize)) ||
                !callback_in_module(module->native_module,
                    reinterpret_cast<std::uintptr_t>(descriptor->execute))) return kInvalid;
            std::string id;
            if (!copy_id(descriptor->route_id, descriptor->route_id_length, id)) return kInvalid;
            EditPolicy policy;
            if (!valid_authorization(descriptor->authorization, policy)) return kInvalid;
            policy.configuration = configuration_;
            ReservationGuard reservation{this, module, &id, nullptr, false, false, false};
            {
                std::lock_guard lock(mutex_);
                if (std::this_thread::get_id() != startup_thread_ || active_ != module ||
                    module->state != ModuleState::starting || frozen_ || routing_ready_ ||
                    pages_.size() + providers_.size() >= kMaxRoutes || reserved_ids_.contains(id))
                    return kInvalid;
                const auto [reserved_it, inserted] = reserved_ids_.emplace(id, module);
                (void)reserved_it;
                if (!inserted) return kInvalid;
                reservation.first_reserved = true;
            }
            auto provider = std::make_unique<Provider>();
            provider->owner = module;
            provider->id = id;
            provider->policy = std::move(policy);
            provider->capability_mask = descriptor->capability_mask;
            provider->callback_context = descriptor->callback_context;
            provider->authorize = descriptor->authorize;
            provider->execute = descriptor->execute;
            {
                std::lock_guard lock(mutex_);
                if (active_ != module || module->state != ModuleState::starting || frozen_ || routing_ready_) {
                    return kInvalid;
                }
                module->owned_ids.reserve(module->owned_ids.size() + 1);
                const auto [provider_it, inserted] = providers_.emplace(id, std::move(provider));
                (void)provider_it;
                if (!inserted) return kInvalid;
                module->owned_ids.push_back(id);
                reservation.committed = true;
            }
            return kOk;
        } catch (...) {
            return kFailed;
        }
    }

    std::uint32_t read_page(Module* module, const char* page_id, std::uint32_t page_id_length,
        char* output, std::uint32_t capacity, std::uint32_t* output_length,
        std::uint64_t* output_revision) noexcept {
        if (reentrant(ServiceOperation::read) || !module_readable(module) || !page_id ||
            !output || !output_length || !output_revision) return kInvalid;
        try {
            std::string id;
            if (!copy_id(page_id, page_id_length, id)) return kInvalid;
            Page* page{};
            {
                std::lock_guard lock(mutex_);
                const auto it = pages_.find(id);
                if (it == pages_.end() || it->second->owner != module) return kUnavailable;
                page = it->second.get();
            }
            const auto result = page->service->read_host();
            if (result.status != SettingsStatus::ok || !result.snapshot) return kUnavailable;
            const auto bytes = values_bytes(result.snapshot->values);
            if (!bytes || bytes->size() > std::numeric_limits<std::uint32_t>::max()) return kUnavailable;
            *output_length = static_cast<std::uint32_t>(bytes->size());
            *output_revision = result.snapshot->revision;
            if (bytes->size() > capacity) return kInvalid;
            std::memcpy(output, bytes->data(), bytes->size());
            return kOk;
        } catch (...) {
            return kFailed;
        }
    }

    std::uint32_t peer_current(Module* module, std::uint64_t steam,
        std::uint64_t connection, std::uint64_t world) noexcept {
        if (reentrant(ServiceOperation::current_peer) || !module_readable(module) ||
            !steam || !connection || world != world_) return kUnavailable;
        const auto peer = current_peer(steam);
        return usable_peer(peer, steam, world_, configuration_) &&
            peer->connection == connection && peer->world == world ? kOk : kUnavailable;
    }

    std::uint32_t observe(Module* module, std::uintptr_t manager,
                          std::uintptr_t native_caller) noexcept {
        if (reentrant(ServiceOperation::observer) || !module_readable(module) ||
            !routing_ready() || !observer_) return kUnavailable;
        CallbackScope scope(this, CallbackPhase::observer);
        try { return observer_(manager, native_caller); }
        catch (...) { return kFailed; }
    }

    std::uint32_t authorize(Provider& provider, std::string_view action,
                            std::string_view values, std::uint32_t& required) noexcept {
        required = 0;
        if (action.empty() || action.size() > SHROUDWEAVE_SERVER_MAX_ID_BYTES_V1 ||
            values.empty() || values.size() > SHROUDWEAVE_SERVER_MAX_JSON_BYTES_V1 ||
            !parse_settings_json(values)) return kInvalid;
        CallbackScope scope(this, CallbackPhase::authorization);
        try {
            return provider.authorize(provider.callback_context, action.data(),
                static_cast<std::uint32_t>(action.size()), values.data(),
                static_cast<std::uint32_t>(values.size()), &required);
        } catch (...) {
            return kFailed;
        }
    }

    std::optional<SettingsJson> handle_settings(Page& page, std::uint64_t sender,
        const PeerAssociation& peer, const SettingsJson& request, const std::string& op,
        const std::string& id) {
        SettingsJson reply{{"v", 1}, {"request", request["request"]},
            {"page", id}, {"status", "unavailable"}};
        if (boot_.empty()) return reply;
        const std::string session = boot_ + ":" + std::to_string(world_) + ":" +
            std::to_string(peer.connection);
        SettingsReply result;
        if (op == "read" && request.size() == 4) {
            result = page.service->read(sender);
        } else if (op == "edit" && request.size() == 7 && request.contains("session") &&
            request["session"] == session && request.contains("revision") &&
            request["revision"].is_number_unsigned() && request.contains("values")) {
            const auto bytes = values_bytes(request["values"]);
            std::optional<SharedSettings::Pending> pending;
            if (bytes) pending = page.service->receive(sender,
                request["revision"].get<std::uint64_t>(), *bytes);
            result = pending ? page.service->execute(*pending)
                             : SettingsReply{SettingsStatus::invalid, {}};
        } else {
            result.status = SettingsStatus::denied;
        }
        reply["status"] = settings_status_name(result.status);
        if (result.snapshot) {
            reply["session"] = session;
            reply["policy"] = policy_name(page.policy.mode);
            reply["revision"] = result.snapshot->revision;
            reply["values"] = result.snapshot->values;
        }
        return reply;
    }

    std::uint32_t available_caps(Provider& provider, std::chrono::steady_clock::time_point at) const noexcept {
        if (provider.heartbeat == std::chrono::steady_clock::time_point{} || at < provider.heartbeat ||
            at - provider.heartbeat >= kCapabilityHeartbeat) return 0;
        return provider.available_capabilities & provider.capability_mask;
    }

    void reclaim_retired_receipts(const std::string& route) {
        struct ObservedReceipt final {
            ReceiptKey key;
            std::uint64_t sequence{};
        };
        std::vector<ObservedReceipt> observed_receipts;
        std::set<std::uint64_t> senders;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [key, receipt] : receipts_) {
                if (std::get<0>(key) != route) continue;
                senders.insert(std::get<1>(key));
                observed_receipts.push_back({key, receipt.sequence});
            }
        }
        std::map<std::uint64_t, std::optional<PeerAssociation>> current_sessions;
        for (const auto steam : senders) {
            auto current = current_peer(steam);
            current_sessions.emplace(steam, std::move(current));
        }
        {
            std::lock_guard lock(mutex_);
            for (const auto& observed_receipt : observed_receipts) {
                const auto receipt_it = receipts_.find(observed_receipt.key);
                if (receipt_it == receipts_.end() ||
                    receipt_it->second.sequence != observed_receipt.sequence) continue;
                const auto observed = current_sessions.find(std::get<1>(observed_receipt.key));
                if (observed == current_sessions.end()) continue;
                const auto& session = observed->second;
                const bool same_session = session &&
                    session->steam == std::get<1>(observed_receipt.key) &&
                    session->connection == std::get<2>(observed_receipt.key) &&
                    session->world == std::get<3>(observed_receipt.key) &&
                    session->configuration == configuration_;
                const bool effect_in_progress =
                    receipt_it->second.status == "executing" && receipt_it->second.effect_started;
                if (same_session || effect_in_progress) continue;
                receipts_.erase(receipt_it);
            }
        }
    }

    std::optional<SettingsJson> handle_actions(Provider& provider, std::uint64_t sender,
        const PeerAssociation& peer, const SettingsJson& request, const std::string& op,
        const std::string& id) {
        SettingsJson reply{{"v", 1}, {"request", request["request"]},
            {"page", id}, {"status", "denied"}};
        if (boot_.empty()) return reply;
        const std::string session = boot_ + ":" + std::to_string(world_) + ":" +
            std::to_string(peer.connection);
        const ReceiptKey key{id, sender, peer.connection, peer.world};
        reclaim_retired_receipts(id);
        const auto at = now();
        std::uint32_t current_capabilities{};
        {
            std::lock_guard lock(mutex_);
            if (!routing_ready_ || stopped_) {
                reply["status"] = "unavailable";
                return reply;
            }
            current_capabilities = available_caps(provider, at);
        }
        {
            std::lock_guard lock(mutex_);
            const auto receipt_it = receipts_.find(key);
            if (receipt_it != receipts_.end() && receipt_it->second.status == "queued" &&
                at >= receipt_it->second.expires) receipt_it->second.status = "expired";
        }
        if (op == "read" && request.size() == 4) {
            reply["status"] = "ok";
            reply["session"] = session;
            reply["policy"] = policy_name(provider.policy.mode);
            reply["capabilities"] = current_capabilities;
            std::lock_guard lock(mutex_);
            const auto it = receipts_.find(key);
            reply["last_sequence"] = it == receipts_.end() ? 0U : it->second.sequence;
            return reply;
        }
        if (!request.contains("session") || request["session"] != session ||
            !request.contains("sequence") || !request["sequence"].is_number_unsigned() ||
            request["sequence"] == 0) return reply;
        const auto sequence = request["sequence"].get<std::uint64_t>();
        reply["sequence"] = sequence;
        reply["session"] = session;
        if (op == "result" && request.size() == 6) {
            std::lock_guard lock(mutex_);
            const auto it = receipts_.find(key);
            reply["status"] = it != receipts_.end() && it->second.sequence == sequence
                ? it->second.status : "unavailable";
            return reply;
        }
        if (op != "act" || request.size() != 8 || !request.contains("action") ||
            !request["action"].is_string() || !request.contains("values") ||
            !request["values"].is_object()) return reply;
        const std::string action = request["action"].get<std::string>();
        if (action.empty() || action.size() > SHROUDWEAVE_SERVER_MAX_ID_BYTES_V1) {
            reply["status"] = "invalid";
            return reply;
        }
        const auto values = values_bytes(request["values"]);
        if (!values) {
            reply["status"] = "invalid";
            return reply;
        }
        const std::string command = SettingsJson{{"action", action},
            {"values", request["values"]}}.dump();
        {
            std::lock_guard lock(mutex_);
            const auto previous = receipts_.find(key);
            if (previous != receipts_.end()) {
                if (sequence < previous->second.sequence) return reply;
                if (sequence == previous->second.sequence) {
                    reply["status"] = previous->second.command == command
                        ? previous->second.status : "invalid";
                    return reply;
                }
                if (previous->second.status == "queued" || previous->second.status == "executing") {
                    reply["status"] = "busy";
                    return reply;
                }
            } else {
                std::size_t receipt_count = 0;
                for (const auto& [receipt_key, receipt] : receipts_) {
                    (void)receipt;
                    if (std::get<0>(receipt_key) == id) ++receipt_count;
                }
                if (receipt_count >= kMaxReceiptsPerRoute) {
                    reply["status"] = "busy";
                    return reply;
                }
            }
        }
        auto ticket = authority_.receive(sender, provider.policy);
        if (!ticket || ticket->world() != world_ ||
            !authority_.revalidate(*ticket, peer, provider.policy)) return reply;
        std::uint32_t required = 0;
        const auto authorized = authorize(provider, action, *values, required);
        if (authorized != kOk) {
            reply["status"] = authorized == kInvalid ? "invalid" : "unavailable";
            return reply;
        }
        if ((required & ~provider.capability_mask) != 0U) {
            reply["status"] = "unavailable";
            return reply;
        }
        if ((required & current_capabilities) != required) {
            reply["status"] = "unavailable";
            return reply;
        }
        if (!authority_.revalidate(*ticket, provider.policy)) return reply;
        {
            std::lock_guard lock(mutex_);
            if (!routing_ready_ || stopped_) {
                reply["status"] = "unavailable";
                return reply;
            }
            const auto previous = receipts_.find(key);
            if (previous != receipts_.end()) {
                if (sequence < previous->second.sequence) return reply;
                if (sequence == previous->second.sequence) {
                    reply["status"] = previous->second.command == command
                        ? previous->second.status : "invalid";
                    return reply;
                }
                if (previous->second.status == "queued" || previous->second.status == "executing") {
                    reply["status"] = "busy";
                    return reply;
                }
            } else {
                std::size_t receipt_count = 0;
                for (const auto& [receipt_key, receipt] : receipts_) {
                    (void)receipt;
                    if (std::get<0>(receipt_key) == id) ++receipt_count;
                }
                if (receipt_count >= kMaxReceiptsPerRoute) {
                    reply["status"] = "busy";
                    return reply;
                }
            }
            Receipt receipt;
            receipt.peer = peer;
            receipt.ticket = std::move(ticket);
            receipt.sequence = sequence;
            receipt.command = command;
            receipt.status = "queued";
            receipt.expires = at + kReceiptLifetime;
            receipt.required_capabilities = required;
            if (previous == receipts_.end()) receipts_.emplace(key, std::move(receipt));
            else previous->second = std::move(receipt);
        }
        reply["status"] = "queued";
        return reply;
    }

    std::uint32_t pump(Module* module, const char* route_id, std::uint32_t route_id_length,
                       std::uint32_t capabilities) noexcept {
        try {
        if (reentrant(ServiceOperation::pump)) return kInvalid;
        if (!module_readable(module)) return kInvalid;
        std::string id;
        if (!copy_id(route_id, route_id_length, id)) return kInvalid;
        Provider* provider{};
        const auto at = now();
        ReceiptKey candidate_key;
        Receipt candidate;
        bool has_candidate = false;
        {
            std::lock_guard lock(mutex_);
            if (!routing_ready_ || stopped_) return kUnavailable;
            const auto provider_it = providers_.find(id);
            if (provider_it == providers_.end() || provider_it->second->owner != module)
                return kUnavailable;
            provider = provider_it->second.get();
            if (pumping_) return kInvalid;
            provider->available_capabilities = capabilities & provider->capability_mask;
            provider->heartbeat = at;
            for (auto it = receipts_.begin(); it != receipts_.end(); ++it) {
                if (std::get<0>(it->first) != id || it->second.status != "queued") continue;
                if (at >= it->second.expires) {
                    it->second.status = "expired";
                    continue;
                }
                if ((it->second.required_capabilities & provider->available_capabilities) !=
                    it->second.required_capabilities) {
                    it->second.status = "unavailable";
                    continue;
                }
                candidate_key = it->first;
                candidate = it->second;
                it->second.status = "executing";
                pumping_ = true;
                has_candidate = true;
                break;
            }
        }
        if (!has_candidate) return kOk;

        auto finish = [this, &candidate_key, &candidate](const char* status) noexcept {
            try {
                std::lock_guard lock(mutex_);
                const auto it = receipts_.find(candidate_key);
                if (it != receipts_.end() && it->second.sequence == candidate.sequence &&
                    it->second.status == "executing") it->second.status = status;
                pumping_ = false;
            } catch (...) {}
        };
        try {
        if (!candidate.ticket || !authority_.revalidate(*candidate.ticket, provider->policy)) {
            finish("denied");
            return kOk;
        }
        auto command = parse_settings_json(candidate.command);
        if (!command || !command->contains("action") || !(*command)["action"].is_string() ||
            !command->contains("values")) {
            finish("invalid");
            return kOk;
        }
        const auto action = (*command)["action"].get<std::string>();
        const auto values = values_bytes((*command)["values"]);
        if (!values) {
            finish("invalid");
            return kOk;
        }
        std::uint32_t required = 0;
        const auto authorized = authorize(*provider, action, *values, required);
        if (authorized != kOk) {
            finish(authorized == kInvalid ? "invalid" : "unavailable");
            return kOk;
        }
        if ((required & ~provider->capability_mask) != 0U ||
            (required & (capabilities & provider->capability_mask)) != required) {
            finish("unavailable");
            return kOk;
        }
        if (!authority_.revalidate(*candidate.ticket, provider->policy)) {
            finish("denied");
            return kOk;
        }
        {
            // This lock is the stop-versus-dispatch linearization point. A
            // stop observed here prevents the callback; a stop after this
            // point allows this synchronous caller-thread callback to finish.
            std::lock_guard lock(mutex_);
            const auto it = receipts_.find(candidate_key);
            if (!routing_ready_ || stopped_ || it == receipts_.end() ||
                it->second.sequence != candidate.sequence || it->second.status != "executing") {
                if (it != receipts_.end() && it->second.sequence == candidate.sequence &&
                    it->second.status == "executing") {
                    it->second.status = "unavailable";
                    it->second.ticket.reset();
                }
                pumping_ = false;
                return kUnavailable;
            }
            it->second.effect_started = true;
        }
        std::uint32_t executed = kFailed;
        {
            CallbackScope scope(this, CallbackPhase::execution);
            try {
                executed = provider->execute(provider->callback_context, candidate.peer.steam,
                    candidate.peer.connection, candidate.peer.world, action.data(),
                    static_cast<std::uint32_t>(action.size()), values->data(),
                    static_cast<std::uint32_t>(values->size()));
            } catch (...) {
                executed = kFailed;
            }
        }
        if (executed == kOk) finish("applied");
        else if (executed == kUnavailable) finish("unavailable");
        else if (executed == kInvalid) finish("invalid");
        else finish("failed");
        return kOk;
        } catch (...) {
            finish("failed");
            return kFailed;
        }
        } catch (...) {
            return kFailed;
        }
    }
};

ServerServicesRuntime::ServerServicesRuntime(ConnectedPeerProvider& peers,
    std::filesystem::path settings_root, std::string boot, std::uint64_t host_world,
    std::uint64_t host_configuration, Clock clock, Observer observer)
    : impl_(std::make_unique<Impl>(peers, std::move(settings_root), std::move(boot),
          host_world, host_configuration, std::move(clock), std::move(observer))) {}

ServerServicesRuntime::~ServerServicesRuntime() = default;

bool ServerServicesRuntime::begin_module(std::string_view id,
    std::uintptr_t native_module) noexcept {
    return impl_ && impl_->begin_module(id, native_module);
}

const ShroudweaveServerServicesV1*
ServerServicesRuntime::services_for_current_module() const noexcept {
    return impl_ ? impl_->services_for_current_module() : nullptr;
}

bool ServerServicesRuntime::finish_module(bool start_succeeded) noexcept {
    return impl_ && impl_->finish_module(start_succeeded);
}

bool ServerServicesRuntime::freeze_registrations() noexcept {
    return impl_ && impl_->freeze_registrations();
}

bool ServerServicesRuntime::start_routing() noexcept {
    return impl_ && impl_->start_routing();
}

bool ServerServicesRuntime::routing_ready() const noexcept {
    return impl_ && impl_->routing_ready();
}

void ServerServicesRuntime::stop_routing() noexcept {
    if (impl_) impl_->stop_routing();
}

std::optional<SettingsJson> ServerServicesRuntime::handle(
    std::uint64_t transport_sender, std::string_view packet) {
    return impl_ ? impl_->handle(transport_sender, packet) : std::nullopt;
}

} // namespace shroudweave::server
