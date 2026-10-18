#include <windows.h>
#include <esm/module_v1.h>
#include <cstdio>
#include <initializer_list>
#include <cstring>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    HMODULE dll = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return 3;
    using GetApi = uint32_t (ESM_MODULE_CALL*)(uint32_t, uint32_t, EsmModuleApiV1*);
    auto get = reinterpret_cast<GetApi>(GetProcAddress(dll, "EsmModule_GetApi"));
    if (!get) return 4;
    int failures = 0, checks = 0;
    auto check = [&](bool pass) { ++checks; if (!pass) { ++failures; std::printf("failed check %d\n", checks); } };
    check(GetProcAddress(dll, "CreateModInstance") == nullptr);
    check(GetProcAddress(dll, "EsmModule_GetServicesApi") == nullptr);
    EsmModuleApiV1 api{};
    check(get(1, sizeof(api), nullptr) == ESM_RUNTIME_STATUS_INVALID_ARGUMENT);
    check(get(1, sizeof(api)-1, &api) == ESM_RUNTIME_STATUS_INVALID_ARGUMENT);
    std::memset(&api, 0x7f, sizeof(api));
    check(get(2, sizeof(api), &api) == ESM_RUNTIME_STATUS_UNSUPPORTED_ABI);
    check(api.create == nullptr && api.start == nullptr);
    check(get(1, sizeof(api), &api) == ESM_RUNTIME_STATUS_OK);
    check(api.struct_size == sizeof(api) && api.abi_major == 1);
    check(api.create && api.start && api.session_begin && api.session_end && api.stop && api.destroy);
    if (!api.create) return 5;
    void* instance = &api;
    check(api.create(nullptr, &instance) == ESM_RUNTIME_STATUS_INVALID_ARGUMENT && instance == nullptr);
    EsmModuleCreateV1 args{};
    check(api.create(&args, nullptr) == ESM_RUNTIME_STATUS_INVALID_ARGUMENT);
    instance = &api;
    check(api.create(&args, &instance) == ESM_RUNTIME_STATUS_INVALID_ARGUMENT && instance == nullptr);
    args.struct_size = sizeof(args); args.abi_major = 2;
    instance = &api;
    check(api.create(&args, &instance) == ESM_RUNTIME_STATUS_UNSUPPORTED_ABI && instance == nullptr);
    args.abi_major = 1;
    for (auto caps : {UINT64_C(0), ESM_RUNTIME_CAPABILITY_CLIENT_CONNECTION, ESM_RUNTIME_CAPABILITY_MASK_V1}) {
        args.capability_bits = caps; instance = &api;
        check(api.create(&args, &instance) == ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER && instance == nullptr);
    }
    check(api.start(nullptr) == ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER);
    check(api.session_begin(nullptr, nullptr) == ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER);
    check(api.session_end(nullptr, nullptr) == ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER);
    check(api.stop(nullptr) == ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER);
    check(api.destroy(nullptr) == ESM_RUNTIME_STATUS_UNSUPPORTED_ADAPTER);
    FreeLibrary(dll);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
