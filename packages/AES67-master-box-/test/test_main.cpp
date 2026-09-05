//
// test_main.cpp
// Runs what this repository owns. The library's own suites run in the
// library: make -C lib/t41-ptp/test.
//
#include <cstdio>

int checksRun = 0;
int failures = 0;

void runToneTests();
void runNmosTests();

int main()
{
    runToneTests();
    runNmosTests();

    std::printf("%d checks, %d failures\n", checksRun, failures);
    return failures == 0 ? 0 : 1;
}
