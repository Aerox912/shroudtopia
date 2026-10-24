#include "dedicated_native_v1.h"
#include "server_module_v1.h"
#include "server_modules.hpp"
#include "shared_runtime.hpp"
#include "startup_services.hpp"
#include "server_services_runtime.hpp"
#include "final-validation/final_validation_contract.hpp"
#include "connection_registry.hpp"
#include "connections.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

extern bool esm_dedicated_run_host(const EsmDedicatedNativeStartV1* args);
extern void esm_dedicated_set_host(void* host);
extern "C" uint32_t __cdecl EsmDedicatedGate_GetApiV1(
    uint32_t abi_major, uint32_t struct_size, EsmDedicatedNativeApiV1* api);

namespace {
struct FakeGate {
    HMODULE executable{};
    HMODULE proxy{};
    unsigned claims{};
    unsigned validations{};
    unsigned commits{};
    HMODULE last_owner{};
    void* last_site{};
    bool encoded_calls{};
} gate;

void* claimed_host{};
std::atomic<unsigned> connection_installs{};
std::atomic<unsigned> transport_calls{};
std::atomic<bool> transport_order_ok{};
std::atomic<bool> transport_route_ready{};
std::atomic<bool> transport_late_start_refused{};
std::atomic<bool> legacy_called{};
std::atomic<bool> legacy_require_synchronous{};
std::wstring current_case;
std::string final_validation_order;

bool same_witness(const EsmDedicatedNativeStartV1* witness) {
    return witness && witness->struct_size == sizeof(*witness) && witness->abi_major == 1 &&
        witness->proxy == gate.proxy && witness->witness[0] == 0x1234 &&
        witness->witness[1] == 0x5678 && witness->generation == 42;
}

void fill_view(const EsmDedicatedNativeStartV1* witness, EsmDedicatedNativeViewV1* view) {
    *view = {};
    view->struct_size = sizeof(*view);
    view->abi_major = 1;
    view->executable = gate.executable;
    view->call_site = nullptr;
    view->thread = GetCurrentThreadId();
    view->committed = gate.commits;
    view->generation = witness->generation;
}

uint32_t __cdecl fake_claim(const EsmDedicatedNativeStartV1* witness, void* host,
    EsmDedicatedNativeViewV1* view) {
    if (!same_witness(witness) || host != gate.proxy || !view) return 3;
    ++gate.claims;
    fill_view(witness, view);
    return 0;
}

uint32_t __cdecl fake_validate(const EsmDedicatedNativeStartV1* witness,
    EsmDedicatedNativeViewV1* view) {
    if (!same_witness(witness) || !view) return 3;
    ++gate.validations;
    fill_view(witness, view);
    return 0;
}

uint32_t __cdecl fake_commit(const EsmDedicatedNativeStartV1* witness, void* owner_module,
    void* site, const void* replacement, const void* expected, size_t size) {
    if (!same_witness(witness) || !owner_module || owner_module == gate.executable ||
        owner_module == gate.proxy || !site || !replacement || !expected || size != 5) return 3;
    const auto* next = static_cast<const unsigned char*>(replacement);
    const auto* prior = static_cast<const unsigned char*>(expected);
    gate.encoded_calls = next[0] == 0xe8 && prior[0] == 0xe8;
    if (!gate.encoded_calls) return 3;
    HMODULE actual{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(site), &actual) || actual != owner_module) return 3;
    gate.last_owner = static_cast<HMODULE>(owner_module);
    gate.last_site = site;
    ++gate.commits;
    return 0;
}

void audit(const char*) {}

void fail(const char* message, int code) {
    std::fprintf(stderr, "FAIL: %s (case=%ls)\n", message, current_case.c_str());
    std::exit(code);
}

using ModuleCountFn = uint32_t (__cdecl*)();
ModuleCountFn module_export(HMODULE module, const char* name) {
    return module ? reinterpret_cast<ModuleCountFn>(GetProcAddress(module, name)) : nullptr;
}
}

void esm_dedicated_set_host(void* host) {
    claimed_host = host;
}

extern "C" __declspec(dllexport) uint32_t __cdecl
ShroudweaveHostIntegrationLegacyCalledV1() {
    return legacy_called.load(std::memory_order_acquire) ? 1u : 0u;
}

extern "C" __declspec(dllexport) uint32_t __cdecl
ShroudweaveHostIntegrationRecordFinalOrderV1(const char* module_id) {
    if (!module_id || std::strlen(module_id) > 32) return 0;
    if (!final_validation_order.empty()) final_validation_order.push_back(',');
    final_validation_order.append(module_id);
    return 1;
}

extern "C" __declspec(dllexport) uint32_t __cdecl
ShroudweaveHostIntegrationForeignFinalV1(void*) {
    return SHROUDWEAVE_SERVER_FINAL_VALIDATION_OK_V1;
}

extern "C" uint32_t __cdecl EsmDedicatedGate_GetApiV1(
    uint32_t abi_major, uint32_t struct_size, EsmDedicatedNativeApiV1* api) {
    if (abi_major != 1 || struct_size != sizeof(EsmDedicatedNativeApiV1) || !api) return 2;
    *api = {sizeof(EsmDedicatedNativeApiV1), 1, fake_claim, fake_validate, fake_commit};
    return 0;
}

namespace shroudweave::server {
namespace {
ConnectionRegistry registry([](std::uintptr_t, std::uint16_t) -> std::optional<NativePeer> {
    return std::nullopt;
});
}

bool install_connections(HMODULE game) {
    ++connection_installs;
    return game == gate.executable;
}

void with_connections(const std::function<void(ConnectionRegistry&)>& work) {
    if (work) work(registry);
}

bool initialize_legacy_loader(const std::filesystem::path&, HMODULE, bool require_synchronous) noexcept {
    legacy_require_synchronous.store(require_synchronous, std::memory_order_release);
    legacy_called.store(true, std::memory_order_release);
    return true;
}

namespace host_test {
bool start_transport(ServerServicesRuntime& services, void (*host_audit)(const char*)) noexcept {
    (void)host_audit;
    ++transport_calls;
    transport_route_ready.store(services.routing_ready(), std::memory_order_release);
    HMODULE module = GetModuleHandleW(L"shroudweave_alpha.dll");
    const auto completed = module_export(module, "ShroudweaveHostSmokeStartCompletedV1");
    const auto status = module_export(module, "ShroudweaveHostSmokeStartStatusV1");
    const auto final_calls = module_export(module, "ShroudweaveHostSmokeFinalValidationCallsV1");
    const auto final_status = module_export(module, "ShroudweaveHostSmokeFinalValidationStatusV1");
    const bool absent_optional = current_case == L"optional-absent";
    const bool two_module_order = current_case == L"final-two-module-order";
    HMODULE beta = two_module_order ? GetModuleHandleW(L"shroudweave_beta.dll") : nullptr;
    const auto beta_completed = module_export(beta, "ShroudweaveHostSmokeStartCompletedV1");
    const auto beta_status = module_export(beta, "ShroudweaveHostSmokeStartStatusV1");
    const auto beta_final_calls = module_export(beta, "ShroudweaveHostSmokeFinalValidationCallsV1");
    const auto beta_final_status = module_export(beta, "ShroudweaveHostSmokeFinalValidationStatusV1");
    const bool final_validation_complete = absent_optional ? module == nullptr :
        final_calls && final_calls() == 1 && final_status &&
        final_status() == shroudweave::test::kFinalValidationOk &&
        (!two_module_order || (beta && beta_final_calls && beta_final_calls() == 1 &&
            beta_final_status && beta_final_status() == shroudweave::test::kFinalValidationOk &&
            final_validation_order == "beta,alpha"));
    const bool module_ready = absent_optional ? module == nullptr :
        completed && status && completed() == 1 && status() == 0 &&
        (!two_module_order || (beta && beta_completed && beta_status &&
            beta_completed() == 1 && beta_status() == 0));
    const bool late_start_refused = !services.begin_module("late_transport_probe",
        reinterpret_cast<std::uintptr_t>(gate.proxy));
    transport_late_start_refused.store(late_start_refused, std::memory_order_release);
    const bool expected_sync_policy = !absent_optional;
    const bool order_ok = module_ready && final_validation_complete && late_start_refused &&
        legacy_require_synchronous.load(std::memory_order_acquire) == expected_sync_policy &&
        services.routing_ready() && gate.validations >= 2;
    transport_order_ok.store(order_ok, std::memory_order_release);
    return order_ok;
}
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    current_case = argv[1];
    const std::filesystem::path root(argv[2]);
    gate.executable = GetModuleHandleW(nullptr);
    gate.proxy = LoadLibraryW((root / L"dbghelp.dll").c_str());
    if (!gate.executable || !gate.proxy) fail("synthetic executable/proxy setup failed", 3);
    const auto proxy_fixture = module_export(gate.proxy, "ShroudweaveHostProxyFixtureV1");
    if (!proxy_fixture || proxy_fixture() != 1)
        fail("loaded proxy was not the isolated synthetic fixture", 16);

    EsmDedicatedNativeStartV1 witness{};
    witness.struct_size = sizeof(witness);
    witness.abi_major = 1;
    witness.proxy = gate.proxy;
    witness.witness[0] = 0x1234;
    witness.witness[1] = 0x5678;
    witness.generation = 42;
    const bool started = esm_dedicated_run_host(&witness);

    const bool optional_absent = current_case == L"optional-absent";
    const bool optional_failure = current_case == L"optional-failure";
    const bool required_failure = current_case == L"required-failure";
    const bool callback_failure = current_case == L"final-callback-failure";
    const bool callback_throw = current_case == L"final-callback-throw";
    const bool duplicate_registration = current_case == L"final-duplicate-registration";
    const bool foreign_thread_registration = current_case == L"final-foreign-thread-registration";
    const bool reentrant_registration = current_case == L"final-reentrant-registration";
    const bool foreign_root_registration = current_case == L"final-foreign-root-registration";
    const bool two_module_order = current_case == L"final-two-module-order";
    const bool early_failure = current_case == L"badsha" || current_case == L"wrongfile" ||
        current_case == L"missingrequired";
    const bool expect_ready = current_case == L"success" || optional_absent || two_module_order;
    const bool expect_host_refusal = callback_failure || callback_throw || duplicate_registration ||
        foreign_thread_registration || foreign_root_registration || reentrant_registration;
    if (started != expect_ready) {
        std::fprintf(stderr,
            "OBS: started=%u expected=%u claim=%u validate=%u commit=%u connections=%u legacy=%u transport=%u\n",
            started ? 1u : 0u, expect_ready ? 1u : 0u, gate.claims, gate.validations,
            gate.commits, connection_installs.load(), legacy_called.load() ? 1u : 0u,
            transport_calls.load());
        fail("host readiness result did not match module case", 4);
    }
    if (claimed_host != gate.proxy || gate.claims != 1) fail("private gate host claim was not used", 5);
    if (current_case != L"missingrequired" && connection_installs == 0)
        fail("shared runtime did not install through synthetic connections", 6);
    const bool legacy_started = legacy_called.load(std::memory_order_acquire);
    const bool expected_legacy = expect_ready || callback_failure || callback_throw || reentrant_registration;
    if (legacy_started != expected_legacy)
        fail("legacy loader boundary did not match final-validation phase", 7);
    const bool expected_sync_legacy = (expect_ready && !optional_absent) ||
        callback_failure || callback_throw || reentrant_registration;
    if (expected_legacy &&
        legacy_require_synchronous.load(std::memory_order_acquire) != expected_sync_legacy)
        fail("legacy synchronous policy did not match registered-validator state", 20);

    const unsigned expected_transports = expect_ready ? 1u : 0u;
    if (transport_calls != expected_transports) fail("transport started before readiness or failed to start", 8);
    if (expect_ready && (!transport_order_ok || !transport_route_ready || !transport_late_start_refused))
        fail("routing started before module completion, freeze, or native finish", 9);

    HMODULE module = GetModuleHandleW(L"shroudweave_alpha.dll");
    if (optional_absent) {
        if (module || gate.commits != 0) fail("absent optional module was loaded or committed", 10);
    } else if (early_failure) {
        const auto completed = module_export(module, "ShroudweaveHostSmokeStartCompletedV1");
        if (gate.commits != 0 || (completed && completed() != 0))
            fail("invalid inventory/module was not rejected before Start", 17);
    } else {
        if (!module) fail("invoked module was not retained", 11);
        const auto completed = module_export(module, "ShroudweaveHostSmokeStartCompletedV1");
        const auto status = module_export(module, "ShroudweaveHostSmokeStartStatusV1");
        const auto retired = module_export(module, "ShroudweaveHostSmokeProbeRetiredV1");
        const auto final_calls = module_export(module, "ShroudweaveHostSmokeFinalValidationCallsV1");
        const auto final_status = module_export(module, "ShroudweaveHostSmokeFinalValidationStatusV1");
        const auto registration_probe = module_export(module, "ShroudweaveHostSmokeRegistrationProbeStatusV1");
        if (!completed || !status || !retired || !final_calls || !final_status || !registration_probe ||
            completed() != 1 || retired() != 0)
            fail("module service/startup scopes did not retire after Start", 12);
        if (expect_ready && (final_calls() != 1 ||
            final_status() != shroudweave::test::kFinalValidationOk))
            fail("final validator was not invoked exactly once on its owning module", 18);
        if (callback_failure && (started || final_calls() != 1 ||
            final_status() != shroudweave::test::kFinalValidationFailed || transport_calls != 0))
            fail("nonzero final callback did not stop startup exactly once", 21);
        if (callback_throw && (started || final_calls() != 1 ||
            final_status() != shroudweave::test::kFinalValidationFailed || transport_calls != 0))
            fail("throwing final callback escaped refusal or retried", 22);
        if (duplicate_registration && (started || final_calls() != 0 ||
            registration_probe() == shroudweave::test::kFinalValidationOk || transport_calls != 0))
            fail("ignored duplicate registration did not poison startup", 23);
        if (foreign_thread_registration && (started || final_calls() != 0 ||
            registration_probe() == shroudweave::test::kFinalValidationOk || transport_calls != 0))
            fail("ignored foreign-thread registration did not poison startup", 24);
        if (reentrant_registration && (started || final_calls() != 1 ||
            registration_probe() == shroudweave::test::kFinalValidationOk || transport_calls != 0))
            fail("late reentrant registration did not poison startup", 25);
        if (foreign_root_registration && (started || final_calls() != 0 ||
            registration_probe() != shroudweave::test::kFinalValidationInvalid ||
            legacy_started || transport_calls != 0))
            fail("callback owned by the host executable was accepted for a module", 26);
        if (two_module_order) {
            HMODULE beta = GetModuleHandleW(L"shroudweave_beta.dll");
            const auto beta_completed = module_export(beta, "ShroudweaveHostSmokeStartCompletedV1");
            const auto beta_status = module_export(beta, "ShroudweaveHostSmokeStartStatusV1");
            const auto beta_retired = module_export(beta, "ShroudweaveHostSmokeProbeRetiredV1");
            const auto beta_calls = module_export(beta, "ShroudweaveHostSmokeFinalValidationCallsV1");
            const auto beta_final_status = module_export(beta, "ShroudweaveHostSmokeFinalValidationStatusV1");
            if (!beta || !beta_completed || !beta_status || !beta_retired || !beta_calls ||
                !beta_final_status || beta_completed() != 1 || beta_status() != 0 ||
                beta_retired() != 0 || beta_calls() != 1 ||
                beta_final_status() != shroudweave::test::kFinalValidationOk ||
                final_validation_order != "beta,alpha")
                fail("multiple final validators did not run once in inventory order", 27);
        }
        if (!expect_ready && !expect_host_refusal && final_calls() != 0)
            fail("final validator ran after failed ModuleStart", 19);
        if (optional_failure && (status() != 17 || gate.commits != 1))
            fail("optional Start failure after a commit was not recorded", 13);
        if (required_failure && (status() != 18 || gate.commits != 1))
            fail("required Start failure was not recorded", 14);
        const unsigned expected_module_commits = two_module_order ? 2u : 1u;
        if (expect_ready && (status() != 0 || gate.commits != expected_module_commits ||
            gate.last_owner != module || !gate.last_site || !gate.encoded_calls))
            fail("module services or generic startup commit did not reach the private gate", 15);
    }

    shroudweave::server::stop_shared_runtime();
    std::puts("PASS: synthetic host and module startup integration");
    return 0;
}
