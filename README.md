# p-grep

`p-grep` is a small C++ Aho-Corasick playground for fixed-string multi-pattern search. It starts with the classic trie plus suffix-link construction described by cp-algorithms, then measures:

- a single-threaded scan
- a shared-memory multi-threaded scan
- a POSIX multi-process scan
- system `grep`
- `ripgrep`

The project reports total matches, not matching lines.

## Build

```sh
make
```

## Generate Data

```sh
bin/generate-corpus --bytes 8388608 --patterns 256 --pattern-length 8
```

This writes:

- `data/patterns.txt`
- `data/corpus.txt`

## Run

```sh
bin/p-grep --patterns data/patterns.txt --input data/corpus.txt --mode single --timing
bin/p-grep --patterns data/patterns.txt --input data/corpus.txt --mode threads --jobs 8 --timing
bin/p-grep --patterns data/patterns.txt --input data/corpus.txt --mode processes --jobs 8 --timing
```

## Benchmark

```sh
make bench
```

Or with custom settings:

```sh
python3 scripts/bench.py --bytes 134217728 --patterns 1024 --pattern-length 12 --jobs 8 --repeats 7 --regenerate
```

`grep` and `ripgrep` are run as fixed-string matchers using `-F -o -f`. Their `-o` mode counts non-overlapping printed matches, while `p-grep` counts every pattern ending, including overlapping matches. The generated corpus uses random fixed-length tokens, which keeps those counts practically comparable.

## Notes

The threaded and process modes split the input into chunks. Each chunk scans up to `max_pattern_length - 1` bytes of left and right overlap and only counts matches whose start position belongs to that chunk, so matches crossing a split are not lost or double-counted.

This project defaults to `/opt/homebrew/bin/g++-15` when it is installed, matching the compiler setup in `~/Desktop/Coding/cc`. You can override it with `make CXX=/path/to/compiler`.
