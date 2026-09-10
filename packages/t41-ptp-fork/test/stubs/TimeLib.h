#pragma once

#include <cstdint>
#include <ctime>

typedef struct
{
    uint8_t Second;
    uint8_t Minute;
    uint8_t Hour;
    uint8_t Wday;
    uint8_t Day;
    uint8_t Month;
    uint8_t Year;
} tmElements_t;

void breakTime(time_t t, tmElements_t &tm);
