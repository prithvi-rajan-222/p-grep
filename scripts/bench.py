#!/usr/bin/env python3

import argparse
import shutil
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
P_GREP = ROOT / "bin" / "p-grep"
DEFAULT_PATTERNS = ROOT / "patterns" / "code-search.txt"


def run(command, repeats, stdout):
    samples = []
    output = None
    for _ in range(repeats):
        start = time.perf_counter()
        completed = subprocess.run(
            command,
            cwd=ROOT,
            check=True,
            text=True,
            stdout=stdout,
            stderr=subprocess.PIPE,
        )
        elapsed = time.perf_counter() - start
        samples.append(elapsed)
        output = completed.stdout.strip() if completed.stdout is not None else None
    return min(samples), output


def count_output_lines(command):
    completed = subprocess.run(
        command,
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    output = completed.stdout.strip()
    return str(len(output.splitlines()) if output else 0)

def main():
    parser = argparse.ArgumentParser(description="Benchmark recursive p-grep against grep and ripgrep.")
    parser.add_argument("--path", type=Path, default=ROOT)
    parser.add_argument("--patterns", type=Path, default=DEFAULT_PATTERNS)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()

    commands = [
        ("p-grep single", [str(P_GREP), "--patterns", str(args.patterns), "--path", str(args.path), "--mode", "single"]),
        (
            f"p-grep threads x{args.jobs}",
            [str(P_GREP), "--patterns", str(args.patterns), "--path", str(args.path), "--mode", "threads", "--jobs", str(args.jobs)],
        ),
        (
            f"p-grep processes x{args.jobs}",
            [str(P_GREP), "--patterns", str(args.patterns), "--path", str(args.path), "--mode", "processes", "--jobs", str(args.jobs)],
        ),
    ]

    rg = shutil.which("rg")
    if rg:
        commands.append(("rg -F -n", [rg, "-F", "-n", "-f", str(args.patterns), str(args.path)]))

    print(f"path={args.path} patterns={args.patterns}")
    print(f"{'command':<24} {'best_ms':>10} {'lines':>12}")
    print("-" * 50)

    baseline = None
    for name, command in commands:
        lines = count_output_lines(command)
        elapsed, _ = run(command, args.repeats, subprocess.DEVNULL)
        if baseline is None:
            baseline = lines
        elif lines != baseline:
            lines = f"{lines} (!= {baseline})"

        print(f"{name:<24} {elapsed * 1000:>10.2f} {lines:>12}")


if __name__ == "__main__":
    main()
