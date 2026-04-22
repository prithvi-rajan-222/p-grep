#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
P_GREP = ROOT / "bin" / "p-grep"
DEFAULT_PATTERNS = ROOT / "patterns" / "code-search.txt"
JOB_COUNTS = [4, 8, 16]


def parse_key_values(line):
    values = {}
    for part in line.split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        values[key] = value
    return values


def parse_number(value):
    try:
        if "." in value:
            return float(value)
        return int(value)
    except ValueError:
        return value


def parse_pgrep_timing(stderr):
    metrics = {}
    max_worker_ms = 0.0
    for line in stderr.splitlines():
        values = {key: parse_number(value) for key, value in parse_key_values(line).items()}
        if not values:
            continue
        if any(key in values for key in ("patterns", "search_ms", "work_units")):
            metrics.update(values)
        if "elapsed_ms" in values and (
            "threads_worker" in values or "processes_worker" in values or "single_worker" in values
        ):
            max_worker_ms = max(max_worker_ms, float(values["elapsed_ms"]))
    metrics["max_worker_ms"] = max_worker_ms
    return metrics


def run_capture(command):
    start = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    wall_ms = (time.perf_counter() - start) * 1000
    return completed, wall_ms


def run_devnull(command):
    start = time.perf_counter()
    subprocess.run(
        command,
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    return (time.perf_counter() - start) * 1000


def run_rg_process(command, stdout):
    completed = subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        text=True,
        stdout=stdout,
        stderr=subprocess.PIPE,
    )
    if completed.returncode not in (0, 1):
        raise subprocess.CalledProcessError(
            completed.returncode,
            command,
            output=None if stdout is not subprocess.PIPE else completed.stdout,
            stderr=completed.stderr,
        )
    return completed


def average(samples, key):
    values = [float(sample.get(key, 0.0)) for sample in samples]
    return sum(values) / len(values) if values else 0.0


def average_int(samples, key):
    values = [int(sample.get(key, 0)) for sample in samples]
    return round(sum(values) / len(values)) if values else 0


def pgrep_command(patterns, path, mode, jobs=None, output=None):
    command = [
        str(P_GREP),
        "--patterns",
        str(patterns),
        "--path",
        str(path),
        "--mode",
        mode,
        "--timing",
    ]
    if jobs is not None:
        command += ["--jobs", str(jobs)]
    if output is not None:
        command += ["--output", str(output)]
    return command


def run_pgrep_case(name, command_factory, repeats):
    samples = []
    baseline_matches = None
    for _ in range(repeats):
        command = command_factory()
        output_path = None
        if "--output" in command:
            output_path = Path(command[command.index("--output") + 1])
        try:
            completed, wall_ms = run_capture(command)
        finally:
            if output_path is not None:
                output_path.unlink(missing_ok=True)
        metrics = parse_pgrep_timing(completed.stderr)
        metrics["wall_ms"] = wall_ms
        try:
            match_count = int(completed.stdout.strip())
        except ValueError as exc:
            raise RuntimeError(f"{name} did not print a numeric count: {completed.stdout!r}") from exc
        metrics["output_lines"] = match_count
        if baseline_matches is None:
            baseline_matches = match_count
        elif match_count != baseline_matches:
            raise RuntimeError(f"{name} produced inconsistent counts: {match_count} != {baseline_matches}")
        samples.append(metrics)
    return {
        "name": name,
        "samples": samples,
        "lines": baseline_matches or 0,
        "files": average_int(samples, "files"),
        "bytes": average_int(samples, "bytes"),
        "matched_lines": average_int(samples, "matched_lines"),
    }


def run_rg(patterns, path, repeats, output_enabled=False):
    rg = shutil.which("rg")
    if not rg:
        return None

    command = [rg, "-F", "-n", "-f", str(patterns), str(path)]
    if output_enabled:
        samples = []
        lines = None
        for _ in range(repeats):
            fd, output_path = tempfile.mkstemp(prefix="p-grep-rg-", suffix=".txt")
            os.close(fd)
            try:
                start = time.perf_counter()
                with open(output_path, "w", encoding="utf-8") as out:
                    run_rg_process(command, out)
                wall_ms = (time.perf_counter() - start) * 1000
                samples.append(wall_ms)
                with open(output_path, "rb") as out:
                    line_count = out.read().count(b"\n")
                if lines is None:
                    lines = line_count
            finally:
                Path(output_path).unlink(missing_ok=True)
    else:
        start = time.perf_counter()
        completed = run_rg_process(command, subprocess.PIPE)
        wall_ms = (time.perf_counter() - start) * 1000
        lines = completed.stdout.count("\n")
        samples = [wall_ms]
        for _ in range(repeats - 1):
            start = time.perf_counter()
            run_rg_process(command, subprocess.DEVNULL)
            samples.append((time.perf_counter() - start) * 1000)
    return {
        "name": "rg -F -n",
        "avg_wall_ms": sum(samples) / len(samples),
        "lines": lines,
    }


def print_pgrep_table(results):
    print("p-grep averages")
    header = (
        f"{'command':<22} {'lines':>8} {'files':>8} {'MB':>8} "
        f"{'wall':>9} {'read':>8} {'trie':>8} {'search':>9} {'plan':>8} "
        f"{'spawn':>8} {'wait':>8} {'merge':>8} {'child_out':>9} {'max_worker':>11}"
    )
    print(header)
    print("-" * len(header))
    for result in results:
        samples = result["samples"]
        mb = result["bytes"] / (1024 * 1024)
        wait_ms = (
            average(samples, "worker_wait_ms")
            + average(samples, "child_report_read_ms")
            + average(samples, "child_wait_ms")
        )
        print(
            f"{result['name']:<22} "
            f"{result['lines']:>8} "
            f"{result['files']:>8} "
            f"{mb:>8.1f} "
            f"{average(samples, 'wall_ms'):>9.2f} "
            f"{average(samples, 'pattern_read_ms'):>8.2f} "
            f"{average(samples, 'trie_build_ms'):>8.2f} "
            f"{average(samples, 'search_ms'):>9.2f} "
            f"{average(samples, 'work_plan_ms'):>8.2f} "
            f"{average(samples, 'worker_spawn_ms'):>8.2f} "
            f"{wait_ms:>8.2f} "
            f"{average(samples, 'merge_ms'):>8.2f} "
            f"{average(samples, 'child_output_collect_ms'):>9.2f} "
            f"{average(samples, 'max_worker_ms'):>11.2f}"
        )


def main():
    parser = argparse.ArgumentParser(description="Benchmark recursive p-grep against ripgrep.")
    parser.add_argument("--path", type=Path, default=ROOT)
    parser.add_argument("--patterns", type=Path, default=DEFAULT_PATTERNS)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--emit-output", action="store_true", help="write matching lines to a temp file")
    args = parser.parse_args()

    def make_case(mode, jobs=None):
        def command_factory():
            output = None
            if args.emit_output:
                fd, output_path = tempfile.mkstemp(prefix="p-grep-out-", suffix=".txt")
                os.close(fd)
                Path(output_path).unlink(missing_ok=True)
                output = output_path
            return pgrep_command(args.patterns, args.path, mode, jobs, output)

        return command_factory

    cases = [("p-grep single", make_case("single"))]
    for jobs in JOB_COUNTS:
        cases.append((f"p-grep threads x{jobs}", make_case("threads", jobs)))
    for jobs in JOB_COUNTS:
        cases.append((f"p-grep processes x{jobs}", make_case("processes", jobs)))

    print(f"path={args.path} patterns={args.patterns} repeats={args.repeats} emit_output={args.emit_output}")
    print()

    pgrep_results = [run_pgrep_case(name, command, args.repeats) for name, command in cases]
    print_pgrep_table(pgrep_results)

    rg_result = run_rg(args.patterns, args.path, args.repeats, args.emit_output)
    if rg_result:
        baseline_lines = pgrep_results[0]["lines"]
        line_note = "" if rg_result["lines"] == baseline_lines else f" (!= {baseline_lines})"
        print()
        print("ripgrep")
        print(f"{'command':<22} {'lines':>8} {'avg_wall_ms':>12}")
        print("-" * 44)
        print(f"{rg_result['name']:<22} {str(rg_result['lines']) + line_note:>8} {rg_result['avg_wall_ms']:>12.2f}")


if __name__ == "__main__":
    main()
