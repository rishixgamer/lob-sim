#!/usr/bin/env python3
"""Time the lob-sim tests after the native binary and Python wheel are built."""

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from time import perf_counter


ROOT = Path(__file__).resolve().parents[1]
BUDGET_SECONDS = 30


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    python = str(args.python.absolute())
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    evidence = {
        "revision": revision,
        "architecture": platform.machine(),
        "os": platform.platform(),
        "runner_image": os.environ.get("ImageOS"),
        "runner_image_version": os.environ.get("ImageVersion"),
        "python": subprocess.check_output([python, "--version"], text=True).strip(),
        "numpy": subprocess.check_output(
            [python, "-c", "import numpy; print(numpy.__version__)"], text=True).strip(),
        "property_cases": os.environ.get("FAIRCROSS_PROPERTY_CASES", "1000"),
        "budget_seconds": BUDGET_SECONDS,
        "tests": {},
    }
    env = os.environ.copy()
    env.setdefault("FAIRCROSS_PROPERTY_CASES", evidence["property_cases"])
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    for name, command in (
        ("native", [str(args.native.absolute())]),
        ("python", [python, "-m", "unittest", "discover", "-s", "python/tests", "-v"]),
    ):
        start = perf_counter()
        result = subprocess.run(command, cwd=ROOT, env=env, check=False)
        evidence["tests"][name] = {
            "seconds": perf_counter() - start,
            "passed": result.returncode == 0,
        }

    evidence["total_seconds"] = sum(test["seconds"] for test in evidence["tests"].values())
    evidence["passed"] = (evidence["total_seconds"] < BUDGET_SECONDS and
                          all(test["passed"] for test in evidence["tests"].values()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Lob-sim test execution {evidence['total_seconds']:.3f} s "
          f"(budget {BUDGET_SECONDS} s), passed {evidence['passed']}")
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
