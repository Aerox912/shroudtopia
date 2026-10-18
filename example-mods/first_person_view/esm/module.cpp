// Offline adoption scaffold. Never installs native hooks or reads live config.
#define ESM_MODULE_EXPORTS
#include <esm/module_v1.h>

namespace {
uint32_t ESM_MODULE_CALL Create(const EsmModuleCreateV1* args, void** out) {
    if (!out) return ESM_RUNTIME_STATUS_INVALID_ARGUMENT;
    *out = nullptr;
    if (!args || args->struct_size != sizeof(*args))
        return ESM_RUNTIME_STATUS_INVALID_ARGUMENT;
    if (args->abi_major != ESM_MODULE_ABI_MAJOR_V1)
        return ESM_RUNTIME_STATUS_UNSUPPORTED_ABI;
    // Fixed v1 has no camera/HUD provider or native hook quiescence contract.
    // Do not retain borrowed input or manufacture a successfully loaded mod.
    return ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER;
}
uint32_t ESM_MODULE_CALL Unavailable(void*) {
    return ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER;
}
uint32_t ESM_MODULE_CALL Session(void*, const EsmModuleSessionV1*) {
    return ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER;
}
}
extern "C" ESM_MODULE_EXPORT uint32_t ESM_MODULE_CALL EsmModule_GetApi(
    uint32_t major, uint32_t bytes, EsmModuleApiV1* out) {
    if (!out || bytes != sizeof(*out)) return ESM_RUNTIME_STATUS_INVALID_ARGUMENT;
    *out = {};
    if (major != ESM_MODULE_ABI_MAJOR_V1) return ESM_RUNTIME_STATUS_UNSUPPORTED_ABI;
    *out = {sizeof(*out), ESM_MODULE_ABI_MAJOR_V1, Create, Unavailable,
            Session, Session, Unavailable, Unavailable};
    return ESM_RUNTIME_STATUS_OK;
}
