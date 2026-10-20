#include <vector>
#include "shroudtopia.h"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[32768]{};
        const DWORD count = GetModuleFileNameW(instance, path, 32768);
        const std::wstring counter = std::wstring(path, count) + L".loads";
        HANDLE file = CreateFileW(counter.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, "1", 1, &written, nullptr);
            CloseHandle(file);
        }
    }
    return TRUE;
}

#ifdef VALID_MOD
class Fixture : public Mod {
public:
    ModMetaData GetMetaData() override { return {"fixture", "test", "1", "test", "0.1.1", true, true}; }
    void Load(ModContext*) override {}
    void Unload(ModContext*) override {}
    void Activate(ModContext*) override {}
    void Deactivate(ModContext*) override {}
    void Update(ModContext*) override {}
};
extern "C" __declspec(dllexport) Mod* CreateModInstance() {
#ifdef NULL_FACTORY
    if (GetFileAttributesW(L"factory-ready") == INVALID_FILE_ATTRIBUTES) return nullptr;
#endif
    return new Fixture;
}
#else
extern "C" __declspec(dllexport) void XhlNativePluginQuery() {}
#endif
