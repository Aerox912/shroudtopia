# Loader regression tests

Configure this directory with CMake and MSVC x64, build Release, then run CTest.
The executable calls the same LoadAndRegisterMod implementation as the loader,
using isolated native DLL fixtures and a private temporary log. It does not load
Shroudtopia's DllMain, the game or any server.

Checks cover unchanged XHL-style DLLs over 10,000 polls, no extra log bytes,
size-only and timestamp-only replacements, valid registration exactly once,
late files, recoverable null factories, and removal/recreation.

API mismatches are cached per process by path, byte size and last-write time.
A replacement which deliberately preserves both size and timestamp requires a
game restart. Ordinary LoadLibrary/factory failures remain retryable. Existing
logs are preserved; this change removes the measured repeated-probe source and
does not introduce log rotation or change configured logging levels.
