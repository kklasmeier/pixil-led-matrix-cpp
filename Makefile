# Native Pixil runtime — build with GNU make + g++ (no other build dependency).
#
#   make                    release build  -> build/release/{pixil,pixil-expr,unit_tests}
#   make MODE=debug         -O0, libstdc++ assertions (bounds-checked containers)
#   make MODE=asan          AddressSanitizer + UndefinedBehaviorSanitizer
#   make MODE=tsan          ThreadSanitizer
#   make test               unit tests (any MODE)
#   make crosscheck         Tier 0 expression parity vs the legacy Python oracle
#   make crosscheck-negative  proves an injected fault is caught
#
# Build on the Pi 4 over SSH (never through the NFS mount): see README.md.

MODE ?= release
CXX ?= g++
PYTHON ?= python3
BUILD := build/$(MODE)

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Werror
# -ffp-contract=off: no fused multiply-add, so float results round exactly like CPython.
CXXFLAGS_COMMON := -std=c++20 $(WARN) -ffp-contract=off -Isrc -MMD -MP
LDFLAGS_COMMON := -pthread

ifeq ($(MODE),release)
  CXXFLAGS_MODE := -O2 -g
else ifeq ($(MODE),debug)
  CXXFLAGS_MODE := -O0 -g -D_GLIBCXX_ASSERTIONS -D_GLIBCXX_DEBUG
else ifeq ($(MODE),asan)
  CXXFLAGS_MODE := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
                   -fno-sanitize-recover=undefined -D_GLIBCXX_ASSERTIONS
  LDFLAGS_MODE := -fsanitize=address,undefined
else ifeq ($(MODE),tsan)
  CXXFLAGS_MODE := -O1 -g -fsanitize=thread
  LDFLAGS_MODE := -fsanitize=thread
else
  $(error unknown MODE=$(MODE) (release|debug|asan|tsan))
endif

CXXFLAGS += $(CXXFLAGS_COMMON) $(CXXFLAGS_MODE)
LDFLAGS += $(LDFLAGS_COMMON) $(LDFLAGS_MODE)

LIB_SRCS := $(wildcard src/runtime/*.cpp src/syntax/*.cpp src/util/*.cpp src/emit/*.cpp src/ipc/*.cpp) \
            $(filter-out src/host/main.cpp,$(wildcard src/host/*.cpp))
LIB_OBJS := $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
TEST_SRCS := $(wildcard tests/unit/*.cpp)
TEST_OBJS := $(TEST_SRCS:%.cpp=$(BUILD)/%.o)

PIXIL := $(BUILD)/pixil
PIXIL_CONSUMER := $(BUILD)/pixil-consumer
PIXIL_EXPR := $(BUILD)/pixil-expr
UNIT := $(BUILD)/unit_tests

# Optional hardware matrix library (Pi 4). Headless builds work without it.
RGBMATRIX_ROOT ?= $(HOME)/rpi-rgb-led-matrix
RGBMATRIX_LIB := $(wildcard $(RGBMATRIX_ROOT)/lib/librgbmatrix.a)
ifneq ($(RGBMATRIX_LIB),)
  CONSUMER_CXXFLAGS += -DPIXIL_HAS_RGBMATRIX -I$(RGBMATRIX_ROOT)/include
  CONSUMER_LDFLAGS += $(RGBMATRIX_LIB) -lpthread -lrt -lm -lpthread
endif

CONSUMER_SRCS := $(wildcard src/consumer/*.cpp)
CONSUMER_SRCS := $(filter-out src/consumer/main.cpp,$(CONSUMER_SRCS))
CONSUMER_LIB_OBJS := $(CONSUMER_SRCS:%.cpp=$(BUILD)/%.o)
CONSUMER_OBJS := $(CONSUMER_LIB_OBJS) $(BUILD)/src/consumer/main.o

.PHONY: all test crosscheck crosscheck-negative goldens check-docs \
        hello-headless hello-native-headless consumer-parity c5-ops clean
all: $(PIXIL) $(PIXIL_CONSUMER) $(PIXIL_EXPR) $(UNIT)

$(BUILD)/libpixil.a: $(LIB_OBJS)
	@mkdir -p $(@D)
	ar rcs $@ $^

$(PIXIL): $(BUILD)/src/host/main.o $(BUILD)/libpixil.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(PIXIL_CONSUMER): $(CONSUMER_OBJS)
	$(CXX) $^ $(LDFLAGS) $(CONSUMER_LDFLAGS) -o $@

$(PIXIL_EXPR): $(BUILD)/src/tools/pixil_expr.o $(BUILD)/libpixil.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(UNIT): $(TEST_OBJS) $(BUILD)/libpixil.a $(CONSUMER_LIB_OBJS)
	$(CXX) $^ $(LDFLAGS) $(CONSUMER_LDFLAGS) -o $@

$(BUILD)/src/consumer/%.o: src/consumer/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CONSUMER_CXXFLAGS) -c $< -o $@

$(BUILD)/tests/unit/test_consumer_batch.o: tests/unit/test_consumer_batch.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CONSUMER_CXXFLAGS) -Itests/unit -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Itests/unit -c $< -o $@

test: $(UNIT)
	$(UNIT)

crosscheck: $(PIXIL_EXPR)
	$(PYTHON) tools/expr_crosscheck.py --native $(PIXIL_EXPR)

crosscheck-negative: $(PIXIL_EXPR)
	$(PYTHON) tools/expr_crosscheck.py --native $(PIXIL_EXPR) --native-arg=--inject-fault=c_mod --expect-failure

# Regenerate oracle-derived artifacts from the legacy tree (Pi 4).
goldens:
	$(PYTHON) tools/gen_expr_cases.py > tests/expr/cases.jsonl
	$(PYTHON) tools/gen_draw_batch_goldens.py > tests/golden/draw_batch.jsonl
	$(PYTHON) tools/gen_command_dialect.py

check-docs:
	$(PYTHON) tools/gen_command_dialect.py --check

hello-headless: $(PIXIL)
	$(PIXIL) --hello --headless --seconds 3

hello-native-headless: $(PIXIL) $(PIXIL_CONSUMER)
	$(PIXIL) --hello --headless --consumer native --seconds 3

# Phase 2 C5: Python vs native drawing-buffer fingerprints (Pi 4, headless).
consumer-parity: $(PIXIL) $(PIXIL_CONSUMER)
	$(PYTHON) -c 'import os,sys; print("parity via shell")' >/dev/null
	PIXIL=$(PIXIL) bash tools/consumer_parity_compare.sh

c5-ops: $(PIXIL) $(PIXIL_CONSUMER)
	PIXIL=$(PIXIL) bash tools/verify_c5_ops.sh

clean:
	rm -rf build

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(CONSUMER_OBJS:.o=.d) $(BUILD)/src/host/main.d $(BUILD)/src/tools/pixil_expr.d
