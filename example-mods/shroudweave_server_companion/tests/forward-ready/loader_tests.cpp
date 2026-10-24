// Includes the complete production loader. Only module mapping/export boundaries
// are modeled; configuration parsing and real caller/worker flow are not copied.
// No game, SDK production DLL or native mod is loaded.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>
#include <string>
#include <cstdio>
#include "../../entry/legacy_loader.hpp"
#include "../../entry/dbghelp_forward.hpp"

namespace fixture {
std::wstring scenario;
std::atomic<unsigned> waits{}, dependents{}, forwards{};
std::atomic<bool> ready{}, before_ready{};
DWORD caller{}, waiter{};
std::wstring config_path;
bool removed{};
unsigned char non_executable{};
DWORD WINAPI Await(DWORD timeout) {
 ++waits; waiter=GetCurrentThreadId();
 if(scenario==L"exception")RaiseException(0xe0000001,0,0,nullptr);
 if(scenario==L"failed")return ERROR_GEN_FAILURE;
 if(scenario==L"timeout"||scenario==L"retry-after-timeout")return WAIT_TIMEOUT;
 if(scenario==L"late-success")Sleep(timeout+30);
 if(scenario==L"delayed")Sleep(40);
 ready=true; return ERROR_SUCCESS;
}
FARPROC WINAPI Symbol(HMODULE module,LPCSTR name) {
 if(module!=GetModuleHandleW(nullptr))return nullptr;
 if(!std::strcmp(name,"MiniDumpWriteDump"))return nullptr;
 if(scenario==L"missing-export")return nullptr;
 if(scenario==L"foreign-export")return ::GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"Sleep");
 if(scenario==L"non-executable")return reinterpret_cast<FARPROC>(&non_executable);
 return reinterpret_cast<FARPROC>(&Await);
}
HMODULE WINAPI Map(LPCWSTR path,HANDLE,DWORD) {
 if(scenario==L"missing-module")return nullptr;
 if(scenario==L"missing-dependent"&&std::wstring(path).find(L"forward.dll")==std::wstring::npos)return nullptr;
 if(std::wstring(path).find(L"forward.dll")!=std::wstring::npos)++forwards;
 else {++dependents;if(!ready.load())before_ready=true;}
 return GetModuleHandleW(nullptr);
}
BOOL WINAPI Close(HANDLE handle) {
 const auto result=::CloseHandle(handle);
 if(scenario==L"nul-disappears")removed=DeleteFileW(config_path.c_str())!=FALSE;
 return result;
}
}
namespace shroudweave::server {bool set_dbghelp_chain(HMODULE) noexcept {return true;}}
#define LoadLibraryExW fixture::Map
#define GetProcAddress fixture::Symbol
#define CloseHandle fixture::Close
#include "../../entry/legacy_loader.cpp"
#undef LoadLibraryExW
#undef GetProcAddress
#undef CloseHandle

int wmain(int argc,wchar_t** argv) {
 if(argc!=3)return 2;
 fixture::scenario=argv[1];fixture::caller=GetCurrentThreadId();
 auto root=std::filesystem::path(argv[2])/fixture::scenario;
 fixture::config_path=(root/L"chainloader.ini").wstring();
 std::filesystem::create_directories(root);
 const bool deferred=fixture::scenario==L"deferred-ready"||fixture::scenario==L"final-deferred";
 std::string config="[Settings]\nLog=true\nForwardTo=forward.dll\nLoadFromFolder=false\nDelayMs=0\nDeferred=";
 config+=deferred?"true\n":"false\n";
 if(fixture::scenario!=L"missing-export-key")
  config+=fixture::scenario==L"invalid-export"?"ForwardToReadyExport=MiniDumpWriteDump\n":"ForwardToReadyExport=FixtureAwaitReadyV1\n";
 if(fixture::scenario!=L"missing-timeout-key"){
  std::string timeout="200";
  if(fixture::scenario==L"bad-timeout")timeout="12junk";
  if(fixture::scenario==L"zero-timeout")timeout="0";
  if(fixture::scenario==L"excessive-timeout")timeout="60001";
  if(fixture::scenario==L"late-success"||fixture::scenario==L"timeout")timeout="20";
  config+="ForwardToReadyTimeoutMs="+timeout+"\n";
 }
 if(fixture::scenario==L"duplicate-export")config+="ForwardToReadyExport=FixtureAwaitReadyV1\n";
 config+="[Load]\ndependent.dll\n";
 if(fixture::scenario==L"nul-config"||fixture::scenario==L"nul-disappears")config.push_back('\0');
 HANDLE locked=INVALID_HANDLE_VALUE;
 if(fixture::scenario!=L"missing-config"){
  std::ofstream output(root/L"chainloader.ini",std::ios::binary);output<<config;
 }
 if(fixture::scenario==L"unreadable-config"){
  locked=CreateFileW((root/L"chainloader.ini").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
  if(locked==INVALID_HANDLE_VALUE)return 3;
 }
 const bool ok=shroudweave::server::initialize_legacy_loader(root,GetModuleHandleW(nullptr),fixture::scenario==L"final-deferred");
 if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
 // A failed readiness result must never release late dependents/callbacks.
 const bool final_callback=ok;
 if(fixture::scenario==L"timeout")fixture::ready=true;
 bool retried{};
 if(fixture::scenario==L"retry-after-timeout"){
  fixture::ready=true;fixture::scenario=L"ready";
  retried=shroudweave::server::initialize_legacy_loader(root,GetModuleHandleW(nullptr),false);
  fixture::scenario=L"retry-after-timeout";
 }
 Sleep(80);
 const bool expected_ready=fixture::scenario==L"ready"||fixture::scenario==L"delayed";
 const bool expected=expected_ready||fixture::scenario==L"missing-config";
 const bool passed=(fixture::scenario!=L"nul-disappears"||fixture::removed) &&
  !retried && ok==expected && final_callback==expected && !fixture::before_ready &&
  fixture::dependents==(expected_ready?1u:0u) &&
  (!expected_ready||(fixture::waits==1&&fixture::waiter==fixture::caller));
 if(!passed){std::fwprintf(stderr,L"FAIL %ls ok=%u waits=%u dependents=%u before=%u\n",argv[1],ok?1u:0u,fixture::waits.load(),fixture::dependents.load(),fixture::before_ready?1u:0u);return 1;}
 std::wprintf(L"PASS %ls: exact production loader consumer; no native DLL/game execution\n",argv[1]);return 0;
}
