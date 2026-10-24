#ifndef SHROUDWEAVE_SERVER_STARTUP_V1_H
#define SHROUDWEAVE_SERVER_STARTUP_V1_H
#include <stddef.h>
#include <stdint.h>
#if defined(_MSC_VER)
#define SHROUDWEAVE_STARTUP_CALL __cdecl
#else
#define SHROUDWEAVE_STARTUP_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define SHROUDWEAVE_SERVER_STARTUP_ABI_MAJOR_V1 1u
#define SHROUDWEAVE_SERVER_STARTUP_OK_V1 0u
#define SHROUDWEAVE_SERVER_STARTUP_UNAVAILABLE_V1 1u
#define SHROUDWEAVE_SERVER_STARTUP_INVALID_V1 2u
#define SHROUDWEAVE_SERVER_STARTUP_FAILED_V1 3u
#define SHROUDWEAVE_SERVER_STARTUP_CONFLICT_V1 4u
/* Optional, independently versioned startup extension. HostV1 consumers must
 * check the appended pointer's struct_size coverage before accessing it.
 * Only this module's synchronous ModuleStart on the actual held entry thread
 * may use this table. A cached table/context is permanently retired at return;
 * it never carries the private native bootstrap witness or runtime authority.
 *
 * replacement and expected each point to five readable encoded E8 rel32
 * instruction bytes. Displacements are relative to site + 5, never to either
 * byte buffer. replacement is not an executable target pointer. Commit replaces
 * one CALL in executable-image code after comparing all five expected bytes.
 * The module owns the complete
 * feature profile, original-call ABI, and callback behavior. The new target
 * must be code in the pinned module, or a canonical RX private leaf relay
 * FF 25 00 00 00 00 <callback pointer> into that module. The module retains its
 * relay and callback for process lifetime, including failed startup. The host
 * rejects overlapping sites and its bootstrap prologue. Limits: 64 total sites
 * and 64 module contexts, never reset/reused. No allocation crosses the ABI.
 *
 * This is a startup-entry barrier, not suspension of all worker threads. The
 * trusted module must establish that its site cannot execute concurrently at
 * this stage. This is not a general memory writer, runtime patcher, atomic
 * transaction, rollback or unload service. A failure after any successful
 * commit makes startup unreleasable; partial changes remain resident.
 * Inputs are borrowed for the call. Callbacks must not throw across this ABI.
 */
typedef uint32_t (SHROUDWEAVE_STARTUP_CALL *ShroudweaveServerCommitDirectCallFnV1)(
    void* startup_context, void* site, const void* replacement,
    const void* expected, size_t size);
typedef struct ShroudweaveServerStartupServicesV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    void* startup_context;
    ShroudweaveServerCommitDirectCallFnV1 commit_direct_call;
} ShroudweaveServerStartupServicesV1;
#ifdef __cplusplus
}
#endif
#endif
