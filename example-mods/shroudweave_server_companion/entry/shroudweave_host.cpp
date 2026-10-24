#include "dedicated_native_v1.h"
#include "inventory.hpp"
#include "file_identity.hpp"
#include "legacy_loader.hpp"
#include "server_modules.hpp"
#include "server_services_runtime.hpp"
#include "shared_runtime.hpp"
#include "startup_services.hpp"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern void esm_dedicated_set_host(void*);
extern "C" uint32_t __cdecl EsmDedicatedGate_GetApiV1(uint32_t, uint32_t, EsmDedicatedNativeApiV1*);

namespace {
HANDLE audit_file = INVALID_HANDLE_VALUE;
void audit(const char* message) {
    if (audit_file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char line[512]{};
    const int length = _snprintf_s(line, sizeof line, _TRUNCATE,
        "%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu %s\r\n",
        static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
        GetCurrentProcessId(), message ? message : "");
    const auto bytes = length >= 0 ? static_cast<DWORD>(length) :
        static_cast<DWORD>(std::char_traits<char>::length(line));
    if (bytes) { DWORD written{}; WriteFile(audit_file, line, bytes, &written, nullptr); }
}

bool read_bounded(const std::filesystem::path& file, std::size_t limit, std::string& out) {
    const auto attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || !size || size > limit) return false;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return false;
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !stream.bad() && out.size() == size;
}
struct StartupCleanup {
    bool ready{};
    ~StartupCleanup() {
        if (!ready) {
            shroudweave::server::startup::abort();
            shroudweave::server::stop_shared_runtime();
        }
    }
};
}

bool esm_dedicated_run_host(const EsmDedicatedNativeStartV1* args) {
    StartupCleanup cleanup;
    try {
        using namespace shroudweave::server;
        if (!args || !args->proxy || args->struct_size != sizeof(*args) || args->abi_major != 1) return false;
        esm_dedicated_set_host(args->proxy);
        EsmDedicatedNativeApiV1 api{};
        if (EsmDedicatedGate_GetApiV1(1, sizeof api, &api) != 0 || api.struct_size != sizeof api ||
            api.abi_major != 1 || !api.claim || !api.validate || !api.commit_call) return false;
        EsmDedicatedNativeViewV1 view{};
        if (api.claim(args, args->proxy, &view) != 0 || view.struct_size != sizeof view || view.abi_major != 1 ||
            view.executable != GetModuleHandleW(nullptr) || view.call_site || view.thread != GetCurrentThreadId() ||
            view.generation != args->generation || !startup::begin(*args, api, view.executable)) return false;
        const auto proxy_path = module_path(static_cast<HMODULE>(args->proxy));
        if (proxy_path.empty()) return false;
        const auto root = std::filesystem::path(proxy_path).parent_path();
        std::string text;
        if (!read_bounded(root / L"shroudweave-server.json", 65536, text)) return false;
        const auto inventory = parse_inventory(text);
        if (!inventory) return false;
        const auto log_file = root / L"shroudweave-server.log";
        const auto attributes = GetFileAttributesW(log_file.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
        audit_file = CreateFileW(log_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (audit_file == INVALID_HANDLE_VALUE) return false;
        auto* services = prepare_shared_runtime(static_cast<HMODULE>(view.executable), root, audit);
        if (!services) { audit("SHROUDWEAVE SERVER REFUSED: generic runtime preparation"); return false; }
        if (!start_server_modules(*inventory, root, static_cast<HMODULE>(view.executable),
                static_cast<HMODULE>(args->proxy), *services, audit)) {
            audit("SHROUDWEAVE SERVER REFUSED: module startup"); return false;
        }
        if (!initialize_legacy_loader(root, static_cast<HMODULE>(args->proxy),
                startup::has_final_validators())) {
            audit("SHROUDWEAVE SERVER REFUSED: legacy loader startup"); return false;
        }
        if (!startup::run_final_validators()) {
            audit("SHROUDWEAVE SERVER REFUSED: final module validation"); return false;
        }
        if (!services->freeze_registrations() || !startup::finish()) {
            audit("SHROUDWEAVE SERVER REFUSED: registration freeze or native startup validation"); return false;
        }
        if (!start_shared_runtime_routing(*services) || !services->routing_ready()) {
            audit("SHROUDWEAVE SERVER REFUSED: generic routing startup"); return false;
        }
        audit("SHROUDWEAVE SERVER READY: modules initialized; native entry still held");
        cleanup.ready = true;
        return true;
    } catch (...) {
        audit("SHROUDWEAVE SERVER REFUSED: initialization exception");
        return false;
    }
}
