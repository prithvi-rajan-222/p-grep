#include "aho_corasick.hpp"

#include <algorithm>
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

struct WorkUnit {
    fs::path path;
    std::uint64_t estimated_files = 0;
    std::uint64_t estimated_bytes = 0;
};

struct SearchOutput {
    SearchStats stats;
    std::vector<std::string> lines;
    std::vector<WorkerReport> workers;
};

[[noreturn]] void usage(std::string_view message = {}) {
    if (!message.empty()) {
        std::cerr << "error: " << message << "\n\n";
    }
    std::cerr
        << "usage: p-grep --patterns FILE --path FILE_OR_DIR [--mode single|threads|processes] [--jobs N] [--timing]\n"
        << "\n"
        << "Searches fixed strings recursively and prints path:line:matching line.\n";
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

bool is_hidden_name(const fs::path& path) {
    const std::string name = path.filename().string();
    return !name.empty() && name[0] == '.';
}

bool is_basic_ignored_directory(const fs::path& path) {
    const std::string name = path.filename().string();
    return name == ".git"
        || name == "build"
        || name == "node_modules"
        || name == "target"
        || name == "dist"
        || name.rfind("cmake-build-", 0) == 0;
}

bool is_probably_binary(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return true;
    }

    constexpr std::size_t kProbeSize = 8192;
    char buffer[kProbeSize];
    in.read(buffer, static_cast<std::streamsize>(kProbeSize));
    const std::streamsize n = in.gcount();

    for (std::streamsize i = 0; i < n; ++i) {
        if (buffer[i] == '\0') {
            return true;
        }
    }
    return false;
}

bool should_search_file(const fs::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_symlink(ec) || !entry.is_regular_file(ec)) {
        return false;
    }
    if (is_hidden_name(entry.path())) {
        return false;
    }
    return !is_probably_binary(entry.path());
}

bool should_descend_directory(const fs::directory_entry& entry) {
    std::error_code ec;
    return entry.is_directory(ec)
        && !entry.is_symlink(ec)
        && !is_hidden_name(entry.path())
        && !is_basic_ignored_directory(entry.path());
}

std::uint64_t searchable_file_size(const fs::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_symlink(ec) || !entry.is_regular_file(ec) || is_hidden_name(entry.path())) {
        return 0;
    }

    const std::uint64_t size = entry.file_size(ec);
    return ec ? 0 : size;
}

SearchStats estimate_searchable_stats(const fs::path& root) {
    std::error_code ec;
    const fs::directory_entry root_entry(root, ec);
    if (ec) {
        return {};
    }

    if (root_entry.is_regular_file(ec)) {
        const std::uint64_t bytes = searchable_file_size(root_entry);
        return bytes == 0 ? SearchStats{} : SearchStats{1, bytes, 0};
    }
    if (!should_descend_directory(root_entry)) {
        return {};
    }

    SearchStats total;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry entry = *it;
        if (entry.is_symlink(ec)) {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
        } else if (entry.is_directory(ec)) {
            if (!should_descend_directory(entry)) {
                it.disable_recursion_pending();
            }
        } else {
            const std::uint64_t bytes = searchable_file_size(entry);
            if (bytes > 0) {
                ++total.files_scanned;
                total.bytes_processed += bytes;
            }
        }

        it.increment(ec);
    }
    return total;
}

std::string format_match_line(const fs::path& path, std::uint64_t line_number, std::string_view line) {
    std::string result = path.string();
    result.push_back(':');
    result += std::to_string(line_number);
    result.push_back(':');
    result.append(line);
    return result;
}

void search_file(const AhoCorasick& ac, const fs::path& path, SearchOutput& output) {
    if (is_probably_binary(path)) {
        return;
    }

    std::error_code ec;
    const std::uint64_t file_size = fs::file_size(path, ec);

    std::ifstream in(path);
    if (!in) {
        return;
    }

    ++output.stats.files_scanned;
    if (!ec) {
        output.stats.bytes_processed += file_size;
    }
    std::string line;
    std::uint64_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (ac.contains_match(line)) {
            ++output.stats.matched_lines;
            output.lines.push_back(format_match_line(path, line_number, line));
        }
    }
}

void search_path(const AhoCorasick& ac, const fs::path& root, SearchOutput& output) {
    std::error_code ec;
    const fs::directory_entry root_entry(root, ec);
    if (ec) {
        return;
    }

    if (root_entry.is_symlink(ec)) {
        return;
    }
    if (root_entry.is_regular_file(ec)) {
        if (!is_hidden_name(root) && !is_probably_binary(root)) {
            search_file(ac, root, output);
        }
        return;
    }
    if (!root_entry.is_directory(ec)) {
        return;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry entry = *it;
        const fs::path path = entry.path();

        if (entry.is_symlink(ec)) {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
        } else if (entry.is_directory(ec)) {
            if (is_hidden_name(path) || is_basic_ignored_directory(path)) {
                it.disable_recursion_pending();
            }
        } else if (should_search_file(entry)) {
            search_file(ac, path, output);
        }

        it.increment(ec);
    }
}

std::uint64_t estimated_work_cost(const WorkUnit& unit);

void add_work_unit(std::vector<WorkUnit>& units, const fs::path& path) {
    const SearchStats estimate = estimate_searchable_stats(path);
    if (estimate.files_scanned > 0 || estimate.bytes_processed > 0) {
        units.push_back(WorkUnit{path, estimate.files_scanned, estimate.bytes_processed});
    }
}

std::vector<WorkUnit> make_work_units(const fs::path& root) {
    std::error_code ec;
    const fs::directory_entry root_entry(root, ec);
    if (ec) {
        throw std::runtime_error("failed to inspect " + root.string());
    }

    if (!root_entry.is_directory(ec)) {
        const SearchStats estimate = estimate_searchable_stats(root);
        return {WorkUnit{root, estimate.files_scanned, estimate.bytes_processed}};
    }

    std::vector<WorkUnit> units;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry entry = *it;
        const fs::path path = entry.path();

        if (entry.is_symlink(ec)) {
            it.increment(ec);
            continue;
        }
        if (entry.is_directory(ec) && !should_descend_directory(entry)) {
            it.increment(ec);
            continue;
        }

        if (entry.is_directory(ec)) {
            std::error_code child_ec;
            fs::directory_iterator child_it(path, fs::directory_options::skip_permission_denied, child_ec);
            const fs::directory_iterator child_end;
            bool added_child = false;
            while (!child_ec && child_it != child_end) {
                const fs::directory_entry child = *child_it;
                if (!child.is_symlink(child_ec)
                    && ((child.is_directory(child_ec) && should_descend_directory(child))
                        || child.is_regular_file(child_ec))) {
                    add_work_unit(units, child.path());
                    added_child = true;
                }
                child_it.increment(child_ec);
            }
            if (!added_child) {
                add_work_unit(units, path);
            }
        } else if (entry.is_regular_file(ec)) {
            add_work_unit(units, path);
        }

        it.increment(ec);
    }

    std::sort(units.begin(), units.end(), [](const WorkUnit& a, const WorkUnit& b) {
        const std::uint64_t a_cost = estimated_work_cost(a);
        const std::uint64_t b_cost = estimated_work_cost(b);
        if (a_cost != b_cost) {
            return a_cost > b_cost;
        }
        return a.path < b.path;
    });
    if (units.empty()) {
        const SearchStats estimate = estimate_searchable_stats(root);
        units.push_back(WorkUnit{root, estimate.files_scanned, estimate.bytes_processed});
    }
    return units;
}

std::uint64_t estimated_work_cost(const WorkUnit& unit) {
    constexpr std::uint64_t kPerFileCostBytes = 32768;
    return unit.estimated_bytes + unit.estimated_files * kPerFileCostBytes;
}

std::vector<std::vector<fs::path>> assign_work_units(const std::vector<WorkUnit>& units, std::size_t jobs) {
    jobs = std::max<std::size_t>(1, jobs);
    std::vector<std::vector<fs::path>> assignments(jobs);
    std::vector<std::uint64_t> assigned_cost(jobs, 0);

    for (const WorkUnit& unit : units) {
        const auto lightest = std::min_element(assigned_cost.begin(), assigned_cost.end());
        const std::size_t worker = static_cast<std::size_t>(lightest - assigned_cost.begin());
        assignments[worker].push_back(unit.path);
        assigned_cost[worker] += estimated_work_cost(unit);
    }

    return assignments;
}

SearchOutput search_single(const AhoCorasick& ac, const fs::path& root) {
    SearchOutput output;
    search_path(ac, root, output);
    return output;
}

SearchOutput search_assigned_roots(const AhoCorasick& ac, const std::vector<fs::path>& roots) {
    SearchOutput output;
    for (const fs::path& root : roots) {
        search_path(ac, root, output);
    }
    return output;
}

SearchOutput search_threads(const AhoCorasick& ac, const fs::path& root, std::size_t jobs) {
    const auto assignments = assign_work_units(make_work_units(root), jobs);
    std::vector<std::thread> threads;
    std::vector<SearchOutput> outputs(assignments.size());
    std::vector<WorkerReport> reports(assignments.size());
    threads.reserve(assignments.size());

    for (std::size_t i = 0; i < assignments.size(); ++i) {
        threads.emplace_back([&, i] {
            const auto start = std::chrono::steady_clock::now();
            outputs[i] = search_assigned_roots(ac, assignments[i]);
            const auto end = std::chrono::steady_clock::now();
            reports[i].worker_id = i;
            reports[i].stats = outputs[i].stats;
            reports[i].elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    SearchOutput combined;
    combined.workers = std::move(reports);
    for (SearchOutput& output : outputs) {
        combined.stats += output.stats;
        combined.lines.insert(
            combined.lines.end(),
            std::make_move_iterator(output.lines.begin()),
            std::make_move_iterator(output.lines.end()));
    }
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
    const auto assignments = assign_work_units(make_work_units(root), jobs);
    std::vector<pid_t> children;
    std::vector<int> read_fds;
    std::vector<fs::path> output_paths;
    children.reserve(assignments.size());
    read_fds.reserve(assignments.size());
    output_paths.reserve(assignments.size());

    const fs::path tmp_dir = fs::temp_directory_path();
    const auto parent_pid = static_cast<long long>(getpid());

    for (std::size_t i = 0; i < assignments.size(); ++i) {
        int fds[2];
        if (pipe(fds) != 0) {
            throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
        }

        fs::path child_output = tmp_dir / ("p-grep-" + std::to_string(parent_pid) + "-" + std::to_string(i) + ".out");
        const pid_t pid = fork();
        if (pid < 0) {
            throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
        }
        if (pid == 0) {
            close(fds[0]);
            try {
                const auto start = std::chrono::steady_clock::now();
                SearchOutput output = search_assigned_roots(ac, assignments[i]);
                const auto end = std::chrono::steady_clock::now();
                std::ofstream out(child_output);
                if (!out) {
                    _exit(1);
                }
                for (const std::string& line : output.lines) {
                    out << line << '\n';
                }
                out.close();
                WorkerReport report;
                report.worker_id = i;
                report.stats = output.stats;
                report.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
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
        output_paths.push_back(std::move(child_output));
    }

    SearchOutput combined;
    for (int fd : read_fds) {
        WorkerReport report;
        read_all(fd, &report, sizeof(report));
        close(fd);
        combined.stats += report.stats;
        combined.workers.push_back(report);
    }

    for (pid_t child : children) {
        int status = 0;
        if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            throw std::runtime_error("child process failed");
        }
    }

    for (const fs::path& output_path : output_paths) {
        std::ifstream in(output_path);
        std::string line;
        while (std::getline(in, line)) {
            combined.lines.push_back(line);
        }
        std::error_code ec;
        fs::remove(output_path, ec);
    }

    return combined;
#else
    (void)ac;
    (void)root;
    (void)jobs;
    throw std::runtime_error("--mode processes requires a POSIX platform");
#endif
}

void print_output(const SearchOutput& output) {
    for (const std::string& line : output.lines) {
        std::cout << line << '\n';
    }
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

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);

        const auto build_start = std::chrono::steady_clock::now();
        AhoCorasick ac = load_matcher(options.patterns_path);
        const auto build_end = std::chrono::steady_clock::now();

        const auto search_start = std::chrono::steady_clock::now();
        SearchOutput output;
        if (options.mode == "single") {
            output = search_single(ac, options.search_path);
        } else if (options.mode == "threads") {
            output = search_threads(ac, options.search_path, options.jobs);
        } else {
            output = search_processes(ac, options.search_path, options.jobs);
        }
        const auto search_end = std::chrono::steady_clock::now();

        print_output(output);

        if (options.timing) {
            const auto build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();
            const auto search_ms = std::chrono::duration<double, std::milli>(search_end - search_start).count();
            std::cerr << "patterns=" << ac.pattern_count()
                      << " files=" << output.stats.files_scanned
                      << " bytes=" << output.stats.bytes_processed
                      << " matched_lines=" << output.stats.matched_lines
                      << " mode=" << options.mode
                      << " jobs=" << options.jobs
                      << " build_ms=" << build_ms
                      << " search_ms=" << search_ms
                      << "\n";
            print_worker_reports(options.mode, output.workers);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
