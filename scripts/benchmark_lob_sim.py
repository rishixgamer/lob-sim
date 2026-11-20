#!/usr/bin/env python3
"""Build the standalone release benchmark and record both qualification workloads."""

import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SEED = 20261120
BUDGET = {"elapsed_seconds_lt": 10, "peak_rss_bytes_lt": 256_000_000}


def output(*command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="cpp/build/lob-release", type=Path)
    parser.add_argument("--output-dir", default="cpp/build/lob-evidence", type=Path)
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    configure = ["cmake", "-S", "cpp", "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release",
                 "-DFAIRCROSS_ENABLE_ASAN=OFF"]
    build = ["cmake", "--build", str(build_dir), "--target", "faircross_lob_bench", "-j", "2"]
    for command in (configure, build):
        subprocess.run(command, cwd=ROOT, check=True)

    entries = json.loads((build_dir / "compile_commands.json").read_text())
    sources = [entry for entry in entries if "/continuous/" in entry["file"]]
    compilation = {str(Path(entry["file"]).relative_to(ROOT)): entry["command"] for entry in sources}
    compiler = shlex.split(sources[0]["command"])[0]
    machine = {
        "architecture": platform.machine(),
        "os": platform.platform(),
        "distribution": platform.freedesktop_os_release().get("PRETTY_NAME"),
        "cpu": json.loads(output("lscpu", "--json"))["lscpu"],
        "affinity_cpu_count": len(os.sched_getaffinity(0)),
        "runner_environment": os.environ.get("RUNNER_ENVIRONMENT"),
        "runner_image": os.environ.get("ImageOS"),
        "runner_image_version": os.environ.get("ImageVersion"),
    }
    records = {
        "revision": output("git", "rev-parse", "HEAD"),
        "worktree_dirty": bool(output("git", "status", "--porcelain")),
        "workflow_run": os.environ.get("GITHUB_RUN_ID"),
        "machine": machine,
        "build": {"compiler": output(compiler, "--version"), "compile_commands": compilation,
                  "configure_command": configure, "build_command": build},
        "budget": BUDGET,
        "runs": [],
    }
    for events in (5_000_000, 10_000_000):
        command = [str(build_dir / "faircross_lob_bench"), str(events), str(SEED)]
        record = json.loads(output(*command))
        record["run_command"] = command
        record["budget_passed"] = (record["elapsed_seconds"] < BUDGET["elapsed_seconds_lt"] and
                                   record["peak_rss_bytes"] < BUDGET["peak_rss_bytes_lt"])
        records["runs"].append(record)

    destination = output_dir / f"lob-sim-{machine['architecture']}.json"
    destination.write_text(json.dumps(records, indent=2) + "\n")
    for run in records["runs"]:
        print(f"{run['events']:,} events in {run['elapsed_seconds']:.3f} s, "
              f"peak RSS {run['peak_rss_bytes']:,} bytes, budget passed {run['budget_passed']}")
    print(destination)
    return 0 if all(run["budget_passed"] for run in records["runs"]) else 1


if __name__ == "__main__":
    sys.exit(main())
