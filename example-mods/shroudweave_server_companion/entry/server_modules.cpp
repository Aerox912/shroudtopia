#include "server_modules.hpp"
#include "server_module_v1.h"
#include "server_services_runtime.hpp"
#include "startup_services.hpp"
#include "file_identity.hpp"
#include <algorithm>
#include <system_error>

namespace shroudweave::server {
namespace {
bool module_binding(const ModuleSpec& spec) {
    if (spec.id.empty() || spec.id.size() > 64 ||
        spec.id.front() < 'a' || spec.id.front() > 'z') return false;
    if (!std::all_of(spec.id.begin(), spec.id.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-';
        })) return false;
    return spec.file == "shroudweave_" + spec.id + ".dll";
}
}

bool start_server_modules(const Inventory& inventory, const std::filesystem::path& root,
    HMODULE executable, HMODULE companion, ServerServicesRuntime& services,
    void (*audit)(const char*)) noexcept {
    try {
        if (!executable || !companion || !audit || !root.is_absolute()) return false;
        for (const auto& spec : inventory.modules) {
            if (!module_binding(spec)) return false;
            const auto file = root / std::filesystem::path(spec.file);
            const DWORD attributes = GetFileAttributesW(file.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = GetLastError();
                if (!spec.required && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
                    audit(("SHROUDWEAVE MODULE: optional module absent: " + spec.id).c_str());
                    continue;
                }
                return false;
            }
            if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            if (!verify_file(file.wstring(), spec.sha256.c_str())) return false;
            const HMODULE module = LoadLibraryExW(file.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) {
                if (spec.required) return false;
                audit(("SHROUDWEAVE MODULE: optional module could not load before Start: " + spec.id).c_str());
                continue;
            }
            // Never unload after LoadLibrary/Start: DLL code and any callbacks
            // may already be resident, even when later validation fails.
            if (module == companion || module == executable) return false;
            const auto loaded = module_path(module);
            std::error_code error;
            if (loaded.empty() || !std::filesystem::equivalent(file, loaded, error) || error ||
                !verify_file(loaded, spec.sha256.c_str())) return false;
            const auto start = reinterpret_cast<ShroudweaveServerModuleStartFnV1>(
                GetProcAddress(module, "ShroudweaveServerModuleStartV1"));
            HMODULE pinned{};
            if (!start || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(start), &pinned) || pinned != module) return false;
            if (!services.begin_module(spec.id, reinterpret_cast<std::uintptr_t>(module))) return false;
            const auto* startup_table = startup::enter_module(module);
            if (!startup_table) {
                services.finish_module(false);
                return false;
            }
            bool succeeded = false;
            if (const auto* table = services.services_for_current_module()) {
                const auto* final_table = startup::final_validation_for_current_module();
                const ShroudweaveServerHostV1 host{sizeof host, 1, executable, companion, module,
                    spec.id.c_str(), root.c_str(), audit, table, startup_table, final_table};
                try { succeeded = final_table && start(&host) == 0; }
                catch (...) { succeeded = false; }
            }
            // A module that ignores a failed native commit is still failed.
            // Close both domains on normal return and on a module exception.
            const bool startup_closed = startup::leave_module(module, succeeded);
            const bool registered = services.finish_module(succeeded && startup_closed);
            if (!succeeded || !startup_closed || !registered) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
}
