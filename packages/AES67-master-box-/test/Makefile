# Host unit tests for what this repository owns: the AES67 tone sender and
# the IS-04 registration. Nothing here runs on the Teensy.
#
# The stubs for Arduino, QNEthernet and TimeLib, the check macros and the
# PTPv2 message builders are the library's, used from where they live
# rather than copied: lib/t41-ptp/test. The library's own sources are
# compiled in too, because both modules are written against PTPBase.
#
#   make        build and run
#   make clean

CXX ?= c++

LIB = ../lib/t41-ptp

# -Wconversion and -Wsign-conversion are here and not only in CI because
# the board's compiler sees narrowings this one does not: long is 32 bits
# there and 64 here.
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wconversion -Wsign-conversion -O0 -g \
            -I $(LIB)/test/stubs -I $(LIB)/test -I $(LIB)/src -I ../src

# Set by CI to -Werror.
EXTRA_CXXFLAGS ?=

BIN = aes67-master-box-test

SRC = ../src/audio/tone-sender.cpp ../src/nmos/nmos-node.cpp \
      $(LIB)/src/ptp/ptp-base.cpp $(LIB)/src/ptp/ptp-servo.cpp \
      $(LIB)/src/ptp/l2ptp.cpp $(LIB)/src/ptp/l3ptp.cpp \
      $(LIB)/test/stubs/stubs.cpp $(LIB)/test/ptp_messages.cpp \
      test_tone.cpp test_nmos.cpp test_main.cpp

HDR = ../src/audio/tone-sender.h ../src/nmos/nmos-node.h \
      $(LIB)/src/ptp/ptp-base.h $(LIB)/test/test_harness.h \
      $(LIB)/test/ptp_messages.h $(LIB)/test/stubs/stub_state.h

.PHONY: all run clean

all: run

run: $(BIN)
	./$(BIN)

$(BIN): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -o $@ $(SRC)

clean:
	rm -rf $(BIN) $(BIN).dSYM
