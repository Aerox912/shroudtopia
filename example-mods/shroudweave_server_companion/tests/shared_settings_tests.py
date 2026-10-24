import pathlib
import subprocess
import sys
import tempfile

exe, root = sys.argv[1:]
pathlib.Path(root).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="settings-", dir=root) as temp:
    subprocess.run([exe, str(pathlib.Path(temp) / "world")], check=True, timeout=15)
