#include <vector>
#include <iostream>
#include <stdexcept>
#include "mod_loader.h"
#include "mod_discovery.h"

int manifestConfigCalls = 0;
void ProcessModManifestConfig(const std::string&, const json&) {
    ++manifestConfigCalls;
}

json Config::jConfig;
std::filesystem::file_time_type Config::lastModifiedTime;
ModContext testContext{};
ModContext* modContext = &testContext;
int checks = 0;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
int main() {
    namespace fs = std::filesystem;
    try {
        wchar_t exe[32768]{};
        check(GetModuleFileNameW(nullptr, exe, 32768) != 0, "test executable location");
        const auto fixtures = fs::path(exe).parent_path();
        const auto original = fs::current_path();
        const auto root = fs::temp_directory_path() / (L"shroudtopia-loader-tests-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);
        fs::current_path(root);
        Config::jConfig = {{"enableLogging", true}, {"logLevel", "VERBOSE"}};

        // The real production directory scanner must reject a foreign manifest
        // before LoadLibrary invokes its DLL entry point or changes defaults.
        const auto mods = root / "mods";
        const auto creative = mods / "creative";
        fs::create_directories(creative);
        const auto foreign = creative / "creative.dll";
        fs::copy_file(fixtures / "flame_fixture.dll", foreign);
        {
            std::ofstream manifest(creative / "mod.json");
            manifest << R"({"id":"creative","flame":{"dll":"creative.dll"}})";
        }
        CheckAndLoadDlls(mods.string());
        check(!fs::exists(foreign.string() + ".loads"), "Flame-only manifest must not execute DllMain");
        check(manifestConfigCalls == 0, "foreign manifest must not change native defaults");

        const auto sidecar = root / "sidecar.dll";
        fs::copy_file(fixtures / "xhl_fixture.dll", sidecar);
        LoadAndRegisterMod(sidecar.string());
        const auto log_size = fs::file_size(SHROUDTOPIA_LOG_FILE);
        for (int i = 0; i < 20; ++i) LoadAndRegisterMod(sidecar.string());
        check(fs::file_size(sidecar.string() + ".loads") == 1, "unchanged incompatible DLL must be loaded only once");
        check(fs::file_size(SHROUDTOPIA_LOG_FILE) == log_size, "unchanged incompatible DLL must not grow the log");
        for (int i = 0; i < 10000; ++i) LoadAndRegisterMod(sidecar.string());
        check(fs::file_size(sidecar.string() + ".loads") == 1, "long idle polling does not reload sidecar");
        check(fs::file_size(SHROUDTOPIA_LOG_FILE) == log_size, "long idle polling does not relog sidecar");
        check(testContext.registeredMods.empty(), "XHL sidecar is not a Shroudtopia mod");

        const auto stamp = fs::last_write_time(sidecar);
        { std::ofstream extra(sidecar, std::ios::binary | std::ios::app); extra << 'x'; }
        fs::last_write_time(sidecar, stamp);
        LoadAndRegisterMod(sidecar.string());
        check(fs::file_size(sidecar.string() + ".loads") == 2, "size change retries even with identical timestamp");
        fs::last_write_time(sidecar, stamp + std::chrono::seconds(2));
        LoadAndRegisterMod(sidecar.string());
        check(fs::file_size(sidecar.string() + ".loads") == 3, "timestamp change retries even with identical size");

        fs::copy_file(fixtures / "valid_fixture.dll", sidecar, fs::copy_options::overwrite_existing);
        fs::last_write_time(sidecar, stamp + std::chrono::seconds(4));
        LoadAndRegisterMod(sidecar.string());
        check(testContext.registeredMods.size() == 1, "changed sidecar can become a valid mod");
        for (int i = 0; i < 100; ++i) LoadAndRegisterMod(sidecar.string());
        check(fs::file_size(sidecar.string() + ".loads") == 4, "valid mod registers only once");

        const auto delayed = root / "delayed.dll";
        LoadAndRegisterMod(delayed.string());
        fs::copy_file(fixtures / "valid_fixture.dll", delayed);
        LoadAndRegisterMod(delayed.string());
        check(testContext.registeredMods.size() == 2, "transient load failure must remain retryable");
        const auto not_ready = root / "not-ready.dll";
        fs::copy_file(fixtures / "null_fixture.dll", not_ready);
        LoadAndRegisterMod(not_ready.string());
        LoadAndRegisterMod(not_ready.string());
        check(fs::file_size(not_ready.string() + ".loads") == 2, "null factories remain retryable");
        { std::ofstream ready("factory-ready"); ready << '1'; }
        LoadAndRegisterMod(not_ready.string());
        check(testContext.registeredMods.size() == 3, "a recovered factory registers without changing its DLL");

        const auto removed = root / "removed.dll";
        fs::copy_file(fixtures / "xhl_fixture.dll", removed);
        LoadAndRegisterMod(removed.string());
        fs::remove(removed);
        PruneIncompatibleDlls();
        check(incompatibleDLLs.empty(), "removed DLLs do not accumulate rejection entries");
        fs::copy_file(fixtures / "valid_fixture.dll", removed);
        LoadAndRegisterMod(removed.string());
        check(testContext.registeredMods.size() == 4, "recreated DLL is discovered");

        const auto writeManifest = [](const fs::path& folder, const std::string& value) {
            std::ofstream manifest(folder / "mod.json");
            manifest << value;
        };
        // Both the ID-named and first-DLL fallback paths must stay behind the
        // ownership boundary, including mixed and malformed ownership metadata.
        const std::vector<std::string> rejectedManifests{
            R"({"id":"foreign","flame":{"dll":"payload.dll"},"shroudtopia":{"default":{"active":true}}})",
            R"({"id":"foreign","flame":null})",
            R"({"id":"foreign","flame":false})",
            R"({"id":"foreign","flame":[]})",
            R"({"id":"foreign","flame":"invalid"})",
            R"({"id":"foreign","shroudtopia":null})",
            R"({"id":"foreign","shroudtopia":false})",
            R"({"id":"foreign","shroudtopia":[]})",
            R"({"id":"foreign","shroudtopia":"invalid"})",
            R"({"id":42})", "[]", "null", "42", "{invalid"
        };
        const auto fallback = mods / "fallback";
        fs::create_directories(fallback);
        const auto foreignFallback = fallback / "payload.dll";
        fs::copy_file(fixtures / "flame_fixture.dll", foreignFallback);
        for (const auto& manifest : rejectedManifests) {
            writeManifest(fallback, manifest);
            CheckAndLoadDlls(mods.string());
            check(!fs::exists(foreignFallback.string() + ".loads"), "rejected manifest must not enter DLL fallback");
            check(manifestConfigCalls == 0, "rejected manifest must not apply defaults");
            check(testContext.registeredMods.size() == 4, "rejected manifest must not register a mod");
        }
        writeManifest(fallback, R"({"id":"foreign","flame":{"dll":"payload.dll"}})");
        for (int i = 0; i < 100; ++i) CheckAndLoadDlls(mods.string());
        check(!fs::exists(foreignFallback.string() + ".loads"), "polling never executes a foreign DLL");
        check(!fs::exists(foreign.string() + ".loads"), "polling never executes Creative DLL");

        const auto native = mods / "native";
        fs::create_directories(native);
        const auto nativeDll = native / "native.dll";
        fs::copy_file(fixtures / "valid_fixture.dll", nativeDll);
        writeManifest(native, R"({"id":"native","shroudtopia":{"default":{"active":true}}})");
        CheckAndLoadDlls(mods.string());
        check(fs::file_size(nativeDll.string() + ".loads") == 1, "native manifest executes its DLL once");
        check(testContext.registeredMods.size() == 5, "native manifest registers");
        check(manifestConfigCalls == 1, "native manifest reaches defaults processing");

        const auto legacy = mods / "legacy";
        fs::create_directories(legacy);
        const auto legacyDll = legacy / "different-name.dll";
        fs::copy_file(fixtures / "valid_fixture.dll", legacyDll);
        writeManifest(legacy, R"({"id":"legacy"})");
        CheckAndLoadDlls(mods.string());
        check(fs::file_size(legacyDll.string() + ".loads") == 1, "legacy manifest retains DLL fallback");
        check(testContext.registeredMods.size() == 6, "legacy manifest registers");

        const auto rootLegacy = mods / "root-legacy.dll";
        fs::copy_file(fixtures / "valid_fixture.dll", rootLegacy);
        CheckAndLoadDlls(mods.string());
        check(fs::file_size(rootLegacy.string() + ".loads") == 1, "root legacy DLL discovery remains enabled");
        check(testContext.registeredMods.size() == 7, "root legacy DLL registers");

        // A corrected manifest must be reconsidered without restarting or
        // touching the incompatible-DLL cache or timestamps.
        fs::copy_file(fixtures / "valid_fixture.dll", foreign, fs::copy_options::overwrite_existing);
        writeManifest(creative, R"({"id":"creative","shroudtopia":{"default":{}}})");
        CheckAndLoadDlls(mods.string());
        check(fs::file_size(foreign.string() + ".loads") == 1, "corrected ownership is discovered on the next scan");
        check(testContext.registeredMods.size() == 8, "corrected ownership registers");
        for (int i = 0; i < 100; ++i) CheckAndLoadDlls(mods.string());
        check(fs::file_size(foreign.string() + ".loads") == 1, "corrected native mod registers once");
        check(!fs::exists(foreignFallback.string() + ".loads"), "foreign fallback remains untouched alongside native mods");
        for (auto& entry : testContext.registeredMods) delete entry.second;
        testContext.registeredMods.clear();
        for (auto& entry : loadedDLLs) FreeLibrary(entry.second);
        loadedDLLs.clear();
        fs::current_path(original);
        check(fs::weakly_canonical(root).parent_path() == fs::weakly_canonical(fs::temp_directory_path()), "cleanup scope");
        fs::remove_all(root);
        std::cout << checks << " real-DLL loader checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
