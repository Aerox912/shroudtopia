#pragma once
#include "server_module_v1.h"
#include <cstddef>
#include <cstdint>

#if __has_include("server_final_validation_v1.h")
#include "server_final_validation_v1.h"
#endif

namespace shroudweave::test {
#if __has_include("server_final_validation_v1.h")
using FinalValidationCallback = ShroudweaveServerFinalValidationCallbackV1;
using RegisterFinalValidationFn = ShroudweaveServerRegisterFinalValidationFnV1;
using FinalValidationServices = ShroudweaveServerFinalValidationServicesV1;
constexpr std::uint32_t kFinalValidationAbiMajor =
    SHROUDWEAVE_SERVER_FINAL_VALIDATION_ABI_MAJOR_V1;
constexpr std::uint32_t kFinalValidationOk =
    SHROUDWEAVE_SERVER_FINAL_VALIDATION_OK_V1;
constexpr std::uint32_t kFinalValidationUnavailable =
    SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1;
constexpr std::uint32_t kFinalValidationInvalid =
    SHROUDWEAVE_SERVER_FINAL_VALIDATION_INVALID_V1;
constexpr std::uint32_t kFinalValidationFailed =
    SHROUDWEAVE_SERVER_FINAL_VALIDATION_FAILED_V1;
#else
using FinalValidationCallback = std::uint32_t (__cdecl *)(void*);
using RegisterFinalValidationFn = std::uint32_t (__cdecl *)(
    void*, FinalValidationCallback, void*);
struct FinalValidationServices {
    std::uint32_t struct_size;
    std::uint32_t abi_major;
    void* registration_context;
    RegisterFinalValidationFn register_validator;
};
constexpr std::uint32_t kFinalValidationAbiMajor = 1;
constexpr std::uint32_t kFinalValidationOk = 0;
constexpr std::uint32_t kFinalValidationUnavailable = 1;
constexpr std::uint32_t kFinalValidationInvalid = 2;
constexpr std::uint32_t kFinalValidationFailed = 3;
#endif

static_assert(offsetof(ShroudweaveServerHostV1, services) == 56,
    "HostV1 services offset changed");
static_assert(offsetof(ShroudweaveServerHostV1, startup_services) == 64,
    "HostV1 startup-services offset changed");
static_assert(sizeof(ShroudweaveServerStartupServicesV1) == 24,
    "the old startup table layout changed");
static_assert(offsetof(FinalValidationServices, struct_size) == 0);
static_assert(offsetof(FinalValidationServices, abi_major) == 4);
static_assert(offsetof(FinalValidationServices, registration_context) == 8);
static_assert(offsetof(FinalValidationServices, register_validator) == 16);
static_assert(sizeof(FinalValidationServices) == 24);

#if defined(SHROUDWEAVE_SERVER_HOST_V1_FINAL_VALIDATION_SERVICES_SIZE)
static_assert(offsetof(ShroudweaveServerHostV1, final_validation_services) == 72,
    "new HostV1 field must be appended after the old prefix");
static_assert(sizeof(ShroudweaveServerHostV1) == 80,
    "new HostV1 size must include only the appended service pointer");
constexpr std::uint32_t kFinalValidationHostSize =
    SHROUDWEAVE_SERVER_HOST_V1_FINAL_VALIDATION_SERVICES_SIZE;
inline const FinalValidationServices* final_validation_services(
    const ShroudweaveServerHostV1* host) noexcept {
    return host && host->struct_size >= kFinalValidationHostSize
        ? host->final_validation_services : nullptr;
}
#else
struct FutureHostV1 {
    ShroudweaveServerHostV1 prefix;
    const FinalValidationServices* final_validation_services;
};
constexpr std::uint32_t kFinalValidationHostSize =
    static_cast<std::uint32_t>(offsetof(FutureHostV1, final_validation_services) +
        sizeof(FutureHostV1::final_validation_services));
static_assert(offsetof(FutureHostV1, final_validation_services) == 72);
static_assert(kFinalValidationHostSize == 80);
inline const FinalValidationServices* final_validation_services(
    const ShroudweaveServerHostV1* host) noexcept {
    if (!host || host->struct_size < kFinalValidationHostSize) return nullptr;
    return reinterpret_cast<const FutureHostV1*>(host)->final_validation_services;
}
#endif
}
