"""Direct-launch synthetic fixtures only; never execute the server input."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time


def launch(exe, cwd, env, debugging):
    if not debugging:
        return subprocess.run([str(exe)], cwd=cwd, env=env, capture_output=True, timeout=15)
    process = subprocess.Popen([str(exe)], cwd=cwd, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, creationflags=2)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    event = ctypes.create_string_buffer(192)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if not kernel.WaitForDebugEvent(ctypes.byref(event), 1000):
            continue
        code, pid, tid = struct.unpack_from("<III", event.raw)
        status = 0x10002
        if code == 1 and struct.unpack_from("<I", event.raw, 16)[0] != 0x80000003:
            status = 0x80010001
        assert kernel.ContinueDebugEvent(pid, tid, status), ctypes.get_last_error()
        if code == 5:
            out, err = process.communicate(timeout=5)
            return subprocess.CompletedProcess(process.args, process.returncode, out, err)
    process.kill()
    raise AssertionError("synthetic debugger case timed out")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("case")
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--proxy", type=Path)
    parser.add_argument("--owner", type=Path)
    args = parser.parse_args()
    native_cases = {
        "native-no-commit", "native-call", "native-mismatch", "native-foreign-owner",
        "native-private-site", "native-invalid-target", "native-bootstrap-site",
        "native-capacity", "native-relay", "native-bad-relay", "native-overlap",
        "native-site-tamper", "native-relay-tamper",
    }
    if args.case in native_cases and not args.owner:
        parser.error("native bridge cases require --owner with the synthetic callback DLL")
    with tempfile.TemporaryDirectory(prefix="entry gate ", dir=args.fixture.parent) as temp:
        root = Path(temp)
        exe = root / "synthetic entry.exe"
        shutil.copy2(args.fixture, exe)
        if args.proxy:
            shutil.copy2(args.proxy, root / "dbghelp.dll")
        if args.owner:
            shutil.copy2(args.owner, root / "shroudweave_startup_owner.dll")
        if args.case in ("mismatch", "foreign-byte"):
            data = bytearray(exe.read_bytes())
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            entry = struct.unpack_from("<I", data, pe + 40)[0]
            if args.case == "mismatch":
                struct.pack_into("<I", data, pe + 40, entry + 1)
            else:
                count = struct.unpack_from("<H", data, pe + 6)[0]
                optional_size = struct.unpack_from("<H", data, pe + 20)[0]
                for i in range(count):
                    section = pe + 24 + optional_size + i * 40
                    virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, section + 8)
                    if rva <= entry < rva + min(virtual_size, raw_size):
                        data[raw + entry - rva] = 0xCC
                        break
                else:
                    raise AssertionError("entry is not in file-backed section")
            exe.write_bytes(data)
        env = dict(os.environ, ESM_ENTRY_FIXTURE=args.case, ESM_DEDICATED_CASE=args.case)
        result = launch(exe, root.parent, env, args.case == "debugger")
        lines = result.stdout.decode("ascii", errors="replace").splitlines()
        print(f"{args.case}: exit={result.returncode}, markers={lines}, diagnostics={result.stderr!r}")
        if args.case in native_cases:
            successful = {
                "native-no-commit": ["READY", "ENTRY", "NATIVE_READY"],
                "native-call": ["READY", "ENTRY", "NATIVE_CALL"],
                "native-relay": ["READY", "ENTRY", "NATIVE_RELAY"],
            }
            refused_markers = {
                "native-capacity": ["NATIVE_CAPACITY_64"],
                "native-site-tamper": ["NATIVE_SITE_TAMPER"],
                "native-relay-tamper": ["NATIVE_RELAY_TAMPER"],
            }
            if args.case in successful:
                assert result.returncode == 0 and lines == successful[args.case], \
                    "bridge must verify the generic host view, retain the patch/owner, and release only ready startup"
            else:
                expected = refused_markers.get(args.case, ["NATIVE_REJECTED"])
                assert result.returncode == 0xE5340001 and lines == expected, \
                    "native bridge must refuse the intended invalid, over-capacity, or tampered commitment before entry"
        elif args.case in ("ready", "forward", "xstate", "unrelated"):
            expected = ["INIT"] + (["UNRELATED"] if args.case == "unrelated" else []) + ["UNWIND"] + (["FORWARD"] if args.case == "forward" else []) + ["READY", "ENTRY", "CONTEXT", "NETWORK"]
            assert result.returncode == 0 and lines == expected, "gate must preserve context and publish READY before original entry exactly once"
        elif args.case == "dynamic":
            assert result.returncode != 0 and lines == ["ENTRY", "REJECTED"], "late dynamic load must reject; entry already happened"
        else:
            expected = ["INIT"] if args.case in ("failed", "wrong-thread") else []
            assert result.returncode == 0xE5340001 and lines == expected, "controlled bootstrap refusal must occur at the intended boundary before entry/TLS/network"


if __name__ == "__main__":
    main()
