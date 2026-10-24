// Adapted from Entrodor Modloader Proxy, MIT. See MODLOADER-PROXY-LICENSE.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include "legacy_loader.hpp"
#include "dbghelp_forward.hpp"
#include "forward_ready.hpp"

namespace {

struct Config {
    bool log = false;
    bool deferred = false;
    DWORD delayMs = 0;
    bool loadFolder = false;
    std::wstring folder = L"mods";
    std::wstring forwardTo;  // optional proxy-mod DLL that receives the game's calls
    std::vector<std::wstring> mods;
    shroudweave::server::ForwardReady ready;
    unsigned forward_keys{}, deferred_keys{}, delay_keys{};
    bool ready_context_valid=true;
};

std::wstring g_dir;       // directory of this DLL, with trailing backslash
std::wstring g_selfName;  // lower-case file name of this DLL
bool g_log = false;
std::atomic<bool> g_attempted{};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    std::string line = Narrow(buf) + "\r\n";
    HANDLE h = CreateFileW((g_dir + L"shroudweave-chainloader.log").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
}

enum class ConfigRead { Absent, Loaded, Error };

ConfigRead ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? ConfigRead::Absent : ConfigRead::Error;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (1 << 20)) {
        CloseHandle(h);
        return ConfigRead::Error;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return ok != FALSE && static_cast<LONGLONG>(got) == sz.QuadPart
        ? ConfigRead::Loaded : ConfigRead::Error;
}

bool ParseBool(const std::wstring& v) {
    return v == L"1" || v == L"true" || v == L"yes" || v == L"on";
}

// Minimal INI parser: [Settings] has key=value, [Load] has one DLL path per line.
ConfigRead LoadConfig(Config& cfg) {
    std::string raw;
    const auto read = ReadWholeFile(g_dir + L"chainloader.ini", raw);
    if (read != ConfigRead::Loaded) return read;
    if (raw.find('\0') != std::string::npos) return ConfigRead::Error;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
        raw.erase(0, 3);
    std::wstring text = Widen(raw), section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t sc = line.find(L" ;");  // inline comment
        if (sc != std::wstring::npos) line = Trim(line.substr(0, sc));
        if (line.empty()) continue;

        if (section == L"settings") {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = Lower(Trim(line.substr(0, eq)));
            std::wstring value = Trim(line.substr(eq + 1));  // case preserved (folder names)
            std::wstring val = Lower(value);
            if (key == L"log") cfg.log = ParseBool(val);
            else if (key == L"deferred") {
                cfg.deferred = ParseBool(val);
                cfg.ready_context_valid = cfg.ready_context_valid && ++cfg.deferred_keys==1 &&
                    (val==L"0"||val==L"1"||val==L"true"||val==L"false"||val==L"yes"||val==L"no"||val==L"on"||val==L"off");
            }
            else if (key == L"delayms") {
                cfg.delayMs = (DWORD)_wtoi(val.c_str());
                cfg.ready_context_valid = cfg.ready_context_valid && ++cfg.delay_keys==1 && val==L"0";
            }
            else if (key == L"loadfromfolder") cfg.loadFolder = ParseBool(val);
            else if (key == L"modsfolder") cfg.folder = value;
            else if (key == L"forwardto") {
                cfg.forwardTo = value;
                cfg.ready_context_valid = cfg.ready_context_valid && ++cfg.forward_keys==1;
            }
            else if (key == L"forwardtoreadyexport") {
                cfg.ready.requested = true;
                cfg.ready.export_name = Narrow(value);
                ++cfg.ready.exports;
            }
            else if (key == L"forwardtoreadytimeoutms") {
                cfg.ready.requested = true;
                cfg.ready.valid = shroudweave::server::ready_timeout(value, cfg.ready.timeout_ms) && cfg.ready.valid;
                ++cfg.ready.timeouts;
            }
        } else if (section == L"load") {
            cfg.mods.push_back(line);
        }
    }
    return ConfigRead::Loaded;
}

// ---- real DLL forwarding ------------------------------------------------

bool BindRealDll(HMODULE fwd) {
    // Only MiniDumpWriteDump is imported by the supported Enshrouded profile.
    // Absent mod exports fall back to the system provider on first use.
    if (!fwd || !GetProcAddress(fwd, "MiniDumpWriteDump")) return true;
    return shroudweave::server::set_dbghelp_chain(fwd);
}

// ---- chainloading -------------------------------------------------------

std::wstring Resolve(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    bool absolute = (p.size() > 1 && p[1] == L':') || (p.size() > 1 && p[0] == L'\\' && p[1] == L'\\');
    return absolute ? p : g_dir + p;
}

// Non-recursive scan for *.dll, sorted by name (case-insensitive).
void ScanFolder(std::wstring dir, std::vector<std::wstring>& out) {
    if (dir.empty() || dir.back() != L'\\') dir += L'\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"No DLLs found in folder %ls (error %lu)", dir.c_str(), GetLastError());
        return;
    }
    std::vector<std::wstring> found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        // "*.dll" can also match longer extensions via 8.3 short names; require exactly .dll
        if (name.size() < 4 || Lower(name.substr(name.size() - 4)) != L".dll") continue;
        found.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(),
              [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
    for (const auto& n : found) out.push_back(dir + n);
}

bool LoadMods(const Config& cfg) {
    // Explicit [Load] entries first (in the order written), then the folder (alphabetical).
    std::vector<std::wstring> paths;
    for (const auto& e : cfg.mods) paths.push_back(Resolve(e));

    if (cfg.loadFolder) {
        std::wstring f = Trim(cfg.folder);
        for (auto& c : f) if (c == L'/') c = L'\\';
        while (!f.empty() && f.back() == L'\\') f.pop_back();
        if (f.empty() || f == L".") {
            Log(L"ModsFolder must be a subfolder, not the game folder itself; skipping folder load");
        } else {
            std::vector<std::wstring> scanned;
            ScanFolder(Resolve(f), scanned);
            for (const auto& p : scanned) {
                bool dup = false;
                for (const auto& q : paths) if (Lower(q) == Lower(p)) { dup = true; break; }
                if (!dup) paths.push_back(p);
            }
        }
    }

    std::wstring fwdPath = cfg.forwardTo.empty() ? std::wstring() : Lower(Resolve(cfg.forwardTo));
    for (const auto& path : paths) {
        size_t slash = path.find_last_of(L'\\');
        if (Lower(path.substr(slash + 1)) == g_selfName) continue;  // never load ourselves
        if (!fwdPath.empty() && Lower(path) == fwdPath) continue;   // already loaded as ForwardTo
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) Log(L"Loaded %ls", path.c_str());
        else {
            Log(L"FAILED to load %ls (error %lu)", path.c_str(), GetLastError());
            if (cfg.ready.requested) return false;
        }
    }
    return true;
}

// Loads the ForwardTo module early (before the game can call our exports).
HMODULE LoadForwardTarget(const std::wstring& entry) {
    std::wstring path = Resolve(entry);
    if (Lower(path) == Lower(g_dir + g_selfName)) {
        Log(L"ForwardTo points at the proxy itself; ignoring");
        return nullptr;
    }
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m) Log(L"ForwardTo module loaded: %ls", path.c_str());
    else Log(L"FAILED to load ForwardTo module %ls (error %lu)", path.c_str(), GetLastError());
    return m;
}

DWORD WINAPI DeferredThread(LPVOID p) {
    std::unique_ptr<Config> cfg(static_cast<Config*>(p));
    if (cfg->delayMs) Sleep(cfg->delayMs);
    LoadMods(*cfg);
    return 0;
}

bool Init(const std::filesystem::path& root, HMODULE self, bool require_synchronous) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    g_dir = root.wstring() + L"\\";
    g_selfName = Lower(full.substr(slash + 1));

    Config cfg;
    const auto config_read = LoadConfig(cfg);
    if (config_read == ConfigRead::Error) return false;
    if (require_synchronous && cfg.deferred) return false;
    g_log = cfg.log;
    if (config_read == ConfigRead::Absent) {
        // Absence belongs to the original open result. Never downgrade a read
        // or parse failure by querying a pathname that may have disappeared.
        Log(L"Optional chainloader.ini absent; using synchronous empty defaults");
    }
    if (cfg.ready.requested && (!cfg.ready.valid || !cfg.ready_context_valid ||
        cfg.ready.exports!=1 || cfg.ready.timeouts!=1 || cfg.forward_keys!=1 ||
        cfg.deferred_keys!=1 || cfg.delay_keys!=1 || cfg.deferred || cfg.delayMs ||
        cfg.forwardTo.empty() || !shroudweave::server::ready_export(cfg.ready.export_name))) {
        Log(L"READINESS REFUSED: explicit synchronous versioned wait configuration required");
        return false;
    }

    // First: the game may call our exports immediately.
    HMODULE fwd = cfg.forwardTo.empty() ? nullptr : LoadForwardTarget(cfg.forwardTo);
    if (!cfg.forwardTo.empty() && !fwd) return false;
    if (!BindRealDll(fwd)) return false;
    if (cfg.ready.requested) {
        const auto status=shroudweave::server::await_forward_ready(fwd, cfg.ready);
        if (status!=ERROR_SUCCESS) {
            Log(L"READINESS REFUSED: ForwardTo wait status=%lu; dependents withheld", status);
            return false;
        }
        Log(L"READINESS READY: ForwardTo initialization completed");
    }

    if (cfg.deferred) {
        auto* heap = new Config(cfg);
        HANDLE t = CreateThread(nullptr, 0, DeferredThread, heap, 0, nullptr);
        if (t) CloseHandle(t); else { delete heap; LoadMods(cfg); }
    } else {
        return LoadMods(cfg);
    }
    return true;
}

}  // namespace

namespace shroudweave::server {
bool initialize_legacy_loader(const std::filesystem::path& root, HMODULE self,
    bool require_synchronous) noexcept {
    if (g_attempted.exchange(true)) return false;
    try { return Init(root, self, require_synchronous); } catch (...) { return false; }
}
}
