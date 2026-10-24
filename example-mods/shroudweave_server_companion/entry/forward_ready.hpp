// Generic versioned startup wait contract. Modloader Proxy adaptation, MIT;
// see MODLOADER-PROXY-LICENSE. Feature names and payload hashes stay in profiles.
#pragma once
#include <windows.h>
#include <string>

namespace shroudweave::server {
struct ForwardReady {
    bool requested{}, valid=true;
    unsigned exports{}, timeouts{};
    DWORD timeout_ms{};
    std::string export_name;
};
inline bool ready_timeout(const std::wstring& text, DWORD& timeout) noexcept {
    if (text.empty() || text.size() > 5) return false;
    DWORD value{};
    for (const auto ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        value = value * 10 + static_cast<DWORD>(ch - L'0');
    }
    if (!value || value > 60000) return false;
    timeout = value; return true;
}
inline bool ready_export(const std::string& name) noexcept {
    if (name.size() < 3 || name.size() > 127 || name.compare(name.size()-2, 2, "V1") != 0) return false;
    for (std::size_t i=0; i<name.size(); ++i) {
        const auto ch=name[i];
        if (!((ch>='A'&&ch<='Z') || (ch>='a'&&ch<='z') || ch=='_' ||
              (i && ch>='0'&&ch<='9'))) return false;
    }
    return true;
}
using ForwardWaitV1 = DWORD(WINAPI*)(DWORD);
inline DWORD call_ready_no_unwind(ForwardWaitV1 function, DWORD timeout) noexcept {
    __try { return function(timeout); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return ERROR_UNHANDLED_EXCEPTION; }
}
// Called only by the SDK's outside-DllMain startup caller. Never unload/retry:
// failure or timeout does not cancel the child's worker or undo its patches.
inline DWORD await_forward_ready(HMODULE module, const ForwardReady& gate) noexcept {
    if (!module) return ERROR_MOD_NOT_FOUND;
    if (!gate.valid || gate.exports!=1 || gate.timeouts!=1 ||
        !ready_export(gate.export_name) || !gate.timeout_ms || gate.timeout_ms>60000)
        return ERROR_INVALID_PARAMETER;
    const auto symbol=GetProcAddress(module, gate.export_name.c_str());
    if (!symbol) return ERROR_PROC_NOT_FOUND;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(symbol), &info, sizeof(info)) ||
        info.AllocationBase!=module || info.Type!=MEM_IMAGE || info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        (info.Protect&0xff)!=PAGE_EXECUTE_READ) return ERROR_INVALID_ADDRESS;
    const auto started=GetTickCount64();
    const auto result=call_ready_no_unwind(reinterpret_cast<ForwardWaitV1>(symbol), gate.timeout_ms);
    if (GetTickCount64()-started>gate.timeout_ms) return WAIT_TIMEOUT;
    return result; // Only ERROR_SUCCESS admits dependent loads.
}
}
