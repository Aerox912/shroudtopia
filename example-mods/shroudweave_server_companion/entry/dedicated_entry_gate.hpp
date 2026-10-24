#pragma once
#include <windows.h>

namespace esm::host {
// Loader-private static-import bootstrap. No readiness authority is public.
class DedicatedEntryGate final {
public:
    static bool arm(HMODULE executable, bool process_start_attach) noexcept;
};
}
