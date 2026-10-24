#pragma once
#include <windows.h>
#include <filesystem>
namespace shroudweave::server {
// Called once after admission commits, outside DllMain.
// A final validator requires actual parsed synchronous config, before DLL effects.
// A configured ForwardTo readiness gate additionally requires explicit synchronous
// Deferred=false/DelayMs=0 and waits here, never from DllMain. False withholds final
// validation; the mapped child remains resident and must not be retried/unloaded.
bool initialize_legacy_loader(const std::filesystem::path& root, HMODULE self,
    bool require_synchronous = false) noexcept;
}
