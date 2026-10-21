#pragma once
#include "mod_loader.h"
#include <fstream>
#include <filesystem>

void ProcessModManifestConfig(const std::string& modId, const json& manifest);

// Check for new mods using EML logic + legacy fallback
inline void CheckAndLoadDlls(const std::string& modFolder = SHROUDTOPIA_MOD_FOLDER) {
    namespace fs = std::filesystem;
    PruneIncompatibleDlls();
    if (!fs::exists(modFolder)) {
        fs::create_directory(modFolder);
        return;
    }

    // 1. Scan for EML-style mods: mods/[mod_name]/mod.json
    for (const auto& entry : fs::directory_iterator(modFolder)) {
        if (entry.is_directory()) {
            fs::path dirPath = entry.path();
            fs::path manifestPath = dirPath / "mod.json";

            if (fs::exists(manifestPath)) {
                try {
                    std::ifstream f(manifestPath);
                    json manifest = json::parse(f);

                    // Flame owns its DLL even if a second loader namespace is
                    // present. Never probe foreign code to discover its API:
                    // LoadLibrary runs DllMain before GetProcAddress can reject it.
                    // Malformed ownership metadata is not a legacy manifest.
                    if (!manifest.is_object() || manifest.contains("flame") ||
                        (manifest.contains("shroudtopia") && !manifest["shroudtopia"].is_object())) {
                        continue;
                    }

                    std::string modId = manifest.value("id", dirPath.filename().string());

                    // Process shroudtopia defaults
                    ProcessModManifestConfig(modId, manifest);

                    // Look for [mod_id].dll inside the folder
                    fs::path dllPath = dirPath / (modId + ".dll");
                    if (fs::exists(dllPath)) {
                        LoadAndRegisterMod(dllPath.string());
                    } else {
                        // Fallback: search for any .dll in that specific folder if ID-named one doesn't exist
                        for (const auto& subEntry : fs::directory_iterator(dirPath)) {
                            if (subEntry.path().extension() == ".dll") {
                                LoadAndRegisterMod(subEntry.path().string());
                                break;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    Utils::Log(Utils::ERRR, "Failed to parse manifest at %s: %s", manifestPath.string().c_str(), e.what());
                }
            }
        }
    }

    // 2. Legacy Fallback: Scan root mods/*.dll
    for (const auto& entry : fs::directory_iterator(modFolder)) {
        if (!entry.is_directory() && entry.path().extension() == ".dll") {
            LoadAndRegisterMod(entry.path().string());
        }
    }
}
