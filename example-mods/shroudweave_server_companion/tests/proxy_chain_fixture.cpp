#include <windows.h>

extern "C" __declspec(dllexport) BOOL WINAPI MiniDumpWriteDump(
    HANDLE, DWORD, HANDLE, unsigned, void*, void*, void*) {
    SetLastError(0x1234);
    return TRUE;
}
