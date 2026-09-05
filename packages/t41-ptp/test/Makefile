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
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wconversion -Wsign-conversion -O0 -g -I stubs -I ../src

# Set by CI to -Werror. A warning that only appears on another compiler
# should fail there, not be argued about later.
EXTRA_CXXFLAGS ?=

# PlatformIO, for the board target only. Nothing else here needs it.
PIO ?= pio

BIN = t41-ptp-test
SRC = ../src/ptp/ptp-base.cpp ../src/ptp/ptp-servo.cpp ../src/ptp/l2ptp.cpp ../src/ptp/l3ptp.cpp \
      stubs/stubs.cpp ptp_messages.cpp test_ptp_base.cpp test_transport.cpp \
      test_servo.cpp test_main.cpp

.PHONY: all run board clean

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
