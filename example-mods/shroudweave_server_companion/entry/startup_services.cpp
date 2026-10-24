#include "startup_services.hpp"
#include <windows.h>
#include <atomic>

namespace shroudweave::server::startup {
namespace {
constexpr unsigned capacity = 64;
constexpr unsigned closed = 0, open = 1, failed = 2;
enum class Phase { modules, validating, validated };
struct ModuleContext {
    ShroudweaveServerStartupServicesV1 table{};
    ShroudweaveServerFinalValidationServicesV1 final_table{};
    void* module{};
    ShroudweaveServerFinalValidationCallbackV1 validator{};
    void* user_context{};
};
struct Session {
    EsmDedicatedNativeStartV1 witness{};
    EsmDedicatedNativeApiV1 api{};
    void* executable{};
    unsigned contexts{}, committed{}, validators{}, active{capacity};
    Phase phase{Phase::modules};
    bool in_call{};
};
Session session;
ModuleContext modules[capacity];
std::atomic<DWORD> startup_thread{};
std::atomic<unsigned> admission{closed};
std::atomic<bool> attempted{};

// Failure and retirement linearize on this one state. Foreign-thread calls
// cannot touch the non-atomic startup-thread-owned session or module records.
void fail() noexcept {
    unsigned expected = open;
    admission.compare_exchange_strong(expected, failed, std::memory_order_acq_rel);
}
bool current_thread() noexcept {
    return GetCurrentThreadId() == startup_thread.load(std::memory_order_acquire);
}
bool owned_callback(ShroudweaveServerFinalValidationCallbackV1 callback, void* module) noexcept {
    const auto* address = reinterpret_cast<const void*>(callback);
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !module || !VirtualQuery(address, &region, sizeof region) ||
        region.AllocationBase != module || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD protection = region.Protect & 0xff;
    if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    HMODULE owner{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(callback), &owner) && owner == module;
}
uint32_t SHROUDWEAVE_FINAL_VALIDATION_CALL register_validator(void* context,
    ShroudweaveServerFinalValidationCallbackV1 callback, void* user_context) noexcept {
    if (!current_thread()) { fail(); return SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1; }
    if (admission.load(std::memory_order_acquire) != open)
        return SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1;
    if (session.phase != Phase::modules || session.active == capacity || session.in_call ||
        context != &modules[session.active]) {
        fail(); return SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1;
    }
    auto& slot = modules[session.active];
    if (slot.validator || !owned_callback(callback, slot.module)) {
        fail(); return SHROUDWEAVE_SERVER_FINAL_VALIDATION_INVALID_V1;
    }
    slot.validator = callback;
    slot.user_context = user_context;
    ++session.validators;
    return admission.load(std::memory_order_acquire) == open
        ? SHROUDWEAVE_SERVER_FINAL_VALIDATION_OK_V1 : SHROUDWEAVE_SERVER_FINAL_VALIDATION_FAILED_V1;
}
bool valid_view(const EsmDedicatedNativeViewV1& view) noexcept {
    return view.struct_size == sizeof view && view.abi_major == 1 &&
        view.executable == session.executable && !view.call_site &&
        view.thread == GetCurrentThreadId() && view.generation == session.witness.generation &&
        view.committed == session.committed;
}
uint32_t SHROUDWEAVE_STARTUP_CALL commit(void* context, void* site,
    const void* replacement, const void* expected, size_t size) noexcept {
    if (!current_thread()) { fail(); return SHROUDWEAVE_SERVER_STARTUP_UNAVAILABLE_V1; }
    if (admission.load(std::memory_order_acquire) != open)
        return SHROUDWEAVE_SERVER_STARTUP_UNAVAILABLE_V1;
    if (session.phase != Phase::modules || session.active == capacity || session.in_call ||
        context != &modules[session.active]) {
        fail(); return SHROUDWEAVE_SERVER_STARTUP_UNAVAILABLE_V1;
    }
    if (!site || !replacement || !expected || size != 5) {
        fail(); return SHROUDWEAVE_SERVER_STARTUP_INVALID_V1;
    }
    if (session.committed == capacity) {
        fail(); return SHROUDWEAVE_SERVER_STARTUP_INVALID_V1;
    }
    session.in_call = true;
    uint32_t result = 3;
    try {
        result = session.api.commit_call(&session.witness, modules[session.active].module,
            site, replacement, expected, size);
    } catch (...) {
        // A native module must not throw across the C ABI. An unexpected bridge
        // exception may follow a write, so startup remains permanently failed.
        fail();
    }
    session.in_call = false;
    if (result == 0) {
        ++session.committed;
        if (admission.load(std::memory_order_acquire) == open)
            return SHROUDWEAVE_SERVER_STARTUP_OK_V1;
        return SHROUDWEAVE_SERVER_STARTUP_FAILED_V1;
    }
    fail();
    if (result == 2) return SHROUDWEAVE_SERVER_STARTUP_INVALID_V1;
    if (result == 6) return SHROUDWEAVE_SERVER_STARTUP_CONFLICT_V1;
    if (result == 8) return SHROUDWEAVE_SERVER_STARTUP_UNAVAILABLE_V1;
    return SHROUDWEAVE_SERVER_STARTUP_FAILED_V1;
}
}

bool begin(const EsmDedicatedNativeStartV1& witness,
    const EsmDedicatedNativeApiV1& api, void* executable) noexcept {
    // Even a foreign/reentrant begin must not read startup-thread session state.
    if (attempted.exchange(true, std::memory_order_acq_rel)) { fail(); return false; }
    if (witness.struct_size != sizeof witness || witness.abi_major != 1 ||
        !witness.proxy || !executable || witness.proxy == executable ||
        api.struct_size != sizeof api || api.abi_major != 1 || !api.validate || !api.commit_call)
        return false;
    session.witness = witness;
    session.api = api;
    session.executable = executable;
    try {
        EsmDedicatedNativeViewV1 view{};
        if (api.validate(&session.witness, &view) != 0 || !valid_view(view)) return false;
    } catch (...) { return false; }
    startup_thread.store(GetCurrentThreadId(), std::memory_order_release);
    admission.store(open, std::memory_order_release);
    return true;
}

const ShroudweaveServerStartupServicesV1* enter_module(void* module) noexcept {
    if (!current_thread()) { fail(); return nullptr; }
    if (admission.load(std::memory_order_acquire) != open) return nullptr;
    if (!module || module == session.executable || module == session.witness.proxy ||
        session.phase != Phase::modules || session.active != capacity || session.in_call ||
        session.contexts == capacity) {
        fail(); return nullptr;
    }
    for (unsigned i = 0; i < session.contexts; ++i)
        if (modules[i].module == module) { fail(); return nullptr; }
    const unsigned index = session.contexts++;
    auto& slot = modules[index];
    slot.module = module;
    slot.table = {sizeof slot.table, SHROUDWEAVE_SERVER_STARTUP_ABI_MAJOR_V1, &slot, commit};
    slot.final_table = {sizeof slot.final_table, SHROUDWEAVE_SERVER_FINAL_VALIDATION_ABI_MAJOR_V1,
        &slot, register_validator};
    session.active = index;
    return &slot.table;
}

const ShroudweaveServerFinalValidationServicesV1* final_validation_for_current_module() noexcept {
    if (!current_thread()) { fail(); return nullptr; }
    if (admission.load(std::memory_order_acquire) != open) return nullptr;
    if (session.phase != Phase::modules || session.active == capacity || session.in_call) {
        fail(); return nullptr;
    }
    return &modules[session.active].final_table;
}

bool has_final_validators() noexcept {
    if (!current_thread()) { fail(); return false; }
    // Poisoning admission must never downgrade the synchronous legacy policy.
    // Only this startup thread owns the count; foreign calls never read it.
    return session.validators != 0;
}

bool run_final_validators() noexcept {
    if (!current_thread()) { fail(); return false; }
    if (admission.load(std::memory_order_acquire) != open) return false;
    if (session.phase != Phase::modules || session.active != capacity || session.in_call) {
        fail(); return false;
    }
    session.phase = Phase::validating;
    for (unsigned i = 0; i < session.contexts; ++i) {
        // Descriptors are private copies, independent of the borrowed HostV1.
        const auto callback = modules[i].validator;
        void* const context = modules[i].user_context;
        if (!callback) continue;
        if (admission.load(std::memory_order_acquire) != open ||
            !owned_callback(callback, modules[i].module)) { fail(); return false; }
        session.in_call = true;
        uint32_t result = SHROUDWEAVE_SERVER_FINAL_VALIDATION_FAILED_V1;
        try { result = callback(context); }
        catch (...) { fail(); }
        session.in_call = false;
        if (result != SHROUDWEAVE_SERVER_FINAL_VALIDATION_OK_V1 ||
            admission.load(std::memory_order_acquire) != open) { fail(); return false; }
    }
    session.phase = Phase::validated;
    return admission.load(std::memory_order_acquire) == open;
}

bool leave_module(void* module, bool module_ready) noexcept {
    if (!current_thread()) { fail(); return false; }
    if (session.phase != Phase::modules || session.active == capacity ||
        modules[session.active].module != module || session.in_call) {
        fail(); return false;
    }
    session.active = capacity; // Contexts are never reused or reopened.
    if (!module_ready) fail();
    return admission.load(std::memory_order_acquire) == open;
}

bool finish() noexcept {
    if (!current_thread()) { fail(); return false; }
    const unsigned previous = admission.exchange(closed, std::memory_order_acq_rel);
    if (previous != open || session.active != capacity || session.in_call ||
        (session.validators && session.phase != Phase::validated)) return false;
    try {
        EsmDedicatedNativeViewV1 view{};
        return session.api.validate(&session.witness, &view) == 0 && valid_view(view);
    } catch (...) { return false; }
}

void abort() noexcept {
    if (!current_thread()) { fail(); return; }
    admission.exchange(closed, std::memory_order_acq_rel);
    session.active = capacity;
}
}
