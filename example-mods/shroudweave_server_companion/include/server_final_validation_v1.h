#ifndef SHROUDWEAVE_SERVER_FINAL_VALIDATION_V1_H
#define SHROUDWEAVE_SERVER_FINAL_VALIDATION_V1_H
#include <stdint.h>
#if defined(_MSC_VER)
#define SHROUDWEAVE_FINAL_VALIDATION_CALL __cdecl
#else
#define SHROUDWEAVE_FINAL_VALIDATION_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define SHROUDWEAVE_SERVER_FINAL_VALIDATION_ABI_MAJOR_V1 1u
#define SHROUDWEAVE_SERVER_FINAL_VALIDATION_OK_V1 0u
#define SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1 1u
#define SHROUDWEAVE_SERVER_FINAL_VALIDATION_INVALID_V1 2u
#define SHROUDWEAVE_SERVER_FINAL_VALIDATION_FAILED_V1 3u
/* Optional independent HostV1 extension. Check HostV1 tail coverage, then this
 * table's size/version before use. Register only during this root module's
 * synchronous Start on the held-entry thread, once, with nonnull executable
 * code in that pinned module. A root wrapper handles its own child modules.
 * The host copies callback and user_context; null user_context is valid.
 * Context data must live for the process, including failed startup. Never
 * retain the borrowed HostV1 object or pass it as user_context.
 *
 * After all Starts and synchronous legacy loading, the host invokes validators
 * once in module order on the same thread, outside SDK locks. Return zero for
 * success. Nonzero, C++ exception or lifecycle misuse prevents routing and
 * entry release, without retry, rollback or unload. Failed Start invokes none.
 * Registration and startup commits are retired during final callbacks.
 * Duplicate, invalid, stale, foreign-thread or reentrant registration poisons
 * an open startup even if the module ignores the returned error. Registration
 * contexts are never reused; this table grants no later registration rights.
 *
 * Callbacks are read-only by contract. This does not enforce purity, quiesce
 * worker threads, grant native lifetime authority, or prove later gameplay.
 * Modules with validators require synchronous legacy config: Deferred=true
 * refuses startup before any configured DLL is forwarded or loaded.
 * No allocation crosses the ABI; callbacks must not throw across it.
 */
typedef uint32_t (SHROUDWEAVE_FINAL_VALIDATION_CALL *ShroudweaveServerFinalValidationCallbackV1)(
    void* user_context);
typedef uint32_t (SHROUDWEAVE_FINAL_VALIDATION_CALL *ShroudweaveServerRegisterFinalValidationFnV1)(
    void* registration_context, ShroudweaveServerFinalValidationCallbackV1 callback,
    void* user_context);
typedef struct ShroudweaveServerFinalValidationServicesV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    void* registration_context;
    ShroudweaveServerRegisterFinalValidationFnV1 register_validator;
} ShroudweaveServerFinalValidationServicesV1;
#ifdef __cplusplus
}
#endif
#endif
