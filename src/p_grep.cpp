#include "aho_corasick.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

struct Options {
    std::string patterns_path;
    fs::path search_path;
    std::string mode = "single";
    std::size_t jobs = std::max(1u, std::thread::hardware_concurrency());
    bool timing = false;
};

struct SearchStats {
    std::uint64_t files_scanned = 0;
    std::uint64_t bytes_processed = 0;
    std::uint64_t matched_lines = 0;

    SearchStats& operator+=(const SearchStats& other) {
        files_scanned += other.files_scanned;
        bytes_processed += other.bytes_processed;
        matched_lines += other.matched_lines;
        return *this;
    }
};

struct WorkerReport {
    std::uint64_t worker_id = 0;
    SearchStats stats;
    double elapsed_ms = 0.0;
};

struct FileTask {
    fs::path path;
    std::uint64_t size = 0;
};

struct PhaseTimings {
    std::uint64_t work_units = 0;
    double work_plan_ms = 0.0;
    double worker_spawn_ms = 0.0;
    double worker_wait_ms = 0.0;
    double merge_ms = 0.0;
    double child_report_read_ms = 0.0;
    double child_wait_ms = 0.0;
    double child_output_collect_ms = 0.0;
};

struct SearchOutput {
    SearchStats stats;
    std::vector<WorkerReport> workers;
    PhaseTimings phases;
};

struct MatcherLoadResult {
    AhoCorasick matcher;
    double pattern_read_ms = 0.0;
    double trie_build_ms = 0.0;
};

[[noreturn]] void usage(std::string_view message = {}) {
    if (!message.empty()) {
        std::cerr << "error: " << message << "\n\n";
    }
    std::cerr
        << "usage: p-grep --patterns FILE --path FILE_OR_DIR [--mode single|threads|processes] [--jobs N] [--timing]\n"
        << "\n"
        << "Counts matching lines recursively.\n";
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
        } else if (arg == "--path" || arg == "--input" || arg == "-i") {
            options.search_path = require_value(arg);
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
    if (options.search_path.empty()) {
        usage("--path is required");
    }
    if (options.mode != "single" && options.mode != "threads" && options.mode != "processes") {
        usage("--mode must be single, threads, or processes");
    }
    if (options.jobs == 0) {
        usage("--jobs must be greater than zero");
    }

    return options;
}

template <typename Start, typename End>
double elapsed_ms(Start start, End end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

MatcherLoadResult load_matcher(const std::string& path) {
    const auto read_start = std::chrono::steady_clock::now();
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
    const auto read_end = std::chrono::steady_clock::now();

    const auto build_start = std::chrono::steady_clock::now();
    ac.build();
    const auto build_end = std::chrono::steady_clock::now();

    if (ac.pattern_count() == 0) {
        throw std::runtime_error("no non-empty patterns found in " + path);
    }

    return MatcherLoadResult{
        std::move(ac),
        elapsed_ms(read_start, read_end),
        elapsed_ms(build_start, build_end),
    };
}

bool is_hidden_name(std::string_view name) {
    return !name.empty() && name[0] == '.';
}

bool is_hidden_path(const fs::path& path) {
    return is_hidden_name(path.filename().string());
}

bool is_basic_ignored_directory(std::string_view name) {
    return name == ".git"
        || name == "build"
        || name == "node_modules"
        || name == "target"
        || name == "dist"
        || name.substr(0, std::string_view("cmake-build-").size()) == "cmake-build-";
}

std::vector<FileTask> collect_file_tasks(const fs::path& root) {
    std::vector<FileTask> files;

#if defined(__unix__) || defined(__APPLE__)
    struct stat root_stat {};
    if (lstat(root.c_str(), &root_stat) != 0) {
        throw std::runtime_error("failed to inspect " + root.string() + ": " + std::strerror(errno));
    }

    if (S_ISREG(root_stat.st_mode)) {
        if (!is_hidden_path(root) && root_stat.st_size > 0) {
            files.push_back(FileTask{root, static_cast<std::uint64_t>(root_stat.st_size)});
        }
        return files;
    }
    if (!S_ISDIR(root_stat.st_mode) || is_hidden_path(root) || is_basic_ignored_directory(root.filename().string())) {
        return files;
    }

    std::vector<fs::path> pending;
    pending.push_back(root);
    while (!pending.empty()) {
        fs::path directory = std::move(pending.back());
        pending.pop_back();

        DIR* dir = opendir(directory.c_str());
        if (dir == nullptr) {
            continue;
        }

        while (dirent* entry = readdir(dir)) {
            const std::string_view name(entry->d_name);
            if (name == "." || name == ".." || is_hidden_name(name)) {
                continue;
            }

            fs::path child = directory / entry->d_name;
            struct stat child_stat {};
            if (lstat(child.c_str(), &child_stat) != 0) {
                continue;
            }
            if (S_ISLNK(child_stat.st_mode)) {
                continue;
            }
            if (S_ISDIR(child_stat.st_mode)) {
                if (!is_basic_ignored_directory(name)) {
                    pending.push_back(std::move(child));
                }
            } else if (S_ISREG(child_stat.st_mode) && child_stat.st_size > 0) {
                files.push_back(FileTask{std::move(child), static_cast<std::uint64_t>(child_stat.st_size)});
            }
        }

        closedir(dir);
    }
#else
    std::error_code ec;
    const fs::directory_entry root_entry(root, ec);
    if (ec) {
        throw std::runtime_error("failed to inspect " + root.string());
    }

    if (root_entry.is_regular_file(ec)) {
        if (!is_hidden_path(root)) {
            const std::uint64_t size = root_entry.file_size(ec);
            if (!ec && size > 0) {
                files.push_back(FileTask{root, size});
            }
        }
        return files;
    }
#endif

    return files;
}

void search_file_task(
    const AhoCorasick& ac,
    const FileTask& file,
    std::vector<char>& buffer,
    SearchOutput& output) {
    std::ifstream in(file.path, std::ios::binary);
    if (!in) {
        return;
    }

    std::uint32_t state = ac.start_state();
    bool line_matched = false;
    bool saw_byte_on_line = false;
    bool saw_content = false;
    std::uint64_t matched_lines = 0;

    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            const unsigned char ch = static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            if (ch == '\0') {
                return;
            }
            saw_content = true;
            if (ch != '\n' && ch != '\r') {
                saw_byte_on_line = true;
            }
            state = ac.step(state, ch);
            if (ac.is_match_state(state)) {
                line_matched = true;
            }
            if (ch == '\n') {
                if (line_matched) {
                    ++matched_lines;
                }
                line_matched = false;
                saw_byte_on_line = false;
                state = ac.start_state();
            }
        }
    }

    if (!saw_content) {
        return;
    }

    ++output.stats.files_scanned;
    output.stats.bytes_processed += file.size;
    if (line_matched && saw_byte_on_line) {
        ++matched_lines;
    }
    output.stats.matched_lines += matched_lines;
}

std::uint64_t estimated_work_cost(const FileTask& file) {
    constexpr std::uint64_t kPerFileCostBytes = 32768;
    return file.size + kPerFileCostBytes;
}

std::vector<std::vector<FileTask>> assign_file_tasks(std::vector<FileTask> files, std::size_t jobs) {
    jobs = std::max<std::size_t>(1, jobs);
    std::vector<std::vector<FileTask>> assignments(jobs);
    std::vector<std::uint64_t> assigned_cost(jobs, 0);

    std::sort(files.begin(), files.end(), [](const FileTask& a, const FileTask& b) {
        const std::uint64_t a_cost = estimated_work_cost(a);
        const std::uint64_t b_cost = estimated_work_cost(b);
        if (a_cost != b_cost) {
            return a_cost > b_cost;
        }
        return a.path < b.path;
    });

    for (const FileTask& file : files) {
        const auto lightest = std::min_element(assigned_cost.begin(), assigned_cost.end());
        const std::size_t worker = static_cast<std::size_t>(lightest - assigned_cost.begin());
        assignments[worker].push_back(file);
        assigned_cost[worker] += estimated_work_cost(file);
    }

    return assignments;
}

SearchOutput search_single(const AhoCorasick& ac, const fs::path& root) {
    SearchOutput output;
    const auto plan_start = std::chrono::steady_clock::now();
    const auto files = collect_file_tasks(root);
    const auto plan_end = std::chrono::steady_clock::now();
    output.phases.work_units = files.size();
    output.phases.work_plan_ms = elapsed_ms(plan_start, plan_end);

    const auto start = std::chrono::steady_clock::now();
    std::vector<char> buffer(256 * 1024);
    for (const FileTask& file : files) {
        search_file_task(ac, file, buffer, output);
    }
    const auto end = std::chrono::steady_clock::now();
    output.phases.worker_wait_ms = elapsed_ms(start, end);
    output.workers.push_back(WorkerReport{0, output.stats, elapsed_ms(start, end)});
    return output;
}

SearchOutput search_assigned_files(const AhoCorasick& ac, const std::vector<FileTask>& files) {
    SearchOutput output;
    std::vector<char> buffer(256 * 1024);
    for (const FileTask& file : files) {
        search_file_task(ac, file, buffer, output);
    }
    return output;
}

SearchOutput search_dynamic_files(
    const AhoCorasick& ac,
    const std::vector<FileTask>& files,
    std::atomic<std::size_t>& next_file) {
    SearchOutput output;
    std::vector<char> buffer(256 * 1024);
    constexpr std::size_t kBatchSize = 5;
    while (true) {
        const std::size_t begin = next_file.fetch_add(kBatchSize, std::memory_order_relaxed);
        if (begin >= files.size()) {
            break;
        }
        const std::size_t end = std::min(begin + kBatchSize, files.size());
        for (std::size_t index = begin; index < end; ++index) {
            search_file_task(ac, files[index], buffer, output);
        }
    }
    return output;
}

SearchOutput search_threads(const AhoCorasick& ac, const fs::path& root, std::size_t jobs) {
    SearchOutput combined;

    const auto plan_start = std::chrono::steady_clock::now();
    const auto files = collect_file_tasks(root);
    const auto plan_end = std::chrono::steady_clock::now();
    combined.phases.work_units = files.size();
    combined.phases.work_plan_ms = elapsed_ms(plan_start, plan_end);

    jobs = std::max<std::size_t>(1, jobs);
    std::vector<std::thread> threads;
    std::vector<SearchOutput> outputs(jobs);
    std::vector<WorkerReport> reports(jobs);
    std::atomic<std::size_t> next_file{0};
    threads.reserve(jobs);

    const auto spawn_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < jobs; ++i) {
        threads.emplace_back([&, i] {
            const auto start = std::chrono::steady_clock::now();
            outputs[i] = search_dynamic_files(ac, files, next_file);
            const auto end = std::chrono::steady_clock::now();
            reports[i].worker_id = i;
            reports[i].stats = outputs[i].stats;
            reports[i].elapsed_ms = elapsed_ms(start, end);
        });
    }
    const auto spawn_end = std::chrono::steady_clock::now();
    combined.phases.worker_spawn_ms = elapsed_ms(spawn_start, spawn_end);

    const auto wait_start = std::chrono::steady_clock::now();
    for (auto& thread : threads) {
        thread.join();
    }
    const auto wait_end = std::chrono::steady_clock::now();
    combined.phases.worker_wait_ms = elapsed_ms(wait_start, wait_end);

    const auto merge_start = std::chrono::steady_clock::now();
    combined.workers = std::move(reports);
    for (SearchOutput& output : outputs) {
        combined.stats += output.stats;
    }
    const auto merge_end = std::chrono::steady_clock::now();
    combined.phases.merge_ms = elapsed_ms(merge_start, merge_end);
    return combined;
}

void write_all(int fd, const void* data, std::size_t size) {
#if defined(__unix__) || defined(__APPLE__)
    const auto* bytes = static_cast<const char*>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = write(fd, bytes + written, size - written);
        if (n <= 0) {
            throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
        }
        written += static_cast<std::size_t>(n);
    }
#else
    (void)fd;
    (void)data;
    (void)size;
#endif
}

void read_all(int fd, void* data, std::size_t size) {
#if defined(__unix__) || defined(__APPLE__)
    auto* bytes = static_cast<char*>(data);
    std::size_t read_total = 0;
    while (read_total < size) {
        const ssize_t n = read(fd, bytes + read_total, size - read_total);
        if (n <= 0) {
            throw std::runtime_error("failed to read child result");
        }
        read_total += static_cast<std::size_t>(n);
    }
#else
    (void)fd;
    (void)data;
    (void)size;
#endif
}

SearchOutput search_processes(const AhoCorasick& ac, const fs::path& root, std::size_t jobs) {
#if defined(__unix__) || defined(__APPLE__)
    SearchOutput combined;

    const auto plan_start = std::chrono::steady_clock::now();
    const auto files = collect_file_tasks(root);
    const auto assignments = assign_file_tasks(files, jobs);
    const auto plan_end = std::chrono::steady_clock::now();
    combined.phases.work_units = files.size();
    combined.phases.work_plan_ms = elapsed_ms(plan_start, plan_end);

    std::vector<pid_t> children;
    std::vector<int> read_fds;
    children.reserve(assignments.size());
    read_fds.reserve(assignments.size());

    const auto spawn_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < assignments.size(); ++i) {
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
            try {
                const auto start = std::chrono::steady_clock::now();
                SearchOutput output = search_assigned_files(ac, assignments[i]);
                const auto end = std::chrono::steady_clock::now();
                WorkerReport report;
                report.worker_id = i;
                report.stats = output.stats;
                report.elapsed_ms = elapsed_ms(start, end);
                write_all(fds[1], &report, sizeof(report));
                close(fds[1]);
                _exit(0);
            } catch (...) {
                _exit(1);
            }
        }

        close(fds[1]);
        children.push_back(pid);
        read_fds.push_back(fds[0]);
    }
    const auto spawn_end = std::chrono::steady_clock::now();
    combined.phases.worker_spawn_ms = elapsed_ms(spawn_start, spawn_end);

    const auto report_start = std::chrono::steady_clock::now();
    for (int fd : read_fds) {
        WorkerReport report;
        read_all(fd, &report, sizeof(report));
        close(fd);
        combined.stats += report.stats;
        combined.workers.push_back(report);
    }
    const auto report_end = std::chrono::steady_clock::now();
    combined.phases.child_report_read_ms = elapsed_ms(report_start, report_end);

    const auto wait_start = std::chrono::steady_clock::now();
    for (pid_t child : children) {
        int status = 0;
        if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            throw std::runtime_error("child process failed");
        }
    }
    const auto wait_end = std::chrono::steady_clock::now();
    combined.phases.child_wait_ms = elapsed_ms(wait_start, wait_end);

    return combined;
#else
    (void)ac;
    (void)root;
    (void)jobs;
    throw std::runtime_error("--mode processes requires a POSIX platform");
#endif
}

void print_output(const SearchOutput& output) {
    std::cout << output.stats.matched_lines << '\n';
}

void print_worker_reports(std::string_view mode, const std::vector<WorkerReport>& workers) {
    for (const WorkerReport& worker : workers) {
        std::cerr << mode
                  << "_worker=" << worker.worker_id
                  << " files=" << worker.stats.files_scanned
                  << " bytes=" << worker.stats.bytes_processed
                  << " matched_lines=" << worker.stats.matched_lines
                  << " elapsed_ms=" << worker.elapsed_ms
                  << "\n";
    }
}

void print_phase_timings(const SearchOutput& output) {
    std::cerr << "work_units=" << output.phases.work_units
              << " work_plan_ms=" << output.phases.work_plan_ms
              << " worker_spawn_ms=" << output.phases.worker_spawn_ms
              << " worker_wait_ms=" << output.phases.worker_wait_ms
              << " merge_ms=" << output.phases.merge_ms;

    if (output.phases.child_report_read_ms > 0.0
        || output.phases.child_wait_ms > 0.0
        || output.phases.child_output_collect_ms > 0.0) {
        std::cerr << " child_report_read_ms=" << output.phases.child_report_read_ms
                  << " child_wait_ms=" << output.phases.child_wait_ms
                  << " child_output_collect_ms=" << output.phases.child_output_collect_ms;
    }

    std::cerr << "\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);

        const auto matcher_start = std::chrono::steady_clock::now();
        MatcherLoadResult matcher = load_matcher(options.patterns_path);
        const auto matcher_end = std::chrono::steady_clock::now();

        const auto search_start = std::chrono::steady_clock::now();
        SearchOutput output;
        if (options.mode == "single") {
            output = search_single(matcher.matcher, options.search_path);
        } else if (options.mode == "threads") {
            output = search_threads(matcher.matcher, options.search_path, options.jobs);
        } else {
            output = search_processes(matcher.matcher, options.search_path, options.jobs);
        }
        const auto search_end = std::chrono::steady_clock::now();

        print_output(output);

        if (options.timing) {
            const auto matcher_total_ms = elapsed_ms(matcher_start, matcher_end);
            const auto search_ms = elapsed_ms(search_start, search_end);
            std::cerr << "patterns=" << matcher.matcher.pattern_count()
                      << " files=" << output.stats.files_scanned
                      << " bytes=" << output.stats.bytes_processed
                      << " matched_lines=" << output.stats.matched_lines
                      << " mode=" << options.mode
                      << " jobs=" << options.jobs
                      << " pattern_read_ms=" << matcher.pattern_read_ms
                      << " trie_build_ms=" << matcher.trie_build_ms
                      << " matcher_total_ms=" << matcher_total_ms
                      << " search_ms=" << search_ms
                      << "\n";
            print_phase_timings(output);
            print_worker_reports(options.mode, output.workers);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
