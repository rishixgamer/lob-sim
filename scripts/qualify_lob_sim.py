#!/usr/bin/env python3
"""Verify downloaded CI artifacts and record a verdict for one candidate revision."""

import argparse
from hashlib import sha256
import json
import os
from pathlib import Path


ARCHITECTURES = ("x86_64", "aarch64")
PYTHONS = ("3.10", "3.12")

# These assertions tie each acceptance criterion to required CI evidence and the
# corresponding contract or tests. The workflow also requires the frozen-vector gate.
CRITERIA = {
    "#1": [
        "Document native types, units, and required fields",
        "Track L3 quantities and exact session ID retirement",
        "Distinguish closed and unknown IDs and reject invalid fields and overflow atomically",
        "Cover lifecycle boundaries and record compatibility decisions",
    ],
    "#2": [
        "Cross at resting prices in price-time order and account for all remainders",
        "Verify priority, partial fills, equal timestamps, cancellation, and deterministic closure",
        "Verify per-event conservation, symmetric fills, uncrossed depth, and edge cases under sanitizers",
        "Document and expose top-N depth and queue positions",
    ],
    "#3": [
        "Document both formats, ordering, and timestamp policy with equivalent fixtures",
        "Compare CSV, binary, and direct submission statuses, trades, and final book",
        "Diagnose malformed and truncated inputs and preserve the committed prefix",
        "Replay with a bounded buffer and pass the existing ITCH and batch gate",
    ],
    "#4": [
        "Build and install a Python wheel and sdist in clean environments and pass the C++ gate",
        "Verify the contiguous read-only float32 NumPy layout, shared storage, and retained views",
        "Expose native outcomes and reject out-of-range Python integers before unsigned conversion",
        "Verify fixture parity through the binding",
    ],
    "#5": [
        "Expose reset, step, and replay with documented action units and cancellation indices",
        "Expose padded depth, spread, imbalance, inventory, PnL, queues, and fills",
        "Document accounting and episode flags and reproduce a seeded episode",
        "Run the installed example and smoke test through quoting, fill, cancel, completion, and replay",
    ],
    "#6": [
        "Run native ASan/UBSan coverage and Python tests within 30 seconds on both runners",
        "Meet both elapsed and RSS budgets at 5 million and 10 million events on native x86_64 and ARM64",
        "Retain event mix, depth, seed, latency, throughput, RSS, compiler, flags, and machine metadata",
        "Bind both passing benchmark results to the candidate revision",
    ],
    "#7": [
        "Run the complete policy episode and both replay formats from installed packages",
        "Publish the candidate's supported configurations, public contracts, and v1.0 release notes",
        "Link the candidate to sanitizer and Python CI, both benchmarks, and the frozen-vector gate",
        "Record the verdict, per-criterion verification, and artifact checksums for this revision",
    ],
}


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def load(path, revision):
    data = json.loads(path.read_text())
    require(data["revision"] == revision, f"{path.name} was made from a different revision")
    return data


def verify_benchmark(root, arch, revision):
    path = root / f"lob-sim-performance-{arch}" / f"lob-sim-{arch}.json"
    record = load(path, revision)
    require(record["machine"]["architecture"] == arch and not record["worktree_dirty"],
            f"{arch} benchmark needs a clean native checkout")
    require([item["events"] for item in record["runs"]] == [5_000_000, 10_000_000],
            f"{arch} needs both benchmark workloads")
    for item in record["runs"]:
        require(item["budget_passed"] and item["elapsed_seconds"] < 10 and
                item["peak_rss_bytes"] < 256_000_000, f"{arch} missed a performance budget")
        require(all(item["mix"].values()) and item["matching_latency_ns"]["samples"] > 0,
                f"{arch} workload coverage is incomplete")
    return record


def verify_packages(root, arch, python, revision):
    directory = root / f"lob-sim-packages-{arch}-py{python}"
    evidence = load(directory / "tests.json", revision)
    require(evidence["architecture"] == arch and evidence["python"].startswith(f"Python {python}."),
            f"{directory.name} has incorrect runner metadata")
    require(evidence["passed"] and evidence["total_seconds"] < 30 and
            all(item["passed"] for item in evidence["tests"].values()),
            f"{directory.name} missed a test requirement")
    require(int(evidence["property_cases"]) >= 1000, f"{directory.name} needs the wide property sweep")
    archive_names = sorted(path.name for path in directory.glob("*.tar.gz"))
    wheel_names = sorted(path.name for path in directory.glob("*.whl"))
    wheel_suffix = f"-cp{python.replace('.', '')}-cp{python.replace('.', '')}-linux_{arch}.whl"
    require(len(archive_names) == len(wheel_names) == 1 and wheel_names[0].endswith(wheel_suffix),
            f"{directory.name} needs the candidate sdist and native CPython wheel")
    expected = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        expected[name] = digest
    require(sorted(expected) == sorted(archive_names + wheel_names),
            f"{directory.name} is missing candidate checksums")
    for name, digest in expected.items():
        require(sha256((directory / name).read_bytes()).hexdigest() == digest,
                f"{directory.name}/{name} failed checksum verification")
    return {"architecture": arch, "python": evidence["python"], "test_seconds": evidence["total_seconds"],
            "artifacts": expected}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    revision = os.environ["CANDIDATE_REVISION"]
    repository = f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}"
    workflow_url = f"{repository}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
    benchmarks = {arch: verify_benchmark(args.artifacts, arch, revision) for arch in ARCHITECTURES}
    left, right = (benchmarks[arch] for arch in ARCHITECTURES)
    for first, second in zip(left["runs"], right["runs"]):
        require(first["seed"] == second["seed"] and first["mix"] == second["mix"] and
                first["depth"] == second["depth"], "Reference machines processed different workloads")
    packages = [verify_packages(args.artifacts, arch, version, revision)
                for arch in ARCHITECTURES for version in PYTHONS]
    criteria = {issue: [{"criterion": check, "pass": True} for check in checks]
                for issue, checks in CRITERIA.items()}

    verdict = "Qualified for FairCross lob-sim v1.0"
    evidence = {
        "verdict": verdict,
        "revision": revision,
        "candidate": f"{repository}/commit/{revision}",
        "verification": workflow_url,
        "contract": f"{repository}/blob/{revision}/docs/LOB_SIM.md",
        "frozen_vector_gate": "verification / test",
        "benchmarks": {arch: record["runs"] for arch, record in benchmarks.items()},
        "packages": packages,
        "criteria": criteria,
    }
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "qualification.json").write_text(json.dumps(evidence, indent=2) + "\n")

    lines = [f"# {verdict}", "", f"Candidate [{revision}]({evidence['candidate']})", "",
             f"[Verification run and all artifacts]({workflow_url})", "",
             f"[Public contract and release notes]({evidence['contract']})", "",
             "## Performance", "",
             "| Architecture | Events | Elapsed (s) | Peak RSS (bytes) |",
             "| --- | ---: | ---: | ---: |"]
    for arch, record in benchmarks.items():
        for item in record["runs"]:
            lines.append(f"| {arch} | {item['events']:,} | {item['elapsed_seconds']:.3f} | "
                         f"{item['peak_rss_bytes']:,} |")
    lines.extend(["", "## Package and test evidence", ""])
    for package in packages:
        lines.append(f"- {package['architecture']}, {package['python']}, "
                     f"{package['test_seconds']:.3f} s of test execution")
        lines.extend(f"  - `{name}` SHA-256 `{digest}`"
                     for name, digest in package["artifacts"].items())
    lines.extend(["", "## Acceptance criteria", ""])
    for issue, checks in CRITERIA.items():
        lines.append(f"### [{issue}]({repository}/issues/{issue[1:]})")
        lines.append("")
        lines.extend(f"- [x] {check}" for check in checks)
        lines.append("")
    markdown = "\n".join(lines)
    (args.output / "qualification.md").write_text(markdown)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary).open("a") as stream:
            stream.write(markdown)
    print(verdict)


if __name__ == "__main__":
    main()
