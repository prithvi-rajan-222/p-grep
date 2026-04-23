#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

class AhoCorasick {
public:
    struct Match {
        std::size_t end;
        std::uint32_t pattern_id;
    };

    AhoCorasick() {
        trie_.push_back(Node{});
    }

    void add_pattern(std::string_view pattern) {
        if (pattern.empty()) {
            return;
        }

        std::uint32_t v = 0;
        for (unsigned char ch : pattern) {
            std::uint32_t& next = trie_[v].next[ch];
            if (next == kMissing) {
                next = static_cast<std::uint32_t>(trie_.size());
                trie_.push_back(Node{});
            }
            v = next;
        }

        trie_[v].out.push_back(static_cast<std::uint32_t>(pattern_lengths_.size()));
        pattern_lengths_.push_back(pattern.size());
        if (pattern.size() > max_pattern_length_) {
            max_pattern_length_ = pattern.size();
        }
    }

    void build() {
        std::queue<std::uint32_t> q;

        for (std::uint32_t c = 0; c < kAlphabet; ++c) {
            std::uint32_t u = trie_[0].next[c];
            if (u == kMissing) {
                trie_[0].next[c] = 0;
            } else {
                trie_[u].link = 0;
                q.push(u);
            }
        }

        while (!q.empty()) {
            std::uint32_t v = q.front();
            q.pop();

            const std::uint32_t suffix = trie_[v].link;
            const auto suffix_out = trie_[suffix].out;
            trie_[v].out.insert(trie_[v].out.end(), suffix_out.begin(), suffix_out.end());

            for (std::uint32_t c = 0; c < kAlphabet; ++c) {
                std::uint32_t u = trie_[v].next[c];
                if (u == kMissing) {
                    trie_[v].next[c] = trie_[suffix].next[c];
                } else {
                    trie_[u].link = trie_[suffix].next[c];
                    q.push(u);
                }
            }
        }

        publish_hot_tables();
        built_ = true;
    }

    [[nodiscard]] std::uint64_t count_matches(std::string_view text) const {
        std::uint64_t count = 0;
        std::uint32_t state = 0;

        for (unsigned char ch : text) {
            state = transitions_[state][ch];
            count += outputs_[state].size();
        }

        return count;
    }

    [[nodiscard]] bool contains_match(std::string_view text) const {
        std::uint32_t state = 0;

        for (unsigned char ch : text) {
            state = transitions_[state][ch];
            if (has_output_[state]) {
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] std::uint32_t start_state() const {
        return 0;
    }

    [[nodiscard]] std::uint32_t step(std::uint32_t state, unsigned char ch) const {
        return transitions_[state][ch];
    }

    [[nodiscard]] bool is_match_state(std::uint32_t state) const {
        return has_output_[state];
    }

    [[nodiscard]] std::uint64_t count_matches_in_range(
        std::string_view text,
        std::size_t valid_begin,
        std::size_t valid_end) const {
        std::uint64_t count = 0;
        std::uint32_t state = 0;

        for (std::size_t i = 0; i < text.size(); ++i) {
            state = transitions_[state][static_cast<unsigned char>(text[i])];
            for (std::uint32_t pattern_id : outputs_[state]) {
                const std::size_t len = pattern_lengths_[pattern_id];
                if (i + 1 < len) {
                    continue;
                }
                const std::size_t start = i + 1 - len;
                if (start >= valid_begin && start < valid_end) {
                    ++count;
                }
            }
        }

        return count;
    }

    [[nodiscard]] std::size_t pattern_count() const {
        return pattern_lengths_.size();
    }

    [[nodiscard]] std::size_t max_pattern_length() const {
        return max_pattern_length_;
    }

    [[nodiscard]] bool built() const {
        return built_;
    }

private:
    static constexpr std::uint32_t kAlphabet = 256;
    static constexpr std::uint32_t kMissing = UINT32_MAX;

    struct Node {
        std::array<std::uint32_t, kAlphabet> next{};
        std::uint32_t link = 0;
        std::vector<std::uint32_t> out;

        Node() {
            next.fill(kMissing);
        }
    };

    void publish_hot_tables() {
        transitions_.clear();
        outputs_.clear();
        has_output_.clear();
        transitions_.reserve(trie_.size());
        outputs_.reserve(trie_.size());
        has_output_.reserve(trie_.size());

        for (const Node& node : trie_) {
            transitions_.push_back(node.next);
            outputs_.push_back(node.out);
            has_output_.push_back(node.out.empty() ? 0 : 1);
        }
    }

    std::vector<Node> trie_;
    std::vector<std::array<std::uint32_t, kAlphabet>> transitions_;
    std::vector<std::vector<std::uint32_t>> outputs_;
    std::vector<std::uint8_t> has_output_;
    std::vector<std::size_t> pattern_lengths_;
    std::size_t max_pattern_length_ = 0;
    bool built_ = false;
};
