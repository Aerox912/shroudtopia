#pragma once
#include <windows.h>
#include <dbghelp.h>

namespace shroudweave::server {
// Configure once, before the dedicated EXE enters main. The caller verifies
// the configured module before passing its handle.
bool set_dbghelp_chain(HMODULE module) noexcept;
BOOL WINAPI forward_minidump(HANDLE process, DWORD id, HANDLE file, MINIDUMP_TYPE type,
    PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION streams,
    PMINIDUMP_CALLBACK_INFORMATION callback) noexcept;
}
