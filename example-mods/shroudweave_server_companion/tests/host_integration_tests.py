"""Synthetic host-entry integration tests; all artifacts live in a temporary root."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit("usage: host_integration_tests.py <test-exe> <proxy-dll> <module-dll> <case>")
    test_exe, proxy_source, module_source = map(Path, sys.argv[1:4])
    case = sys.argv[4]
    optional_absent = case == "optional-absent"
    optional_failure = case == "optional-failure"
    missing_required = case == "missingrequired"
    two_module_order = case == "final-two-module-order"
    with tempfile.TemporaryDirectory(prefix="shroudweave-host-smoke-") as temporary:
        root = Path(temporary)
        proxy = root / "dbghelp.dll"
        module = root / "shroudweave_alpha.dll"
        shutil.copy2(proxy_source, proxy)
        if case not in ("optional-absent", "wrongfile", "missingrequired"):
            shutil.copy2(module_source, module)
        if two_module_order:
            shutil.copy2(module_source, root / "shroudweave_beta.dll")
        modules = []
        required = ["missing_module"] if missing_required else []
        if two_module_order:
            module_digest = sha256(module_source)
            modules.extend([
                {"id": "beta", "file": "shroudweave_beta.dll",
                 "sha256": module_digest, "required": True},
                {"id": "alpha", "file": "shroudweave_alpha.dll",
                 "sha256": module_digest, "required": True},
            ])
            required = ["beta", "alpha"]
        elif case != "missingrequired":
            module_file = "wrong_module.dll" if case == "wrongfile" else "shroudweave_alpha.dll"
            feature_required = case in (
                "success", "required-failure", "badsha", "wrongfile",
                "final-foreign-root-registration",
            )
            module_digest = "0" * 64 if case == "badsha" else sha256(module_source)
            modules.append({
                "id": "alpha",
                "file": module_file,
                "sha256": module_digest,
                "required": feature_required,
            })
            if feature_required:
                required.append("alpha")
        (root / "shroudweave-server.json").write_text(json.dumps({
            "schema": 1,
            "requiredModules": required,
            "modules": modules,
        }, separators=(",", ":")), encoding="utf-8")
        (root / "shroudweave-services.json").write_text(json.dumps({
            "schema": 1,
            "state_id": "host-smoke",
        }, separators=(",", ":")), encoding="utf-8")
        environment = os.environ.copy()
        environment["SHROUDWEAVE_HOST_TEST_CASE"] = case
        result = subprocess.run([str(test_exe), case, str(root)], env=environment,
                                capture_output=True, text=True, timeout=30, check=False)
        if result.returncode:
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            audit = root / "shroudweave-server.log"
            if audit.is_file():
                print("HOST AUDIT:")
                print(audit.read_text(encoding="utf-8", errors="replace"), end="")
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
