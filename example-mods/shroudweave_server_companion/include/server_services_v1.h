#ifndef SHROUDWEAVE_SERVER_SERVICES_V1_H
#define SHROUDWEAVE_SERVER_SERVICES_V1_H

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#define SHROUDWEAVE_CALL __cdecl
#else
#define SHROUDWEAVE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SHROUDWEAVE_SERVER_SERVICES_ABI_MAJOR_V1 1u
#define SHROUDWEAVE_SERVER_MAX_ID_BYTES_V1 64u
#define SHROUDWEAVE_SERVER_MAX_JSON_BYTES_V1 49152u

#define SHROUDWEAVE_SERVER_STATUS_OK_V1 0u
#define SHROUDWEAVE_SERVER_STATUS_UNAVAILABLE_V1 1u
#define SHROUDWEAVE_SERVER_STATUS_INVALID_V1 2u
#define SHROUDWEAVE_SERVER_STATUS_FAILED_V1 3u

#define SHROUDWEAVE_SERVER_AUTH_EXACT_GROUP_V1 1u
#define SHROUDWEAVE_SERVER_AUTH_EQUIVALENT_PERMISSIONS_V1 2u
#define SHROUDWEAVE_SERVER_AUTH_OPEN_CONNECTED_V1 3u

/*
 * IDs are explicit-length, lowercase ASCII and match [a-z][a-z0-9_-]{0,63}.
 * JSON inputs are UTF-8, explicit-length JSON objects, bounded by
 * SHROUDWEAVE_SERVER_MAX_JSON_BYTES_V1. No allocation ownership crosses this
 * ABI. A module may cache the services table and its opaque context for the
 * process lifetime. Descriptor pointers and strings are borrowed only for the
 * registration call; the host copies them before that call returns. Callback
 * pointers and callback contexts remain module-owned, so an accepted module
 * remains loaded for the process lifetime. Callbacks must not reenter the
 * services table while the host validates or transforms data.
 */
typedef struct ShroudweaveServerAuthorizationPolicyV1 {
    uint32_t struct_size;
    uint32_t mode;
    uint64_t revision;
    uint64_t selected_group_permissions;
    const char* selected_group;
    uint32_t selected_group_length;
} ShroudweaveServerAuthorizationPolicyV1;

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerSettingsValidateFnV1)(
    void* callback_context,
    const char* values_json,
    uint32_t values_json_length);

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerSettingsMigrateFnV1)(
    void* callback_context,
    uint32_t source_schema_version,
    uint64_t source_revision,
    const char* source_values_json,
    uint32_t source_values_json_length,
    char* output_values_json,
    uint32_t output_capacity,
    uint32_t* output_values_json_length,
    uint64_t* output_revision);

typedef struct ShroudweaveServerSettingsPageV1 {
    uint32_t struct_size;
    uint32_t schema_version;
    ShroudweaveServerAuthorizationPolicyV1 authorization;
    const char* page_id;
    uint32_t page_id_length;
    const char* defaults_json;
    uint32_t defaults_json_length;
    void* callback_context;
    ShroudweaveServerSettingsValidateFnV1 validate;
    const char* legacy_page_id;
    uint32_t legacy_page_id_length;
    ShroudweaveServerSettingsMigrateFnV1 migrate;
} ShroudweaveServerSettingsPageV1;

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerActionAuthorizeFnV1)(
    void* callback_context,
    const char* action,
    uint32_t action_length,
    const char* values_json,
    uint32_t values_json_length,
    uint32_t* required_capabilities);

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerActionExecuteFnV1)(
    void* callback_context,
    uint64_t peer_steam,
    uint64_t connection,
    uint64_t world,
    const char* action,
    uint32_t action_length,
    const char* values_json,
    uint32_t values_json_length);

typedef struct ShroudweaveServerActionProviderV1 {
    uint32_t struct_size;
    ShroudweaveServerAuthorizationPolicyV1 authorization;
    const char* route_id;
    uint32_t route_id_length;
    uint32_t capability_mask;
    void* callback_context;
    ShroudweaveServerActionAuthorizeFnV1 authorize;
    ShroudweaveServerActionExecuteFnV1 execute;
} ShroudweaveServerActionProviderV1;

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerRegisterSettingsPageFnV1)(
    void* services_context,
    const ShroudweaveServerSettingsPageV1* page);

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerRegisterActionProviderFnV1)(
    void* services_context,
    const ShroudweaveServerActionProviderV1* provider);

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerReadSettingsPageFnV1)(
    void* services_context,
    const char* page_id,
    uint32_t page_id_length,
    char* output_values_json,
    uint32_t output_capacity,
    uint32_t* output_values_json_length,
    uint64_t* output_revision);

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerPumpActionsFnV1)(
    void* services_context,
    const char* route_id,
    uint32_t route_id_length,
    uint32_t available_capabilities);

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerIsPeerCurrentFnV1)(
    void* services_context,
    uint64_t peer_steam,
    uint64_t connection,
    uint64_t world);

typedef uint32_t (SHROUDWEAVE_CALL *ShroudweaveServerObserveCreationContextFnV1)(
    void* services_context,
    uintptr_t manager,
    uintptr_t native_caller);

typedef struct ShroudweaveServerServicesV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    void* services_context;
    ShroudweaveServerRegisterSettingsPageFnV1 register_settings_page;
    ShroudweaveServerRegisterActionProviderFnV1 register_action_provider;
    ShroudweaveServerReadSettingsPageFnV1 read_settings_page;
    ShroudweaveServerPumpActionsFnV1 pump_actions;
    ShroudweaveServerIsPeerCurrentFnV1 is_peer_current;
    ShroudweaveServerObserveCreationContextFnV1 observe_creation_context;
} ShroudweaveServerServicesV1;

/* The observer is an optional tail field. Required V1 callers accept a larger
 * table and validate the required prefix instead of requiring an exact size. */
#define SHROUDWEAVE_SERVER_SERVICES_V1_REQUIRED_SIZE \
    ((uint32_t)offsetof(ShroudweaveServerServicesV1, observe_creation_context))
#define SHROUDWEAVE_SERVER_SERVICES_V1_OBSERVER_SIZE \
    ((uint32_t)(offsetof(ShroudweaveServerServicesV1, observe_creation_context) + \
        sizeof(((ShroudweaveServerServicesV1*)0)->observe_creation_context)))

#ifdef __cplusplus
}
#endif

#endif
