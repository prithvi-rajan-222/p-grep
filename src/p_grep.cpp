#include "aho_corasick.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct Options {
    std::string patterns_path;
    std::string input_path;
    std::string mode = "single";
    std::size_t jobs = std::max(1u, std::thread::hardware_concurrency());
    bool timing = false;
};

struct Chunk {
    std::size_t scan_begin;
    std::size_t scan_end;
    std::size_t valid_begin;
    std::size_t valid_end;
};

[[noreturn]] void usage(std::string_view message = {}) {
    if (!message.empty()) {
        std::cerr << "error: " << message << "\n\n";
    }
    std::cerr
        << "usage: p-grep --patterns FILE --input FILE [--mode single|threads|processes] [--jobs N] [--timing]\n"
        << "\n"
        << "Counts fixed-string matches with Aho-Corasick. Patterns are read one per line.\n";
    std::exit(message.empty() ? 0 : 2);
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        auto require_value = [&](std::string_view name) -> std::string {
            if (i + 1 >= argc) {
                usage("missing value for " + std::string(name));
            }
            return argv[++i];
        };

        if (arg == "--patterns" || arg == "-p") {
            options.patterns_path = require_value(arg);
        } else if (arg == "--input" || arg == "-i") {
            options.input_path = require_value(arg);
        } else if (arg == "--mode" || arg == "-m") {
            options.mode = require_value(arg);
        } else if (arg == "--jobs" || arg == "-j") {
            options.jobs = std::stoull(require_value(arg));
        } else if (arg == "--timing") {
            options.timing = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
        } else {
            usage("unknown argument " + std::string(arg));
        }
    }

    if (options.patterns_path.empty()) {
        usage("--patterns is required");
    }
    if (options.input_path.empty()) {
        usage("--input is required");
    }
    if (options.mode != "single" && options.mode != "threads" && options.mode != "processes") {
        usage("--mode must be single, threads, or processes");
    }
    if (options.jobs == 0) {
        usage("--jobs must be greater than zero");
    }

    return options;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open " + path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string data(static_cast<std::size_t>(std::max<std::streamoff>(0, size)), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in && !in.eof()) {
        throw std::runtime_error("failed to read " + path);
    }
    return data;
}

AhoCorasick load_matcher(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open " + path);
    }

    AhoCorasick ac;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        ac.add_pattern(line);
    }
    ac.build();

    if (ac.pattern_count() == 0) {
        throw std::runtime_error("no non-empty patterns found in " + path);
    }

    return ac;
}

std::vector<Chunk> make_chunks(std::size_t text_size, std::size_t jobs, std::size_t overlap) {
    jobs = std::min(jobs, std::max<std::size_t>(1, text_size));
    std::vector<Chunk> chunks;
    chunks.reserve(jobs);

    for (std::size_t job = 0; job < jobs; ++job) {
        const std::size_t valid_begin = text_size * job / jobs;
        const std::size_t valid_end = text_size * (job + 1) / jobs;
        const std::size_t scan_begin = valid_begin > overlap ? valid_begin - overlap : 0;
        const std::size_t scan_end = std::min(text_size, valid_end + overlap);
        chunks.push_back(Chunk{scan_begin, scan_end, valid_begin - scan_begin, valid_end - scan_begin});
    }

    return chunks;
}

std::uint64_t count_single(const AhoCorasick& ac, std::string_view text) {
    return ac.count_matches(text);
}

std::uint64_t count_threads(const AhoCorasick& ac, std::string_view text, std::size_t jobs) {
    const std::size_t overlap = ac.max_pattern_length() > 0 ? ac.max_pattern_length() - 1 : 0;
    const auto chunks = make_chunks(text.size(), jobs, overlap);
    std::vector<std::thread> threads;
    std::vector<std::uint64_t> counts(chunks.size(), 0);
    threads.reserve(chunks.size());

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        threads.emplace_back([&, i] {
            const Chunk& chunk = chunks[i];
            const std::string_view view(text.data() + chunk.scan_begin, chunk.scan_end - chunk.scan_begin);
            counts[i] = ac.count_matches_in_range(view, chunk.valid_begin, chunk.valid_end);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    std::uint64_t total = 0;
    for (std::uint64_t count : counts) {
        total += count;
    }
    return total;
}

std::uint64_t count_processes(const AhoCorasick& ac, std::string_view text, std::size_t jobs) {
#if defined(__unix__) || defined(__APPLE__)
    const std::size_t overlap = ac.max_pattern_length() > 0 ? ac.max_pattern_length() - 1 : 0;
    const auto chunks = make_chunks(text.size(), jobs, overlap);
    std::vector<pid_t> children;
    std::vector<int> read_fds;
    children.reserve(chunks.size());
    read_fds.reserve(chunks.size());

    for (const Chunk& chunk : chunks) {
        int fds[2];
        if (pipe(fds) != 0) {
            throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
        }

        const pid_t pid = fork();
        if (pid < 0) {
            throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
        }
        if (pid == 0) {
            close(fds[0]);
            const std::string_view view(text.data() + chunk.scan_begin, chunk.scan_end - chunk.scan_begin);
            const std::uint64_t count = ac.count_matches_in_range(view, chunk.valid_begin, chunk.valid_end);
            const auto* bytes = reinterpret_cast<const char*>(&count);
            std::size_t written = 0;
            while (written < sizeof(count)) {
                const ssize_t n = write(fds[1], bytes + written, sizeof(count) - written);
                if (n <= 0) {
                    _exit(1);
                }
                written += static_cast<std::size_t>(n);
            }
            close(fds[1]);
            _exit(0);
        }

        close(fds[1]);
        children.push_back(pid);
        read_fds.push_back(fds[0]);
    }

    std::uint64_t total = 0;
    for (int fd : read_fds) {
        std::uint64_t count = 0;
        auto* bytes = reinterpret_cast<char*>(&count);
        std::size_t read_total = 0;
        while (read_total < sizeof(count)) {
            const ssize_t n = read(fd, bytes + read_total, sizeof(count) - read_total);
            if (n <= 0) {
                break;
            }
            read_total += static_cast<std::size_t>(n);
        }
        close(fd);
        if (read_total != sizeof(count)) {
            throw std::runtime_error("failed to read child result");
        }
        total += count;
    }

    for (pid_t child : children) {
        int status = 0;
        if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            throw std::runtime_error("child process failed");
        }
    }

    return total;
#else
    (void)ac;
    (void)text;
    (void)jobs;
    throw std::runtime_error("--mode processes requires a POSIX platform");
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);

        const auto build_start = std::chrono::steady_clock::now();
        AhoCorasick ac = load_matcher(options.patterns_path);
        const auto build_end = std::chrono::steady_clock::now();

        const std::string text = read_file(options.input_path);
        const auto search_start = std::chrono::steady_clock::now();

        std::uint64_t count = 0;
        if (options.mode == "single") {
            count = count_single(ac, text);
        } else if (options.mode == "threads") {
            count = count_threads(ac, text, options.jobs);
        } else {
            count = count_processes(ac, text, options.jobs);
        }

        const auto search_end = std::chrono::steady_clock::now();
        std::cout << count << "\n";

        if (options.timing) {
            const auto build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();
            const auto search_ms = std::chrono::duration<double, std::milli>(search_end - search_start).count();
            std::cerr << "patterns=" << ac.pattern_count()
                      << " bytes=" << text.size()
                      << " mode=" << options.mode
                      << " jobs=" << options.jobs
                      << " build_ms=" << build_ms
                      << " search_ms=" << search_ms
                      << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
