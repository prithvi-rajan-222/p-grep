#!/usr/bin/env python3

import argparse
import shutil
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
P_GREP = ROOT / "bin" / "p-grep"
GEN = ROOT / "bin" / "generate-corpus"
DATA = ROOT / "data"
PATTERNS = DATA / "patterns.txt"
CORPUS = DATA / "corpus.txt"


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


def ensure_data(args):
    if args.regenerate or not PATTERNS.exists() or not CORPUS.exists():
        subprocess.run(
            [
                str(GEN),
                "--data-dir",
                str(DATA),
                "--bytes",
                str(args.bytes),
                "--patterns",
                str(args.patterns),
                "--pattern-length",
                str(args.pattern_length),
            ],
            cwd=ROOT,
            check=True,
        )


def main():
    parser = argparse.ArgumentParser(description="Benchmark p-grep against grep and ripgrep.")
    parser.add_argument("--bytes", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--patterns", type=int, default=256)
    parser.add_argument("--pattern-length", type=int, default=8)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--regenerate", action="store_true")
    args = parser.parse_args()

    ensure_data(args)

    commands = [
        ("p-grep single", [str(P_GREP), "--patterns", str(PATTERNS), "--input", str(CORPUS), "--mode", "single"]),
        (
            f"p-grep threads x{args.jobs}",
            [str(P_GREP), "--patterns", str(PATTERNS), "--input", str(CORPUS), "--mode", "threads", "--jobs", str(args.jobs)],
        ),
        (
            f"p-grep processes x{args.jobs}",
            [str(P_GREP), "--patterns", str(PATTERNS), "--input", str(CORPUS), "--mode", "processes", "--jobs", str(args.jobs)],
        ),
    ]

    grep = shutil.which("grep")
    if grep:
        commands.append(("grep -F -o", [grep, "-F", "-o", "-f", str(PATTERNS), str(CORPUS)]))

    rg = shutil.which("rg")
    if rg:
        commands.append(("rg -F -o", [rg, "-F", "-o", "-f", str(PATTERNS), str(CORPUS)]))

    print(f"corpus={CORPUS} bytes={CORPUS.stat().st_size} patterns={PATTERNS}")
    print(f"{'command':<24} {'best_ms':>10} {'matches':>12}")
    print("-" * 50)

    baseline = None
    for name, command in commands:
        if name.startswith("grep") or name.startswith("rg"):
            matches = count_output_lines(command)
            elapsed, _ = run(command, args.repeats, subprocess.DEVNULL)
        else:
            elapsed, output = run(command, args.repeats, subprocess.PIPE)
            matches = output.splitlines()[-1] if output else "?"
            if baseline is None:
                baseline = matches
            elif matches != baseline:
                matches = f"{matches} (!= {baseline})"

        print(f"{name:<24} {elapsed * 1000:>10.2f} {matches:>12}")


if __name__ == "__main__":
    main()
