#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include "server_services_v1.h"
#include "server_startup_v1.h"
#include "server_final_validation_v1.h"

// The caller owns these pointers during ShroudweaveServerModuleStartV1 only.
// A module must copy any ID, path or context it needs after startup. The host
// prefix through audit is ABI V1 and remains unchanged; services is appended.
typedef struct ShroudweaveServerHostV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    HMODULE executable;
    HMODULE companion;
    HMODULE module;
    const char* module_id;
    const wchar_t* root_directory;
    void (*audit)(const char* message);
    const ShroudweaveServerServicesV1* services;
    const ShroudweaveServerStartupServicesV1* startup_services;
    const ShroudweaveServerFinalValidationServicesV1* final_validation_services;
} ShroudweaveServerHostV1;

#define SHROUDWEAVE_SERVER_HOST_V1_PREFIX_SIZE \
    ((uint32_t)offsetof(ShroudweaveServerHostV1, services))
#define SHROUDWEAVE_SERVER_HOST_V1_SERVICES_SIZE \
    ((uint32_t)(offsetof(ShroudweaveServerHostV1, services) + \
        sizeof(((ShroudweaveServerHostV1*)0)->services)))
#define SHROUDWEAVE_SERVER_HOST_V1_STARTUP_SERVICES_SIZE \
    ((uint32_t)(offsetof(ShroudweaveServerHostV1, startup_services) + \
        sizeof(((ShroudweaveServerHostV1*)0)->startup_services)))
#define SHROUDWEAVE_SERVER_HOST_V1_FINAL_VALIDATION_SERVICES_SIZE \
    ((uint32_t)(offsetof(ShroudweaveServerHostV1, final_validation_services) + \
        sizeof(((ShroudweaveServerHostV1*)0)->final_validation_services)))

// Return zero only when the module has installed its guarded runtime and is
// ready for the server entry to resume. The module remains loaded for life.
typedef uint32_t (__cdecl *ShroudweaveServerModuleStartFnV1)(const ShroudweaveServerHostV1*);
