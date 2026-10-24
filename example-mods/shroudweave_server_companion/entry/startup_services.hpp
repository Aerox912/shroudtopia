#pragma once
#include "dedicated_native_v1.h"
#include "server_startup_v1.h"
#include "server_final_validation_v1.h"
namespace shroudweave::server::startup {
// Host-private coordinator. Begin only after the native host claim succeeds.
// No witness, API bridge, or host lifecycle method is exposed to modules.
bool begin(const EsmDedicatedNativeStartV1& witness,
    const EsmDedicatedNativeApiV1& api, void* executable) noexcept;
const ShroudweaveServerStartupServicesV1* enter_module(void* module) noexcept;
const ShroudweaveServerFinalValidationServicesV1* final_validation_for_current_module() noexcept;
bool has_final_validators() noexcept;
// Call after synchronous legacy loading. Closes module/commit registration;
// each copied validator runs once, outside locks, before finish().
bool run_final_validators() noexcept;
// Close on normal return and on C++ exception. False means this module failed;
// a failure with any committed patch also permanently poisons finish().
bool leave_module(void* module, bool module_ready) noexcept;
bool finish() noexcept;
void abort() noexcept;
}
