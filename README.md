# p-grep

Small C++ search playground for testing Aho-Corasick against `ripgrep`.

This is not trying to be a full `rg` clone. It is mostly for learning how much speed we can get from a fixed-string multi-pattern matcher, threads, processes, and different output strategies.


## Benchmarks

Count-only benchmark:

```sh
python3 scripts/bench.py --path ../openclaw --patterns patterns/code-search.txt --repeats 1
```

Benchmark while writing matched output to temp files:

```sh
python3 scripts/bench.py --path ../openclaw --patterns patterns/code-search.txt --repeats 1 --emit-output
```

Pattern sets:

```text
patterns/few-similar.txt       4 similar import patterns
patterns/code-search.txt       33 code-search patterns
patterns/many-code-search.txt  250 larger code/search patterns
```

Recent OpenClaw snapshot, count-only, `--repeats 1`:

| Pattern file | Patterns | Matches | Best p-grep | p-grep ms | rg ms |
| --- | ---: | ---: | --- | ---: | ---: |
| `patterns/few-similar.txt` | 4 | 60,211 | threads x8 | 200.87 | 227.81 |
| `patterns/code-search.txt` | 33 | 46,503 | threads x8 | 202.53 | 223.08 |
| `patterns/many-code-search.txt` | 250 | 1,123,976 | threads x8 | 196.58 | 706.17 |

Recent OpenClaw snapshot, direct output writes:

```text
33 patterns:   p-grep threads x8 ~266 ms, rg ~197 ms
```

Direct line-by-line output is intentionally expensive. Buffered output was faster, but we removed it to see the raw write/lock cost.

## Learnings
- Multi-threading works better than multiple processes
- Pre calculating while thread reads which file is a bad idea - this causes us to walk the file tree unnecessarily
- Keep a list of files, and allow threads to pick from that file when free. Slight overhead due to locks beats the need to walk the file tree
- Moved from a struct that represented each node in the DFA, to simple vectors. This improves cache locality. Very minimal improvement ~2% faster
- Read files chunk by chunk than line by line
- Allow a thread to pickup multiple files (5) at once from the queue. Lesser locks acquired, leading to a speedup in time

## Future path
- Build an index of the entire codebase. Use that index to narrow down which files we need to search
- Walk file tree while the DFA builds


## My Setup

Built/tested on macOS with:

```sh
/opt/homebrew/bin/g++-15
C++20 / gnu++20
Python 3
ripgrep
```

The `Makefile` prefers `/opt/homebrew/bin/g++-15` if it exists:

```make
CXX := /opt/homebrew/bin/g++-15
CXXFLAGS := -std=gnu++20 -O3 -DNDEBUG -Wall -Wextra -pedantic
```

Override if needed:

```sh
make CXX=g++
```

## Build

```sh
git clone <repo-url>
cd p-grep
make
```

This builds:

```text
bin/p-grep
bin/generate-corpus
```

## Run

Count matching lines:

```sh
bin/p-grep --patterns patterns/code-search.txt --path ../openclaw --mode single --timing
bin/p-grep --patterns patterns/code-search.txt --path ../openclaw --mode threads --jobs 8 --timing
bin/p-grep --patterns patterns/code-search.txt --path ../openclaw --mode processes --jobs 8 --timing
```

Write matched lines too:

```sh
bin/p-grep --patterns patterns/code-search.txt --path ../openclaw --mode threads --jobs 8 --output matches.txt --timing
```

Output format:

```text
path:line_number:line text
```

Current file filtering is simple: skip hidden names, symlinks, empty files, binary files with NUL bytes, and common generated directories like `.git`, `node_modules`, `build`, `dist`, `target`, and `cmake-build-*`.

Full `.gitignore`, `.ignore`, and `.rgignore` semantics are deliberately out of scope for now.
