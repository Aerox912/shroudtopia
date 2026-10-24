#pragma once
#include <windows.h>
#include <filesystem>
namespace shroudweave::server {
class ServerServicesRuntime;
// Startup-thread only, one attempt. Installs the real connection observer and
// returns a process-long generic services host; modules choose all policy.
ServerServicesRuntime* prepare_shared_runtime(HMODULE game,
    const std::filesystem::path& root, void (*audit)(const char*)) noexcept;
// Call after registration freeze and private native startup validation. No
// worker/transport is started while a module's Start is still active.
bool start_shared_runtime_routing(ServerServicesRuntime& services) noexcept;
// Permanently removes authority/routing; never unloads modules or host objects.
void stop_shared_runtime() noexcept;
#if defined(SHROUDWEAVE_HOST_TEST_SEAMS)
namespace host_test {
// Linked only into synthetic smoke fixtures. Production never defines this
// macro and always starts the owned Steam transport worker.
bool start_transport(ServerServicesRuntime& services, void (*audit)(const char*)) noexcept;
}
#endif
}
