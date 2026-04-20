# p-grep

`p-grep` is a small C++ Aho-Corasick playground for fixed-string multi-pattern code search. It starts with the classic trie plus suffix-link construction described by cp-algorithms, then searches files in a ripgrep-like `path:line:line text` format.

It currently supports:

- single-threaded recursive path search
- shared-memory multi-threaded recursive path search
- POSIX multi-process recursive path search
- benchmark comparison with system `grep` and `ripgrep`

## Build

```sh
make
```

## Patterns

The default real-code-search pattern set lives at:

```text
patterns/code-search.txt
```

## Run

```sh
bin/p-grep --patterns patterns/code-search.txt --path /path/to/codebase --mode single --timing
bin/p-grep --patterns patterns/code-search.txt --path /path/to/codebase --mode threads --jobs 8 --timing
bin/p-grep --patterns patterns/code-search.txt --path /path/to/codebase --mode processes --jobs 8 --timing
```

Output looks like:

```text
src/main.cpp:42:    // TODO: handle this case
```

With `--timing`, summary and worker timing are printed to stderr. Thread/process modes include one line per requested worker:

```text
threads_worker=0 files=10 bytes=123456 matched_lines=42 elapsed_ms=3.21
```

## Benchmark

```sh
make bench
```

Or with custom settings:

```sh
python3 scripts/bench.py --path /path/to/codebase --patterns patterns/code-search.txt --jobs 8 --repeats 3
```

`ripgrep` is run as a fixed-string line matcher using `rg -F -n -f`. `grep` is run with `grep -R -F -n -f`.

## Notes

The threaded and process modes do not split individual files. They expand directory work one level below the search root, estimate each unit by bytes plus file count, then greedily assign the largest units to the currently lightest worker.

The recursive search follows the basic visible ripgrep defaults: hidden files/directories are skipped, symlinks are not followed, binary files are skipped when a NUL byte is found in the first 8 KiB, and common generated directories such as `.git`, `build`, `cmake-build-*`, `node_modules`, `target`, and `dist` are skipped. Full `.gitignore`/`.ignore`/`.rgignore` parsing is not implemented yet.

This project defaults to `/opt/homebrew/bin/g++-15` when it is installed, matching the compiler setup in `~/Desktop/Coding/cc`. You can override it with `make CXX=/path/to/compiler`.
