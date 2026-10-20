#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <memory_utils.h>

namespace Flight {
constexpr std::size_t Site = 0x22AFE1, Constant = 0x1400330;
inline uintptr_t FindTarget(const std::uint8_t* image) {
    if (!image) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 4096) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->FileHeader.TimeDateStamp != 1782724296 ||
        nt->OptionalHeader.SizeOfImage != 0x2DA7000) return 0;
    const std::uint8_t expected[] = {0xF3,0x0F,0x10,0x05,0x47,0x53,0x1D,0x01,0xF2,0x0F,0x11,0x4C,0x24,0x40,0x77,0x15};
    const float original = 0.0872664600610733f;
    if (std::memcmp(image + Site, expected, sizeof(expected)) || std::memcmp(image + Constant, &original, 4)) return 0;
    return reinterpret_cast<uintptr_t>(image + Site);
}
struct Toggle {
    bool on = false, armed = false;
    bool sample(bool down, bool focused) {
        if (!focused) {armed=false; return false;}
        if (!down) {armed=true; return false;}
        if (!armed) return false;
        armed=false;on=!on;return true;
    }
};
// Only aligned data changes while playing. The detour is installed once,
// initially reproducing the original constant; Off restores vanilla glide.
class Patch {
    Mem::Detour* hook = nullptr;
public:
    bool prepare(uintptr_t target) {
        if (!target || hook) return false;
        std::uint8_t code[] = {0xF3,0x0F,0x10,0x05,8,0,0,0, 0xE9,0,0,0,0, 0x90,0x90,0x90, 0,0,0,0};
        const float original=0.0872664600610733f;
        std::memcpy(code+16,&original,4);
        hook=new Mem::Detour(target,code,sizeof(code),false,3);
        const auto stub=hook->shellcode->data->address;
        const auto delta=static_cast<std::int64_t>(target+8)-static_cast<std::int64_t>(stub+13);
        const auto entry=static_cast<std::int64_t>(stub)-static_cast<std::int64_t>(target+5);
        if (!stub || delta<INT32_MIN || delta>INT32_MAX || entry<INT32_MIN || entry>INT32_MAX) {release();return false;}
        hook->shellcode->updateValue<std::int32_t>(9,static_cast<std::int32_t>(delta));
        return true;
    }
    bool install() {if(!hook)return false;if(hook->active)return true;const bool ok=hook->activate();FlushInstructionCache(GetCurrentProcess(),nullptr,0);return ok;}
    void enable(bool on) {
        if (!hook || !hook->active) return;
        const float value=on ? -1.57f : 0.0872664600610733f;
        LONG bits;std::memcpy(&bits,&value,4);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(hook->shellcode->data->address+16),bits);
    }
    void release() {
        if(!hook)return;
        hook->deactivate();FlushInstructionCache(GetCurrentProcess(),nullptr,0);
        const auto allocation=hook->shellcode->data->address;
        delete hook->patch;delete hook->shellcode;delete hook;hook=nullptr;
        if(allocation)VirtualFree(reinterpret_cast<void*>(allocation),0,MEM_RELEASE);
    }
    ~Patch(){release();}
};
}
