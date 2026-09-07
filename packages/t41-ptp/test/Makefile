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
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wconversion -Wsign-conversion -O0 -g -I stubs -I ../src -I ../../aes67-profiles

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

# clang-tidy over the library sources, with the same flags and stubs the host
# build uses, so no compile database is needed. The checks are the .clang-tidy
# at the package root, shared with the other packages here. Opt-in through the
# gate (AES67_ANALYSE=1); it wants a clang-tidy the Command Line Tools do not
# ship, found the way the other packages find it.
TIDY ?= $(firstword $(wildcard $(HOME)/.local/venvs/cpptools/bin/clang-tidy) $(shell command -v clang-tidy))
tidy:
	@test -n "$(TIDY)" || { echo "SKIP: no clang-tidy found"; exit 0; }
	$(TIDY) --quiet ../src/ptp/*.cpp -- $(CXXFLAGS) -isysroot $$(xcrun --show-sdk-path) -I ../../aes67-profiles

.PHONY: tidy all run board clean

all: run

run: $(BIN)
	./$(BIN)

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
	rm -rf $(BIN) $(BIN).dSYM ../ci/src/main.cpp ../ci/.pio
