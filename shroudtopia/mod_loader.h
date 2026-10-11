#pragma once
#include <map>
#include <optional>
#include "utils.h"
#include "shroudtopia.h"

extern ModContext* modContext;
inline std::map<std::string, HMODULE> loadedDLLs;
using CreateModFunc = Mod* (*)();

struct DllStamp {
    std::uintmax_t size;
    std::filesystem::file_time_type written;
    bool operator==(const DllStamp&) const = default;
};

inline std::optional<DllStamp> ReadDllStamp(const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return std::nullopt;
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) return std::nullopt;
    return DllStamp{size, written};
}

// Only permanent API mismatches are remembered. LoadLibrary failures and null
// factories remain retryable, so late dependencies do not disable valid mods.
inline std::map<std::string, DllStamp> incompatibleDLLs;

inline void PruneIncompatibleDlls() {
    for (auto it = incompatibleDLLs.begin(); it != incompatibleDLLs.end();) {
        std::error_code error;
        const bool exists = std::filesystem::exists(it->first, error);
        if (!exists && !error) it = incompatibleDLLs.erase(it);
        else ++it;
    }
}

// Load a mod and register it
inline void LoadAndRegisterMod(const std::string& dllPath) {
    if (loadedDLLs.count(dllPath)) return;

    const auto stamp = ReadDllStamp(dllPath);
    const auto rejected = incompatibleDLLs.find(dllPath);
    if (rejected != incompatibleDLLs.end()) {
        if (stamp && rejected->second == *stamp) return;
        incompatibleDLLs.erase(rejected);
    }

    Utils::Log(Utils::DEBUG, "Attempting to load: %s", dllPath.c_str());

    HMODULE hMod = LoadLibraryA(dllPath.c_str());
    if (!hMod) {
        Utils::Log(Utils::ERRR, "Failed to load DLL: %s", dllPath.c_str());
        return;
    }

    CreateModFunc createMod = (CreateModFunc)GetProcAddress(hMod, "CreateModInstance");
    if (!createMod) {
        Utils::Log(Utils::ERRR, "CreateModInstance not found in: %s", dllPath.c_str());
        if (stamp) incompatibleDLLs.insert_or_assign(dllPath, *stamp);
        FreeLibrary(hMod);
        return;
    }

    Mod* modInstance = createMod();
    if (!modInstance) {
        Utils::Log(Utils::ERRR, "Failed to create mod instance for: %s", dllPath.c_str());
        FreeLibrary(hMod);
        return;
    }

    loadedDLLs[dllPath] = hMod;
    modContext->registeredMods[dllPath] = modInstance;
    Utils::Log(Utils::INFO, "Registered mod: %s", modInstance->GetMetaData().name.c_str());
}
