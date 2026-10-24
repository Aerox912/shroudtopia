import hashlib
import subprocess
import sys
from pathlib import Path


def main() -> int:
    test, outer, flight, sample, root = map(Path, sys.argv[1:])
    result = subprocess.run(
        [
            str(test),
            str(outer),
            hashlib.sha256(flight.read_bytes()).hexdigest(),
            hashlib.sha256(sample.read_bytes()).hexdigest(),
            str(root),
        ],
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    if result.returncode:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
