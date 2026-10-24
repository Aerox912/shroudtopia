#include "server_module_v1.h"

#if !defined(_WIN64)
#error This fixture pins the supported Windows x64 ABI layout.
#endif

_Static_assert(sizeof(void*) == 8, "Windows x64 pointer width changed");

_Static_assert(sizeof(ShroudweaveServerAuthorizationPolicyV1) == 40,
    "authorization policy size changed");
_Static_assert(offsetof(ShroudweaveServerAuthorizationPolicyV1, selected_group) == 24,
    "selected_group offset changed");
_Static_assert(offsetof(ShroudweaveServerAuthorizationPolicyV1, selected_group_length) == 32,
    "selected_group_length offset changed");

_Static_assert(sizeof(ShroudweaveServerSettingsPageV1) == 120,
    "settings page descriptor size changed");
_Static_assert(offsetof(ShroudweaveServerSettingsPageV1, authorization) == 8,
    "settings page authorization offset changed");
_Static_assert(offsetof(ShroudweaveServerSettingsPageV1, page_id) == 48,
    "settings page id offset changed");
_Static_assert(offsetof(ShroudweaveServerSettingsPageV1, callback_context) == 80,
    "settings page callback context offset changed");
_Static_assert(offsetof(ShroudweaveServerSettingsPageV1, migrate) == 112,
    "settings page migration callback offset changed");

_Static_assert(sizeof(ShroudweaveServerActionProviderV1) == 88,
    "action provider descriptor size changed");
_Static_assert(offsetof(ShroudweaveServerActionProviderV1, authorize) == 72,
    "action authorize callback offset changed");
_Static_assert(offsetof(ShroudweaveServerActionProviderV1, execute) == 80,
    "action execute callback offset changed");

_Static_assert(offsetof(ShroudweaveServerServicesV1, struct_size) == 0,
    "services table size field moved");
_Static_assert(offsetof(ShroudweaveServerServicesV1, abi_major) == 4,
    "services table ABI field moved");
_Static_assert(offsetof(ShroudweaveServerServicesV1, services_context) == 8,
    "services table context moved");
_Static_assert(offsetof(ShroudweaveServerServicesV1, register_settings_page) == 16,
    "registration prefix changed");
_Static_assert(offsetof(ShroudweaveServerServicesV1, register_action_provider) == 24,
    "registration prefix changed");
_Static_assert(offsetof(ShroudweaveServerServicesV1, read_settings_page) == 32,
    "settings read prefix changed");
_Static_assert(offsetof(ShroudweaveServerServicesV1, pump_actions) == 40,
    "action pump prefix changed");
_Static_assert(offsetof(ShroudweaveServerServicesV1, is_peer_current) == 48,
    "peer-current prefix changed");
_Static_assert(offsetof(ShroudweaveServerServicesV1, observe_creation_context) == 56,
    "optional observation tail moved");
_Static_assert(sizeof(ShroudweaveServerServicesV1) == 64,
    "services table size changed");
_Static_assert(SHROUDWEAVE_SERVER_SERVICES_V1_REQUIRED_SIZE == 56,
    "required services prefix size changed");
_Static_assert(SHROUDWEAVE_SERVER_SERVICES_V1_OBSERVER_SIZE == 64,
    "optional observer table size changed");

typedef struct LegacyShroudweaveServerHostV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    HMODULE executable;
    HMODULE companion;
    HMODULE module;
    const char* module_id;
    const wchar_t* root_directory;
    void (*audit)(const char* message);
    const ShroudweaveServerServicesV1* services;
} LegacyShroudweaveServerHostV1;

_Static_assert(sizeof(LegacyShroudweaveServerHostV1) == 64,
    "frozen services-era HostV1 prefix layout changed");
_Static_assert(offsetof(LegacyShroudweaveServerHostV1, services) == 56,
    "frozen HostV1 services pointer offset changed");

_Static_assert(offsetof(ShroudweaveServerHostV1, struct_size) == 0,
    "host size field moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, abi_major) == 4,
    "host ABI field moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, executable) == 8,
    "host executable prefix moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, companion) == 16,
    "host companion prefix moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, module) == 24,
    "host module prefix moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, module_id) == 32,
    "host module-id prefix moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, root_directory) == 40,
    "host root prefix moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, audit) == 48,
    "host audit prefix moved");
_Static_assert(offsetof(ShroudweaveServerHostV1, services) == 56,
    "services pointer must remain at the V1 Host tail");
_Static_assert(offsetof(ShroudweaveServerHostV1, startup_services) == 64,
    "startup table must be appended after the established services tail");
_Static_assert(sizeof(ShroudweaveServerStartupServicesV1) == 24,
    "startup-services table size changed");
_Static_assert(offsetof(ShroudweaveServerHostV1, final_validation_services) == 72,
    "final-validation table must follow the pinned startup-services tail");
_Static_assert(SHROUDWEAVE_SERVER_HOST_V1_PREFIX_SIZE == 56,
    "legacy host prefix size changed");
_Static_assert(SHROUDWEAVE_SERVER_HOST_V1_SERVICES_SIZE == 64,
    "services-era host size changed");
_Static_assert(SHROUDWEAVE_SERVER_HOST_V1_STARTUP_SERVICES_SIZE == 72,
    "startup-services host size macro changed");
_Static_assert(sizeof(ShroudweaveServerHostV1) == 80,
    "final-validation table must extend HostV1 by one pointer");
_Static_assert(SHROUDWEAVE_SERVER_HOST_V1_FINAL_VALIDATION_SERVICES_SIZE == 80,
    "final-validation HostV1 size macro changed");

/* A v2 consumer compiled against the services-era Host prefix reads only its
 * old 64-byte view and accepts a future larger struct_size. */
static int legacy_v2_accepts_larger_host(const void* host_bytes) {
    LegacyShroudweaveServerHostV1 old_view = {0};
    memcpy(&old_view, host_bytes, sizeof(old_view));
    return old_view.struct_size >= sizeof(old_view) &&
        old_view.abi_major == SHROUDWEAVE_SERVER_SERVICES_ABI_MAJOR_V1 &&
        old_view.services != NULL;
}

int main(void) {
    ShroudweaveServerServicesV1 services = {0};
    ShroudweaveServerStartupServicesV1 startup_services = {0};
    ShroudweaveServerFinalValidationServicesV1 final_validation_services = {0};
    ShroudweaveServerHostV1 host = {0};
    host.struct_size = SHROUDWEAVE_SERVER_HOST_V1_FINAL_VALIDATION_SERVICES_SIZE;
    host.abi_major = SHROUDWEAVE_SERVER_SERVICES_ABI_MAJOR_V1;
    host.services = &services;
    host.startup_services = &startup_services;
    host.final_validation_services = &final_validation_services;
    if (sizeof(host) != SHROUDWEAVE_SERVER_HOST_V1_FINAL_VALIDATION_SERVICES_SIZE) return 1;
    if (!legacy_v2_accepts_larger_host(&host)) return 2;
    return 0;
}