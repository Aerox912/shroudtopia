"""Run only the synthetic client fixture under a server filename, never a game executable."""
import pathlib
import subprocess
import sys

fixture = pathlib.Path(sys.argv[1])
proxy = pathlib.Path(sys.argv[2])
folder = pathlib.Path(sys.argv[3])
if fixture.name != "shroudweave_client_passthrough_fixture.exe" or proxy.name != "dbghelp.dll" or folder.name != "production-refusal-fixture":
    raise SystemExit("fixture identity mismatch")
folder.mkdir(parents=True, exist_ok=True)
(folder / "enshrouded_server.exe").write_bytes(fixture.read_bytes())
(folder / "dbghelp.dll").write_bytes(proxy.read_bytes())
result = subprocess.run([str(folder / "enshrouded_server.exe")], cwd=folder,
                        capture_output=True, timeout=10, check=False)
if result.returncode & 0xffffffff != 0xE5340001 or result.stdout:
    raise SystemExit(f"expected early profile refusal, got exit={result.returncode} stdout={result.stdout!r}")
print("PASS: unsupported server-named synthetic EXE refused before main")