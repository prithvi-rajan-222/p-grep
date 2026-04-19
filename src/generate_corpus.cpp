#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::filesystem::path data_dir = "data";
    std::size_t bytes = 32 * 1024 * 1024;
    std::size_t patterns = 256;
    std::size_t pattern_length = 8;
    std::uint32_t seed = 12345;
};

[[noreturn]] void usage(std::string_view message = {}) {
    if (!message.empty()) {
        std::cerr << "error: " << message << "\n\n";
    }
    std::cerr
        << "usage: generate-corpus [--data-dir DIR] [--bytes N] [--patterns N] [--pattern-length N] [--seed N]\n";
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

        if (arg == "--data-dir") {
            options.data_dir = require_value(arg);
        } else if (arg == "--bytes") {
            options.bytes = std::stoull(require_value(arg));
        } else if (arg == "--patterns") {
            options.patterns = std::stoull(require_value(arg));
        } else if (arg == "--pattern-length") {
            options.pattern_length = std::stoull(require_value(arg));
        } else if (arg == "--seed") {
            options.seed = static_cast<std::uint32_t>(std::stoul(require_value(arg)));
        } else if (arg == "--help" || arg == "-h") {
            usage();
        } else {
            usage("unknown argument " + std::string(arg));
        }
    }

    if (options.patterns == 0 || options.pattern_length == 0 || options.bytes == 0) {
        usage("bytes, patterns, and pattern-length must be greater than zero");
    }

    return options;
}

std::string random_word(std::mt19937& rng, std::size_t length) {
    static constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::string word;
    word.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        word.push_back(alphabet[pick(rng)]);
    }
    return word;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);
        std::filesystem::create_directories(options.data_dir);

        std::mt19937 rng(options.seed);
        std::vector<std::string> patterns;
        patterns.reserve(options.patterns);
        while (patterns.size() < options.patterns) {
            std::string candidate = random_word(rng, options.pattern_length);
            if (std::find(patterns.begin(), patterns.end(), candidate) == patterns.end()) {
                patterns.push_back(std::move(candidate));
            }
        }

        const auto patterns_path = options.data_dir / "patterns.txt";
        const auto input_path = options.data_dir / "corpus.txt";
        {
            std::ofstream out(patterns_path);
            for (const auto& pattern : patterns) {
                out << pattern << '\n';
            }
        }

        std::ofstream corpus(input_path, std::ios::binary);
        if (!corpus) {
            throw std::runtime_error("failed to open corpus for writing");
        }

        std::uniform_int_distribution<std::size_t> pick_pattern(0, patterns.size() - 1);
        std::uniform_int_distribution<int> pick_noise('a', 'z');
        std::uniform_int_distribution<int> pick_injection(0, 99);

        std::size_t written = 0;
        while (written < options.bytes) {
            std::string token;
            if (pick_injection(rng) < 8) {
                token = patterns[pick_pattern(rng)];
            } else {
                token = random_word(rng, options.pattern_length);
                for (char& ch : token) {
                    if (pick_injection(rng) == 0) {
                        ch = static_cast<char>(pick_noise(rng));
                    }
                }
            }
            token.push_back((written / 80) % 2 == 0 ? ' ' : '\n');
            if (written + token.size() > options.bytes) {
                token.resize(options.bytes - written);
            }
            corpus.write(token.data(), static_cast<std::streamsize>(token.size()));
            written += token.size();
        }

        std::cout << "patterns=" << patterns_path << "\n";
        std::cout << "input=" << input_path << "\n";
        std::cout << "bytes=" << options.bytes << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
