# Host unit tests for src/ptp. Nothing here runs on the
# Teensy: stubs/ replaces Arduino, QNEthernet and TimeLib so the file
# compiles and runs on the development machine.
#
#   make        build and run
#   make board  compile the example for the Teensy 4.1 (needs PlatformIO
#               and the submodules; see ../ci/platformio.ini)
#   make clean

CXX ?= c++
# -Wconversion and -Wsign-conversion are here and not only in CI because the
# board's compiler sees narrowings this one does not: long is 32 bits there
# and 64 here. Keeping the host build clean under them is what stops a
# truncation from reaching the Teensy unnoticed.
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -O0 -g -I stubs -I ../src

# Set by CI to -Werror. A warning that only appears on another compiler
# should fail there, not be argued about later.
EXTRA_CXXFLAGS ?=

# PlatformIO, for the board target only. Nothing else here needs it.
PIO ?= pio

BIN = t41-ptp-test
SRC = ../src/ptp/ptp-base.cpp ../src/ptp/ptp-config.cpp ../src/ptp/ptp-discipline.cpp \
      ../src/ptp/ptp-parse.cpp ../src/ptp/ptp-send.cpp \
      ../src/ptp/ptp-servo.cpp ../src/ptp/l2ptp.cpp ../src/ptp/l3ptp.cpp \
      stubs/stubs.cpp ptp_messages.cpp test_ptp_base.cpp test_transport.cpp \
      test_servo.cpp test_main.cpp

.PHONY: tidy cppcheck fuzz fuzz-libfuzzer coverage all run board clean

# First target in the file, and it has to stay first: make takes the first
# rule as the default goal, and .PHONY does not change that. When the tidy
# rule sat above this one, a plain `make` ran clang-tidy and never built the
# tests -- and the gate's host-test step ran no test at all while reporting
# that it had.
all: run

run: $(BIN)
	./$(BIN)

# clang-tidy over the library sources, with the same flags and stubs the host
# build uses, so no compile database is needed. The checks are the .clang-tidy
# at the package root, shared with the other packages here. Opt-in through the
# gate (AES67_ANALYSE=1); it wants a clang-tidy the Command Line Tools do not
# ship, found the way the other packages find it.
TIDY ?= $(firstword $(wildcard $(HOME)/.local/venvs/cpptools/bin/clang-tidy) $(shell command -v clang-tidy))
# -isysroot only where there is one to point at: xcrun is macOS's, and on Linux
# the same line ran clang-tidy with an empty sysroot, which is why this target
# had never run anywhere but here.
SYSROOT_FLAGS = $(if $(shell command -v xcrun),-isysroot $(shell xcrun --show-sdk-path),)
tidy:
	@if [ -z "$(TIDY)" ]; then echo "SKIP: no clang-tidy found"; else \
	  echo "$(TIDY) --quiet ../src/ptp/*.cpp"; \
	  $(TIDY) --quiet ../src/ptp/*.cpp -- $(CXXFLAGS) $(SYSROOT_FLAGS); fi

# cppcheck, where there is one. It overlaps clang-tidy and does not replace it:
# what it adds is a second opinion from a different engine, and it reads the
# library alone rather than a translation unit, so it sees code no test
# instantiates. Same posture as tidy above -- opt-in, and quiet about being
# absent, because the machine this is written on has no package manager to
# install it with.
CPPCHECK ?= $(shell command -v cppcheck)
cppcheck:
	@if [ -z "$(CPPCHECK)" ]; then echo "SKIP: no cppcheck found"; else \
	  $(CPPCHECK) --quiet --error-exitcode=1 --enable=warning,performance,portability \
	    --inline-suppr --std=c++17 --language=c++ \
	    --suppress=missingInclude --suppress=missingIncludeSystem \
	    -I ../src -I stubs ../src/ptp; fi

# The parser, fed what nobody thought to write a test for. See fuzz/fuzz_parse.cpp
# for why there are two builds of one function.
#
#   make fuzz              the deterministic driver, everywhere
#   make fuzz ITER=1000000 longer
#   make fuzz-libfuzzer    coverage-guided, needs a clang with the fuzzer runtime
FUZZ_SRC = fuzz/fuzz_parse.cpp ../src/ptp/ptp-base.cpp ../src/ptp/ptp-config.cpp \
           ../src/ptp/ptp-discipline.cpp ../src/ptp/ptp-parse.cpp ../src/ptp/ptp-send.cpp \
           ../src/ptp/ptp-servo.cpp stubs/stubs.cpp ptp_messages.cpp
FUZZ_FLAGS = -std=c++17 -Wall -Wextra -O1 -g -I stubs -I ../src -I .
ITER ?= 200000
FUZZ_TIME ?= 60

fuzz:
	$(CXX) $(FUZZ_FLAGS) -fsanitize=address,undefined $(FUZZ_SRC) -o t41-ptp-fuzz
	./t41-ptp-fuzz $(ITER)
	@rm -f t41-ptp-fuzz

fuzz-libfuzzer:
	@$(CXX) $(FUZZ_FLAGS) -DFUZZ_LIBFUZZER -fsanitize=fuzzer,address,undefined \
	  $(FUZZ_SRC) -o t41-ptp-fuzz-guided 2> /dev/null || \
	  { echo "SKIP: this compiler has no libFuzzer runtime"; exit 0; }
	@test -x t41-ptp-fuzz-guided || exit 0; \
	  mkdir -p fuzz/corpus && \
	  ./t41-ptp-fuzz-guided fuzz/corpus -max_total_time=$(FUZZ_TIME) -max_len=4096 \
	    -print_final_stats=1
	@rm -f t41-ptp-fuzz-guided

# Line coverage of the library, from the host tests. Not a gate: it is a number
# to look at when deciding what to test next, and a threshold nobody chose is a
# number people learn to ignore.
GCOV ?= $(if $(shell command -v xcrun),xcrun llvm-cov gcov,gcov)
coverage:
	@rm -rf coverage && mkdir -p coverage
	$(CXX) $(CXXFLAGS) --coverage -o t41-ptp-coverage $(SRC)
	./t41-ptp-coverage > /dev/null
	@$(GCOV) *.gcda > /dev/null 2>&1 || true
	@mv -f *.gcov coverage/ 2> /dev/null || true
	@LC_ALL=C awk -F: '/^ *#####:/ { miss++ } /^ *[0-9]+[*]?:/ { hit++ } \
	  END { total = hit + miss; \
	        if (total == 0) { print "coverage: nothing measured"; exit 1 } \
	        printf "coverage: %d/%d lines of src/ptp, %.1f%%\n", hit, total, 100 * hit / total }' \
	  coverage/ptp-base.cpp.gcov coverage/ptp-config.cpp.gcov \
	  coverage/ptp-discipline.cpp.gcov coverage/ptp-parse.cpp.gcov \
	  coverage/ptp-send.cpp.gcov coverage/ptp-servo.cpp.gcov \
	  coverage/l2ptp.cpp.gcov coverage/l3ptp.cpp.gcov
	@rm -f t41-ptp-coverage *.gcda *.gcno

HDR = stubs/Arduino.h stubs/QNEthernet.h stubs/TimeLib.h stubs/stub_state.h \
      ptp_messages.h test_harness.h \
      ../src/ptp/ptp-base.h ../src/ptp/ptp-servo.h ../src/ptp/l2ptp.h ../src/ptp/l3ptp.h

$(BIN): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -o $@ $(SRC)

# The same build CI runs: a change that only compiles against the host
# stubs cannot pass unnoticed. An .ino gets its prototypes from the
# Arduino preprocessor; the .cpp handed to PlatformIO needs them written
# out.
board: ../ci/src/main.cpp
	$(PIO) run -d ../ci

../ci/src/main.cpp: ../examples/PTPNode/PTPNode.ino
	{ printf '#include <Arduino.h>\n'; \
	  printf 'void syncInterrupt();\n'; \
	  printf 'void announceInterrupt();\n'; \
	  printf 'static void interrupt_1588_timer();\n'; \
	  cat $<; } > $@

clean:
	rm -rf $(BIN) $(BIN).dSYM ../ci/src/main.cpp ../ci/.pio \
	  t41-ptp-fuzz t41-ptp-fuzz.dSYM t41-ptp-fuzz-guided t41-ptp-fuzz-guided.dSYM \
	  t41-ptp-coverage t41-ptp-coverage.dSYM coverage *.gcda *.gcno *.gcov
