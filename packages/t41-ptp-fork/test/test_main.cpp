#include <cstdio>

int checksRun = 0;
int failures = 0;

void runPtpBaseTests();
void runTransportTests();
void runServoTests();

int main()
{
    runPtpBaseTests();
    runTransportTests();
    runServoTests();

    std::printf("%d checks, %d failures\n", checksRun, failures);
    return failures == 0 ? 0 : 1;
}
