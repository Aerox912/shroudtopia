#include <windows.h>
extern "C" __declspec(dllexport) __declspec(noinline) int residency_replacement(int value) { return value+100; }
extern "C" int detour_fixture_target(int);
#pragma comment(linker,"/EXPORT:detour_fixture_target")
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID) { return TRUE; }
