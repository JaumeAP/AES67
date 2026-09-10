#pragma once

// Host stub for the Arduino core, enough to compile the library off the
// Teensy. Only the symbols the library actually uses are provided.

#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <ctime>
#include <math.h>

class SerialStub
{
public:
    void begin(unsigned long) {}

    // Not a template: with the format attribute the compiler checks the
    // conversions against the arguments, which is the only checking the
    // logging paths ever get. They are compiled with logging off, so
    // nothing else would ever look at them.
    int printf(const char *, ...) __attribute__((format(printf, 2, 3)));

    void print(const char *) {}
    void println(const char *) {}
    void println() {}
};

extern SerialStub Serial;

// A fake monotonic microsecond clock. Every call advances it by
// ptptest::state().microsStep, so the transmit-timestamp wait loops
// terminate without a real timer.
unsigned long micros();

// Milliseconds, and the pseudo-random source the Delay_Req pacing uses.
// Neither moves on its own: the tests set ptptest::state().millisNow and
// ptptest::state().randomValue.
unsigned long millis();
// The board's own signature: uint32_t, not long. A stub that took long
// hid a sign conversion that only the board's compiler saw.
uint32_t random(uint32_t howbig);

// i.MX RT ENET timer registers, read by the logging branch of
// updateController(). Plain variables here.
extern uint32_t ENET_ATINC;
extern uint32_t ENET_ATPER;
extern uint32_t ENET_ATCOR;

// The interrupt gate the PPS snapshot in update() takes. Nothing on the
// host runs in an interrupt, so both are no-ops here; the board's core
// defines them as macros.
inline void noInterrupts() {}
inline void interrupts() {}

class IPAddress
{
public:
    IPAddress() : octets{0, 0, 0, 0} {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : octets{a, b, c, d} {}

    uint8_t operator[](int i) const { return octets[i]; }

    bool operator==(const IPAddress &other) const
    {
        for (int i = 0; i < 4; i++)
        {
            if (octets[i] != other.octets[i])
            {
                return false;
            }
        }
        return true;
    }

private:
    uint8_t octets[4];
};
