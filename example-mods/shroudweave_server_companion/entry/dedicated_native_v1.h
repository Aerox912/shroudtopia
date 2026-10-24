#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(__cplusplus)
extern "C" {
#endif
/* Private bootstrap/host bridge, never handed to feature modules. The opaque
   witness is issued only by the running native thunk and cannot be queried. */
typedef struct EsmDedicatedNativeStartV1 {
    uint32_t struct_size,abi_major;
    void* proxy;
    uint64_t witness[2];
    uint64_t generation;
} EsmDedicatedNativeStartV1;
typedef struct EsmDedicatedNativeViewV1 {
    uint32_t struct_size,abi_major;
    void* executable;
    void* call_site; /* Reserved: always null. No feature site belongs to bootstrap. */
    uint32_t thread;
    uint32_t committed; /* Number of retained generic CALL commitments (0..64). */
    uint64_t generation;
} EsmDedicatedNativeViewV1;
typedef struct EsmDedicatedNativeApiV1 {
    uint32_t struct_size,abi_major;
    uint32_t (__cdecl* claim)(const EsmDedicatedNativeStartV1*,void* host,EsmDedicatedNativeViewV1*);
    uint32_t (__cdecl* validate)(const EsmDedicatedNativeStartV1*,EsmDedicatedNativeViewV1*);
    uint32_t (__cdecl* commit_call)(const EsmDedicatedNativeStartV1*,void* owner_module,void* site,const void* replacement,const void* expected,size_t);
} EsmDedicatedNativeApiV1;
typedef uint32_t (__cdecl* EsmDedicatedNativeGetApiV1)(uint32_t,uint32_t,EsmDedicatedNativeApiV1*);
typedef uint32_t (__cdecl* EsmDedicatedHostStartV1)(const EsmDedicatedNativeStartV1*);
#if defined(__cplusplus)
}
#endif
