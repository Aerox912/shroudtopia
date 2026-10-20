#include <vector>
#include <iostream>
#include <stdexcept>
#include "mod_loader.h"

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
