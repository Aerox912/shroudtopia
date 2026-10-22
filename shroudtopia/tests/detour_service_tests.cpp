#include "detour_service.h"
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <array>
extern "C" int detour_fixture_target(int);
using Function=int(*)(int);
Function original;
__declspec(noinline) int replacement(int value) { return original(value)+100; }
int main(int argc,char** argv) {
 if(argc<2)return 2;
 void* trampoline=reinterpret_cast<void*>(1);
 if(std::strcmp(argv[1],"residency")==0) {
  if(argc!=3)return 13;
  auto module=LoadLibraryA(argv[2]);if(!module)return 14;
  auto target=GetProcAddress(module,"detour_fixture_target");auto detour=GetProcAddress(module,"residency_replacement");
  if(!target||!detour||!ModContext_CreateDetour(reinterpret_cast<void*>(target),reinterpret_cast<void*>(detour),&trampoline)||!trampoline)return 15;
  auto callable=reinterpret_cast<Function>(target);auto baseline=reinterpret_cast<Function>(trampoline);
  if(callable(10)!=110||baseline(10)!=17)return 16;
  FreeLibrary(module);HMODULE retained{};
  if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(target),&retained)||retained!=module||callable(10)!=110||baseline(10)!=17)return 17;
  std::puts("PASS target/detour DLL stays resident after caller FreeLibrary");return 0;
 }
 if(std::strcmp(argv[1],"invalid")==0) {
  unsigned char data[32]{};
  if(ModContext_CreateDetour(data,reinterpret_cast<void*>(&replacement),&trampoline)||trampoline)return 3;
  if(ModContext_CreateDetour(reinterpret_cast<void*>(&detour_fixture_target),reinterpret_cast<void*>(&replacement),nullptr))return 4;
  std::puts("PASS invalid/nonimage target and null trampoline output refused");return 0;
 }
 if(std::strcmp(argv[1],"foreign")==0) {
  if(MH_Initialize()!=MH_OK)return 5;
  if(MH_CreateHook(reinterpret_cast<void*>(&detour_fixture_target),reinterpret_cast<void*>(&replacement),&trampoline)!=MH_OK)return 6;
  trampoline=nullptr;
  if(ModContext_CreateDetour(reinterpret_cast<void*>(&detour_fixture_target),reinterpret_cast<void*>(&replacement),&trampoline)||trampoline||detour_fixture_target(10)!=17)return 7;
  std::puts("PASS foreign initialized MinHook refused without enabling its hooks");return 0;
 }
 if(std::strcmp(argv[1],"concurrent")==0) {
  std::array<std::thread,8> threads;std::array<void*,8> outputs{};std::array<bool,8> results{};
  for(unsigned i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{results[i]=ModContext_CreateDetour(reinterpret_cast<void*>(&detour_fixture_target),reinterpret_cast<void*>(&replacement),&outputs[i]);});
  for(auto& thread:threads)thread.join();unsigned winners{};
  for(unsigned i=0;i<results.size();++i)if(results[i]){++winners;original=reinterpret_cast<Function>(outputs[i]);}else if(outputs[i])return 8;
  if(winners!=1||!original||original(10)!=17||detour_fixture_target(10)!=117)return 9;
  std::puts("PASS one concurrent owner and actual trampoline");return 0;
 }
 if(!ModContext_CreateDetour(reinterpret_cast<void*>(&detour_fixture_target),reinterpret_cast<void*>(&replacement),&trampoline)||!trampoline)return 10;
 original=reinterpret_cast<Function>(trampoline);
 if(original(10)!=17||detour_fixture_target(10)!=117)return 11;
 trampoline=reinterpret_cast<void*>(1);
 if(ModContext_CreateDetour(reinterpret_cast<void*>(&detour_fixture_target),reinterpret_cast<void*>(&replacement),&trampoline)||trampoline||detour_fixture_target(10)!=117)return 12;
 std::puts("PASS real public callback, trampoline execution and duplicate refusal");return 0;
}
