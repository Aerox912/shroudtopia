#include "server_module_v1.h"
#include "final-validation/final_validation_contract.hpp"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string_view>
#include <string>
#include <stdexcept>

namespace {
const ShroudweaveServerServicesV1* saved_services{};
const ShroudweaveServerStartupServicesV1* saved_startup{};
void* saved_services_context{};
void* saved_startup_context{};
volatile LONG start_completed{};
volatile LONG start_status{-1};
volatile LONG final_validation_calls{};
volatile LONG final_validation_status{-1};
volatile LONG registration_probe_status{-1};
DWORD start_thread{};
HMODULE start_module{};
void* saved_registration_context{};
shroudweave::test::RegisterFinalValidationFn saved_register_validator{};
std::string active_module_id;

uint32_t SHROUDWEAVE_CALL validate_page(void*, const char*, uint32_t) {
    return SHROUDWEAVE_SERVER_STATUS_OK_V1;
}

bool mode_is(const wchar_t* expected) {
    wchar_t value[64]{};
    const DWORD length = GetEnvironmentVariableW(L"SHROUDWEAVE_HOST_TEST_CASE", value,
        static_cast<DWORD>(sizeof(value) / sizeof(value[0])));
    return length != 0 && length < sizeof(value) / sizeof(value[0]) &&
        std::wcscmp(value, expected) == 0;
}

std::uint32_t __cdecl validate_final(void* context) {
    InterlockedIncrement(&final_validation_calls);
    if (context != nullptr || GetCurrentThreadId() != start_thread || !start_module) {
        InterlockedExchange(&final_validation_status,
            static_cast<LONG>(shroudweave::test::kFinalValidationFailed));
        return shroudweave::test::kFinalValidationFailed;
    }
    HMODULE callback_owner{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&validate_final), &callback_owner) ||
        callback_owner != start_module) {
        InterlockedExchange(&final_validation_status,
            static_cast<LONG>(shroudweave::test::kFinalValidationFailed));
        return shroudweave::test::kFinalValidationFailed;
    }
    using LegacyCalledFn = std::uint32_t (__cdecl*)();
    const auto legacy_called = reinterpret_cast<LegacyCalledFn>(GetProcAddress(
        GetModuleHandleW(nullptr), "ShroudweaveHostIntegrationLegacyCalledV1"));
    if (!legacy_called || legacy_called() != 1) {
        InterlockedExchange(&final_validation_status,
            static_cast<LONG>(shroudweave::test::kFinalValidationFailed));
        return shroudweave::test::kFinalValidationFailed;
    }
    if (mode_is(L"final-two-module-order")) {
        using RecordOrderFn = std::uint32_t (__cdecl*)(const char*);
        const auto record_order = reinterpret_cast<RecordOrderFn>(GetProcAddress(
            GetModuleHandleW(nullptr), "ShroudweaveHostIntegrationRecordFinalOrderV1"));
        if (!record_order || record_order(active_module_id.c_str()) != 1) {
            InterlockedExchange(&final_validation_status,
                static_cast<LONG>(shroudweave::test::kFinalValidationFailed));
            return shroudweave::test::kFinalValidationFailed;
        }
    }
    if (mode_is(L"final-callback-throw")) {
        InterlockedExchange(&final_validation_status,
            static_cast<LONG>(shroudweave::test::kFinalValidationFailed));
        throw std::runtime_error("synthetic final-validator exception");
    }
    if (mode_is(L"final-reentrant-registration")) {
        const auto result = saved_register_validator(
            saved_registration_context, &validate_final, nullptr);
        InterlockedExchange(&registration_probe_status, static_cast<LONG>(result));
    }
    const std::uint32_t result = mode_is(L"final-callback-failure")
        ? shroudweave::test::kFinalValidationFailed : shroudweave::test::kFinalValidationOk;
    InterlockedExchange(&final_validation_status, static_cast<LONG>(result));
    return result;
}
}

namespace {
struct RegisterProbe {
    shroudweave::test::RegisterFinalValidationFn register_validator{};
    void* registration_context{};
    shroudweave::test::FinalValidationCallback callback{};
    std::uint32_t status{UINT32_MAX};
};
DWORD WINAPI register_from_foreign_thread(void* raw) {
    auto* probe = static_cast<RegisterProbe*>(raw);
    probe->status = probe->register_validator(
        probe->registration_context, probe->callback, nullptr);
    return 0;
}
}

extern "C" __declspec(dllexport) void __cdecl ShroudweaveHostSmokeCallSiteV1() {}

extern "C" __declspec(dllexport) uint32_t __cdecl ShroudweaveServerModuleStartV1(
    const ShroudweaveServerHostV1* host) {
    const bool order_case = mode_is(L"final-two-module-order");
    const bool valid_module_id = host && host->module_id &&
        (order_case ? (std::strcmp(host->module_id, "alpha") == 0 ||
            std::strcmp(host->module_id, "beta") == 0) :
            std::strcmp(host->module_id, "alpha") == 0);
    if (!host || host->struct_size < SHROUDWEAVE_SERVER_HOST_V1_STARTUP_SERVICES_SIZE ||
        host->abi_major != 1 || host->executable != GetModuleHandleW(nullptr) ||
        !host->companion || !host->module || !valid_module_id ||
        !host->root_directory || !host->audit ||
        !host->services || host->services->abi_major != SHROUDWEAVE_SERVER_SERVICES_ABI_MAJOR_V1 ||
        host->services->struct_size < SHROUDWEAVE_SERVER_SERVICES_V1_REQUIRED_SIZE ||
        !host->services->services_context || !host->services->register_settings_page ||
        !host->services->read_settings_page || !host->startup_services ||
        host->startup_services->abi_major != SHROUDWEAVE_SERVER_STARTUP_ABI_MAJOR_V1 ||
        host->startup_services->struct_size < sizeof(ShroudweaveServerStartupServicesV1) ||
        !host->startup_services->startup_context || !host->startup_services->commit_direct_call) {
        InterlockedExchange(&start_status, 100);
        InterlockedExchange(&start_completed, 1);
        return 100;
    }

    start_thread = GetCurrentThreadId();
    start_module = host->module;
    active_module_id = host->module_id;
    saved_services = host->services;
    saved_services_context = host->services->services_context;
    saved_startup = host->startup_services;
    saved_startup_context = host->startup_services->startup_context;

    ShroudweaveServerSettingsPageV1 page{};
    page.struct_size = sizeof(page);
    page.schema_version = 1;
    page.authorization.struct_size = sizeof(page.authorization);
    page.authorization.mode = SHROUDWEAVE_SERVER_AUTH_OPEN_CONNECTED_V1;
    page.authorization.revision = 1;
    const std::string page_id = "host_smoke_settings_" + active_module_id;
    page.page_id = page_id.c_str();
    page.page_id_length = static_cast<std::uint32_t>(page_id.size());
    page.defaults_json = "{\"enabled\":true}";
    page.defaults_json_length = 16;
    page.validate = validate_page;
    if (host->services->register_settings_page(host->services->services_context, &page) !=
        SHROUDWEAVE_SERVER_STATUS_OK_V1) {
        InterlockedExchange(&start_status, 101);
        InterlockedExchange(&start_completed, 1);
        return 101;
    }

    char values[128]{};
    uint32_t values_length{};
    uint64_t revision{};
    if (host->services->read_settings_page(host->services->services_context,
            page.page_id, page.page_id_length, values, sizeof(values), &values_length, &revision) !=
            SHROUDWEAVE_SERVER_STATUS_OK_V1 || revision == 0 ||
        std::string_view(values, values_length) != "{\"enabled\":true}") {
        InterlockedExchange(&start_status, 102);
        InterlockedExchange(&start_completed, 1);
        return 102;
    }

    static constexpr unsigned char expected[5]{0xe8, 0, 0, 0, 0};
    static constexpr unsigned char replacement[5]{0xe8, 1, 0, 0, 0};
    if (host->startup_services->commit_direct_call(host->startup_services->startup_context,
            reinterpret_cast<void*>(&ShroudweaveHostSmokeCallSiteV1), replacement, expected,
            sizeof(expected)) != SHROUDWEAVE_SERVER_STARTUP_OK_V1) {
        InterlockedExchange(&start_status, 103);
        InterlockedExchange(&start_completed, 1);
        return 103;
    }

    const auto* final_services =
        shroudweave::test::final_validation_services(host);
    if (!final_services) {
        InterlockedExchange(&start_status,
            static_cast<LONG>(shroudweave::test::kFinalValidationUnavailable));
        InterlockedExchange(&start_completed, 1);
        return shroudweave::test::kFinalValidationUnavailable;
    }
    if (final_services->struct_size < sizeof(*final_services) ||
        final_services->abi_major != shroudweave::test::kFinalValidationAbiMajor ||
        !final_services->register_validator) {
        InterlockedExchange(&start_status,
            static_cast<LONG>(shroudweave::test::kFinalValidationInvalid));
        InterlockedExchange(&start_completed, 1);
        return shroudweave::test::kFinalValidationInvalid;
    }
    saved_registration_context = final_services->registration_context;
    saved_register_validator = final_services->register_validator;

    if (mode_is(L"final-foreign-root-registration")) {
        using ForeignCallback = shroudweave::test::FinalValidationCallback;
        const auto foreign_callback = reinterpret_cast<ForeignCallback>(GetProcAddress(
            GetModuleHandleW(nullptr), "ShroudweaveHostIntegrationForeignFinalV1"));
        const auto result = foreign_callback ? final_services->register_validator(
            final_services->registration_context, foreign_callback, nullptr) : UINT32_MAX;
        InterlockedExchange(&registration_probe_status, static_cast<LONG>(result));
        InterlockedExchange(&start_status, 0);
        InterlockedExchange(&start_completed, 1);
        return 0;
    }

    const std::uint32_t registration = final_services->register_validator(
        final_services->registration_context, &validate_final, nullptr);
    if (registration != shroudweave::test::kFinalValidationOk) {
        InterlockedExchange(&start_status, static_cast<LONG>(registration));
        InterlockedExchange(&start_completed, 1);
        return registration;
    }

    if (mode_is(L"final-duplicate-registration")) {
        const auto duplicate = final_services->register_validator(
            final_services->registration_context, &validate_final, nullptr);
        InterlockedExchange(&registration_probe_status, static_cast<LONG>(duplicate));
    } else if (mode_is(L"final-foreign-thread-registration")) {
        RegisterProbe probe{final_services->register_validator,
            final_services->registration_context, &validate_final, UINT32_MAX};
        const HANDLE thread = CreateThread(nullptr, 0, register_from_foreign_thread, &probe, 0, nullptr);
        if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
            if (thread) CloseHandle(thread);
            InterlockedExchange(&start_status, 107);
            InterlockedExchange(&start_completed, 1);
            return 107;
        }
        CloseHandle(thread);
        InterlockedExchange(&registration_probe_status, static_cast<LONG>(probe.status));
    }

    const bool optional_failure = mode_is(L"optional-failure");
    const bool required_failure = mode_is(L"required-failure");
    const uint32_t result = optional_failure ? 17u : (required_failure ? 18u : 0u);
    InterlockedExchange(&start_status, static_cast<LONG>(result));
    InterlockedExchange(&start_completed, 1);
    return result;
}

extern "C" __declspec(dllexport) uint32_t __cdecl ShroudweaveHostSmokeStartCompletedV1() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&start_completed, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t __cdecl ShroudweaveHostSmokeStartStatusV1() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&start_status, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t __cdecl ShroudweaveHostSmokeProbeRetiredV1() {
    if (!saved_services || !saved_startup || !saved_services_context || !saved_startup_context)
        return 1;
    ShroudweaveServerSettingsPageV1 page{};
    page.struct_size = sizeof(page);
    page.schema_version = 1;
    page.authorization.struct_size = sizeof(page.authorization);
    page.authorization.mode = SHROUDWEAVE_SERVER_AUTH_OPEN_CONNECTED_V1;
    page.authorization.revision = 1;
    page.page_id = "after_start_probe";
    page.page_id_length = 17;
    page.defaults_json = "{}";
    page.defaults_json_length = 2;
    page.validate = validate_page;
    const uint32_t service_result = saved_services->register_settings_page(
        saved_services_context, &page);
    static constexpr unsigned char expected[5]{0xe8, 0, 0, 0, 0};
    static constexpr unsigned char replacement[5]{0xe8, 1, 0, 0, 0};
    const uint32_t startup_result = saved_startup->commit_direct_call(saved_startup_context,
        reinterpret_cast<void*>(&ShroudweaveHostSmokeCallSiteV1), replacement, expected,
        sizeof(expected));
    return service_result != SHROUDWEAVE_SERVER_STATUS_OK_V1 &&
        startup_result != SHROUDWEAVE_SERVER_STARTUP_OK_V1 ? 0u : 2u;
}

extern "C" __declspec(dllexport) uint32_t __cdecl
ShroudweaveHostSmokeFinalValidationCallsV1() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&final_validation_calls, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t __cdecl
ShroudweaveHostSmokeFinalValidationStatusV1() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&final_validation_status, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t __cdecl
ShroudweaveHostSmokeRegistrationProbeStatusV1() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&registration_probe_status, 0, 0));
}
