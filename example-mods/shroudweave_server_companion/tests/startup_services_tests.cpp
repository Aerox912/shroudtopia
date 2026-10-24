#include "startup_services.hpp"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t kOk = SHROUDWEAVE_SERVER_STARTUP_OK_V1;
constexpr uint32_t kUnavailable = SHROUDWEAVE_SERVER_STARTUP_UNAVAILABLE_V1;
constexpr uint32_t kInvalid = SHROUDWEAVE_SERVER_STARTUP_INVALID_V1;
constexpr uint32_t kFailed = SHROUDWEAVE_SERVER_STARTUP_FAILED_V1;

struct Fixture {
    EsmDedicatedNativeStartV1 witness{};
    EsmDedicatedNativeApiV1 api{};
    void* executable{};
    void* active_module{};
    const ShroudweaveServerStartupServicesV1* active_services{};
    uint32_t validate_result{};
    uint32_t commit_result{};
    uint32_t commit_calls{};
    uint32_t host_committed{};
    void* committed_owner{};
    void* committed_site{};
    const void* committed_replacement{};
    const void* committed_expected{};
    size_t committed_size{};
    bool argument_mismatch{};
    bool reenter{};
    uint32_t reentrant_result{UINT32_MAX};
    bool claimed{};
    bool invalid_view{};
} g;

bool same_witness(const EsmDedicatedNativeStartV1* value) {
    return value && value->struct_size == sizeof(g.witness) &&
        value->abi_major == 1 && value->proxy == g.witness.proxy &&
        value->witness[0] == g.witness.witness[0] &&
        value->witness[1] == g.witness.witness[1] &&
        value->generation == g.witness.generation;
}

void fill_view(EsmDedicatedNativeViewV1* out) {
    *out = {sizeof(*out), 1, g.executable, g.invalid_view ? reinterpret_cast<void*>(1) : nullptr,
        GetCurrentThreadId(), g.host_committed, g.witness.generation};
}

uint32_t __cdecl fake_claim(const EsmDedicatedNativeStartV1* witness, void* host,
    EsmDedicatedNativeViewV1* out) {
    if (!out || !same_witness(witness) || host != g.witness.proxy) return 2;
    g.claimed = true;
    fill_view(out);
    return 0;
}

uint32_t __cdecl fake_validate(const EsmDedicatedNativeStartV1* witness,
    EsmDedicatedNativeViewV1* out) {
    if (!out || !g.claimed || !same_witness(witness)) return 8;
    if (g.validate_result != 0) return g.validate_result;
    fill_view(out);
    return 0;
}

uint32_t __cdecl fake_commit_call(const EsmDedicatedNativeStartV1* witness,
    void* owner, void* site, const void* replacement, const void* expected, size_t size) {
    ++g.commit_calls;
    g.committed_owner = owner;
    g.committed_site = site;
    g.committed_replacement = replacement;
    g.committed_expected = expected;
    g.committed_size = size;
    g.argument_mismatch = !same_witness(witness) || owner != g.active_module ||
        !site || !replacement || !expected || size != 5;
    if (g.reenter && g.active_services) {
        static const unsigned char call[5] = {0xe8, 1, 0, 0, 0};
        g.reentrant_result = g.active_services->commit_direct_call(
            g.active_services->startup_context, site, call, call, sizeof call);
    }
    if (g.commit_result == 0) ++g.host_committed;
    return g.commit_result;
}

bool require(bool condition, const char* message, int line) {
    if (condition) return true;
    std::fprintf(stderr, "startup_services_tests.cpp:%d: %s\n", line, message);
    return false;
}
#define REQUIRE(condition) do { if (!require((condition), #condition, __LINE__)) return false; } while (0)

bool begin(bool validation_succeeds = true) {
    g = {};
    g.executable = GetModuleHandleW(nullptr);
    g.witness = {sizeof(g.witness), 1,
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234)),
        {0x77553311ULL, 0x2468ace0ULL}, 7};
    g.api = {sizeof(g.api), 1, fake_claim, fake_validate, fake_commit_call};
    // The fake private host has already issued its claim before coordinator begin.
    g.claimed = validation_succeeds;
    g.validate_result = validation_succeeds ? 0u : 8u;
    return shroudweave::server::startup::begin(g.witness, g.api, g.executable);
}

const unsigned char kExpected[5] = {0xe8, 0x11, 0x22, 0x33, 0x44};
const unsigned char kReplacement[5] = {0xe8, 0x55, 0x66, 0x77, 0x00};

bool successful_start() {
    REQUIRE(begin());
    static int owner;
    static int site;
    g.active_module = &owner;
    const auto* services = shroudweave::server::startup::enter_module(&owner);
    REQUIRE(services != nullptr);
    REQUIRE(services->struct_size >= sizeof(*services));
    REQUIRE(services->abi_major == 1);
    g.active_services = services;
    const auto status = services->commit_direct_call(services->startup_context,
        &site, kReplacement, kExpected, sizeof kExpected);
    REQUIRE(status == kOk);
    REQUIRE(g.commit_calls == 1);
    REQUIRE(!g.argument_mismatch);
    REQUIRE(g.committed_owner == &owner);
    REQUIRE(g.committed_site == &site);
    REQUIRE(g.committed_replacement == kReplacement);
    REQUIRE(g.committed_expected == kExpected);
    REQUIRE(g.committed_size == 5);
    REQUIRE(shroudweave::server::startup::leave_module(&owner, true));
    REQUIRE(shroudweave::server::startup::finish());
    return true;
}

bool empty_startup_is_ready() {
    REQUIRE(begin());
    REQUIRE(shroudweave::server::startup::finish());
    REQUIRE(g.commit_calls == 0);
    return true;
}

bool validation_failure_blocks_start() {
    REQUIRE(!begin(false));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool malformed_private_view_blocks_start() {
    g = {};
    g.executable = GetModuleHandleW(nullptr);
    g.witness = {sizeof(g.witness), 1,
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234)),
        {0x77553311ULL, 0x2468ace0ULL}, 7};
    g.api = {sizeof(g.api), 1, fake_claim, fake_validate, fake_commit_call};
    g.claimed = true;
    g.invalid_view = true;
    REQUIRE(!shroudweave::server::startup::begin(g.witness, g.api, g.executable));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool failed_module_poisoned_finish() {
    REQUIRE(begin());
    static int owner;
    g.active_module = &owner;
    REQUIRE(shroudweave::server::startup::enter_module(&owner) != nullptr);
    REQUIRE(!shroudweave::server::startup::leave_module(&owner, false));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool native_service_failure_poisoned_finish() {
    REQUIRE(begin());
    static int owner;
    static int site;
    g.active_module = &owner;
    const auto* services = shroudweave::server::startup::enter_module(&owner);
    REQUIRE(services != nullptr);
    g.active_services = services;
    g.commit_result = 6;
    const auto status = services->commit_direct_call(services->startup_context,
        &site, kReplacement, kExpected, sizeof kExpected);
    REQUIRE(status != kOk);
    REQUIRE(g.commit_calls == 1);
    REQUIRE(!shroudweave::server::startup::leave_module(&owner, true));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool retired_context_refuses_late_call() {
    REQUIRE(begin());
    static int owner;
    static int site;
    g.active_module = &owner;
    const auto* services = shroudweave::server::startup::enter_module(&owner);
    REQUIRE(services != nullptr);
    REQUIRE(shroudweave::server::startup::leave_module(&owner, true));
    const auto status = services->commit_direct_call(services->startup_context,
        &site, kReplacement, kExpected, sizeof kExpected);
    REQUIRE(status == kUnavailable);
    REQUIRE(g.commit_calls == 0);
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

struct ForeignCall {
    const ShroudweaveServerStartupServicesV1* services{};
    void* site{};
    uint32_t status{UINT32_MAX};
};

DWORD WINAPI invoke_foreign(void* raw) {
    auto* call = static_cast<ForeignCall*>(raw);
    call->status = call->services->commit_direct_call(call->services->startup_context,
        call->site, kReplacement, kExpected, sizeof kExpected);
    return 0;
}

bool foreign_thread_refuses_call() {
    REQUIRE(begin());
    static int owner;
    static int site;
    g.active_module = &owner;
    const auto* services = shroudweave::server::startup::enter_module(&owner);
    REQUIRE(services != nullptr);
    ForeignCall call{services, &site, UINT32_MAX};
    const HANDLE thread = CreateThread(nullptr, 0, invoke_foreign, &call, 0, nullptr);
    REQUIRE(thread != nullptr);
    REQUIRE(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    REQUIRE(call.status == kUnavailable);
    REQUIRE(g.commit_calls == 0);
    REQUIRE(!shroudweave::server::startup::leave_module(&owner, true));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

struct ForeignFinalRegistration {
    const ShroudweaveServerFinalValidationServicesV1* services{};
    ShroudweaveServerFinalValidationCallbackV1 callback{};
    uint32_t status{UINT32_MAX};
};

DWORD WINAPI register_final_from_foreign_thread(void* raw) {
    auto* call = static_cast<ForeignFinalRegistration*>(raw);
    call->status = call->services->register_validator(
        call->services->registration_context, call->callback, nullptr);
    return 0;
}

bool registered_count_survives_foreign_poison(const char* owner_path) {
    REQUIRE(owner_path != nullptr);
    REQUIRE(begin());
    HMODULE owner = LoadLibraryA(owner_path);
    REQUIRE(owner != nullptr);
    auto callback = reinterpret_cast<ShroudweaveServerFinalValidationCallbackV1>(
        GetProcAddress(owner, "EsmStartupOwnerFinalValidator"));
    auto count = reinterpret_cast<LONG (__cdecl*)()>(
        GetProcAddress(owner, "EsmStartupOwnerCount"));
    REQUIRE(callback != nullptr);
    REQUIRE(count != nullptr);
    REQUIRE(count() == 0);
    REQUIRE(shroudweave::server::startup::enter_module(owner) != nullptr);
    const auto* services = shroudweave::server::startup::final_validation_for_current_module();
    REQUIRE(services != nullptr);
    REQUIRE(services->register_validator(services->registration_context, callback, nullptr) ==
        SHROUDWEAVE_SERVER_FINAL_VALIDATION_OK_V1);
    REQUIRE(shroudweave::server::startup::leave_module(owner, true));

    ForeignFinalRegistration call{services, callback, UINT32_MAX};
    const HANDLE thread = CreateThread(nullptr, 0, register_final_from_foreign_thread, &call, 0, nullptr);
    REQUIRE(thread != nullptr);
    REQUIRE(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    REQUIRE(call.status == SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1);
    REQUIRE(shroudweave::server::startup::has_final_validators());
    REQUIRE(!shroudweave::server::startup::run_final_validators());
    REQUIRE(count() == 0);
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool owner_thread_stale_final_registration_poison(const char* owner_path) {
    REQUIRE(owner_path != nullptr);
    REQUIRE(begin());
    HMODULE owner = LoadLibraryA(owner_path);
    REQUIRE(owner != nullptr);
    auto callback = reinterpret_cast<ShroudweaveServerFinalValidationCallbackV1>(
        GetProcAddress(owner, "EsmStartupOwnerFinalValidator"));
    auto count = reinterpret_cast<LONG (__cdecl*)()>(
        GetProcAddress(owner, "EsmStartupOwnerCount"));
    REQUIRE(callback != nullptr);
    REQUIRE(count != nullptr);
    REQUIRE(count() == 0);
    REQUIRE(shroudweave::server::startup::enter_module(owner) != nullptr);
    const auto* services = shroudweave::server::startup::final_validation_for_current_module();
    REQUIRE(services != nullptr);
    REQUIRE(services->register_validator(services->registration_context, callback, nullptr) ==
        SHROUDWEAVE_SERVER_FINAL_VALIDATION_OK_V1);
    REQUIRE(shroudweave::server::startup::leave_module(owner, true));
    REQUIRE(services->register_validator(services->registration_context, callback, nullptr) ==
        SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1);
    REQUIRE(shroudweave::server::startup::has_final_validators());
    REQUIRE(!shroudweave::server::startup::run_final_validators());
    REQUIRE(count() == 0);
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool reentrant_call_refuses_and_poisoned_finish() {
    REQUIRE(begin());
    static int owner;
    static int site;
    g.active_module = &owner;
    const auto* services = shroudweave::server::startup::enter_module(&owner);
    REQUIRE(services != nullptr);
    g.active_services = services;
    g.reenter = true;
    const auto outer = services->commit_direct_call(services->startup_context,
        &site, kReplacement, kExpected, sizeof kExpected);
    REQUIRE(outer == kFailed);
    REQUIRE(g.commit_calls == 1);
    REQUIRE(g.reentrant_result == kUnavailable);
    REQUIRE(!shroudweave::server::startup::leave_module(&owner, true));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool context_capacity_is_not_reused() {
    REQUIRE(begin());
    static int modules[65];
    for (unsigned i = 0; i < 64; ++i) {
        g.active_module = &modules[i];
        REQUIRE(shroudweave::server::startup::enter_module(&modules[i]) != nullptr);
        REQUIRE(shroudweave::server::startup::leave_module(&modules[i], true));
    }
    g.active_module = &modules[64];
    REQUIRE(shroudweave::server::startup::enter_module(&modules[64]) == nullptr);
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool invalid_commit_size_refuses() {
    REQUIRE(begin());
    static int owner;
    static int site;
    g.active_module = &owner;
    const auto* services = shroudweave::server::startup::enter_module(&owner);
    REQUIRE(services != nullptr);
    const auto status = services->commit_direct_call(services->startup_context,
        &site, kReplacement, kExpected, 4);
    REQUIRE(status == kInvalid);
    REQUIRE(g.commit_calls == 0);
    REQUIRE(!shroudweave::server::startup::leave_module(&owner, true));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}

bool commitment_capacity_is_not_exceeded() {
    REQUIRE(begin());
    static int owner;
    static int site;
    g.active_module = &owner;
    const auto* services = shroudweave::server::startup::enter_module(&owner);
    REQUIRE(services != nullptr);
    for (unsigned i = 0; i != 64; ++i) {
        REQUIRE(services->commit_direct_call(services->startup_context,
            &site, kReplacement, kExpected, sizeof kExpected) == kOk);
    }
    REQUIRE(g.commit_calls == 64);
    REQUIRE(services->commit_direct_call(services->startup_context,
        &site, kReplacement, kExpected, sizeof kExpected) == kInvalid);
    REQUIRE(g.commit_calls == 64);
    REQUIRE(!shroudweave::server::startup::leave_module(&owner, true));
    REQUIRE(!shroudweave::server::startup::finish());
    return true;
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    const char* name = argv[1];
    bool passed = false;
    if (std::strcmp(name, "success") == 0) passed = successful_start();
    else if (std::strcmp(name, "empty-startup") == 0) passed = empty_startup_is_ready();
    else if (std::strcmp(name, "validation-failure") == 0) passed = validation_failure_blocks_start();
    else if (std::strcmp(name, "invalid-view") == 0) passed = malformed_private_view_blocks_start();
    else if (std::strcmp(name, "failed-module") == 0) passed = failed_module_poisoned_finish();
    else if (std::strcmp(name, "service-failure") == 0) passed = native_service_failure_poisoned_finish();
    else if (std::strcmp(name, "retired-context") == 0) passed = retired_context_refuses_late_call();
    else if (std::strcmp(name, "foreign-thread") == 0) passed = foreign_thread_refuses_call();
    else if (std::strcmp(name, "reentrant") == 0) passed = reentrant_call_refuses_and_poisoned_finish();
    else if (std::strcmp(name, "final-registered-count-poison") == 0 && argc == 3)
        passed = registered_count_survives_foreign_poison(argv[2]);
    else if (std::strcmp(name, "final-owner-stale-registration") == 0 && argc == 3)
        passed = owner_thread_stale_final_registration_poison(argv[2]);
    else if (std::strcmp(name, "context-capacity") == 0) passed = context_capacity_is_not_reused();
    else if (std::strcmp(name, "invalid-size") == 0) passed = invalid_commit_size_refuses();
    else if (std::strcmp(name, "commit-capacity") == 0) passed = commitment_capacity_is_not_exceeded();
    else return 2;
    if (!passed) return 1;
    std::printf("PASS: startup service case %s\n", name);
    return 0;
}
