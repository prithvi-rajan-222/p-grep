CXX := $(shell command -v /opt/homebrew/bin/g++-15 2>/dev/null || echo c++)
CXXFLAGS ?= -std=gnu++20 -O3 -DNDEBUG -Wall -Wextra -pedantic
LDFLAGS ?=

BIN_DIR := bin
SRC_DIR := src
INCLUDE_DIR := include

.PHONY: all clean bench

all: $(BIN_DIR)/p-grep $(BIN_DIR)/generate-corpus

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/p-grep: $(SRC_DIR)/p_grep.cpp $(INCLUDE_DIR)/aho_corasick.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/generate-corpus: $(SRC_DIR)/generate_corpus.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

bench: all
	python3 scripts/bench.py

clean:
	rm -rf $(BIN_DIR) data
