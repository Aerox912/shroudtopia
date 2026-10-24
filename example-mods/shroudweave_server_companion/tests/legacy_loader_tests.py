import subprocess
import sys
import tempfile
import shutil
from pathlib import Path

exe, outer, fixture, base = map(Path, sys.argv[1:])
base.mkdir(parents=True, exist_ok=True)
for mode in ("sync", "deferred", "deferred-final", "failure"):
    with tempfile.TemporaryDirectory(dir=base) as tmp:
        root = Path(tmp)
        mods = root / "mods"
        mods.mkdir()
        for name in ("a.dll", "b.dll", "forward.dll"):
            shutil.copyfile(fixture, mods / name)
        forward = "missing.dll" if mode == "failure" else "mods/forward.dll"
        (root / "chainloader.ini").write_text(
            "[Settings]\nLog=true\nLoadFromFolder=true\nModsFolder=mods\n"
            + f"ForwardTo={forward}\nDeferred={str(mode in ('deferred', 'deferred-final')).lower()}\nDelayMs=200\n"
            + "[Load]\nmods/b.dll\n", encoding="utf-8")
        result = subprocess.run([str(exe), str(outer), str(root), mode], timeout=10)
        assert result.returncode == 0, (mode, result.returncode)
        if mode == "deferred-final":
            continue
        if mode != "failure":
            log = (root / "shroudweave-chainloader.log").read_text()
            loaded = [line for line in log.splitlines() if line.startswith("Loaded ")]
            assert len(loaded) == 2, loaded
            assert loaded[0].endswith("b.dll") and loaded[1].endswith("a.dll"), loaded
print("PASS: native explicit order, folder discovery, deduplication, delay, forwarding and failure")
