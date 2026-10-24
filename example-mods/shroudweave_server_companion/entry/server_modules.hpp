#pragma once
#include "inventory.hpp"
#include <windows.h>
#include <filesystem>
namespace shroudweave::server {
class ServerServicesRuntime;
// Both runtime registration and native startup admission surround each Start.
// Every invoked Start failure is fatal, including optional modules. Modules
// that reached Start remain resident for process lifetime on every outcome.
bool start_server_modules(const Inventory& inventory, const std::filesystem::path& root,
    HMODULE executable, HMODULE companion, ServerServicesRuntime& services,
    void (*audit)(const char*)) noexcept;
}
