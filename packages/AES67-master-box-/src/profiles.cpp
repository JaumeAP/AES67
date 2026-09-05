#include "profiles.h"

#include <EEPROM.h>

// The list. The order matters: the first one is what gets picked when
// nothing is stored, and it is exactly what the box did before profiles
// existed, so putting this feature to use changes no behaviour.
static const PtpProfile kProfiles[] = {
  {
    "master-box",
    "Master box (default)",
    "The usual one: 8 sync per second, announce every second, domain 0.",
    0,      // domain
    -3,     // sync, 8 per second
    0,      // announce, 1 per second
    128,    // priority1, neutral
    128,    // priority2, neutral
    100,    // locked below 100 ns
  },
  {
    "conservative",
    "Conservative",
    "1 sync per second and announce every 2 s: less traffic, less precision.",
    0,
    0,      // sync, 1 per second
    1,      // announce, every 2 s
    128,
    128,
    100,
  },
  {
    "tight",
    "Tight",
    "16 sync per second and a 50 ns lock window, for well kept networks.",
    0,
    -4,     // sync, 16 per second
    0,
    128,
    128,
    50,
  },
  {
    "standby",
    "Standby",
    "Like the default but with priority1 200: it does not win if another is there.",
    0,
    -3,
    0,
    200,    // the BMCA looks at priority1 before the clockClass
    128,
    100,
  },
};

static const size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

// The block that gets stored. The signature and the version are there so
// that a virgin EEPROM (0xff everywhere) or one written by another version of
// the program is not mistaken for a valid choice.
struct StoredSelection
{
  uint32_t magic;
  uint8_t version;
  uint8_t index;
  uint16_t check;
};

static const uint32_t kMagic = 0x41455337;  // "AES7"
static const uint8_t kVersion = 1;
static const int kEepromAddress = 0;

static uint16_t selectionCheck(const StoredSelection &s)
{
  return (uint16_t)((s.magic & 0xffff) ^ (s.magic >> 16) ^ (s.version << 8) ^ s.index);
}

size_t profileCount()
{
  return kProfileCount;
}

const PtpProfile &profileAt(size_t index)
{
  if(index >= kProfileCount){
    index = profileDefaultIndex();
  }
  return kProfiles[index];
}

size_t profileDefaultIndex()
{
  return 0;
}

size_t profileLoadSelection()
{
  StoredSelection stored;
  EEPROM.get(kEepromAddress, stored);

  if(stored.magic != kMagic || stored.version != kVersion){
    return profileDefaultIndex();
  }
  if(stored.check != selectionCheck(stored)){
    return profileDefaultIndex();
  }
  if(stored.index >= kProfileCount){
    return profileDefaultIndex();
  }
  return stored.index;
}

bool profileSaveSelection(size_t index)
{
  if(index >= kProfileCount){
    return false;
  }

  StoredSelection stored;
  stored.magic = kMagic;
  stored.version = kVersion;
  stored.index = (uint8_t)index;
  stored.check = selectionCheck(stored);

  // put() already avoids rewriting the bytes that do not change, which on
  // the Teensy's emulated EEPROM is not cosmetic: every write wears it.
  EEPROM.put(kEepromAddress, stored);

  // And it is read back, because storing without checking is not storing.
  StoredSelection back;
  EEPROM.get(kEepromAddress, back);
  return back.magic == stored.magic &&
         back.version == stored.version &&
         back.index == stored.index &&
         back.check == stored.check;
}

uint32_t profileSyncIntervalUs(const PtpProfile &profile)
{
  // The interval is 2^logSyncInterval seconds. The values we use run from
  // -4 (16 per second) to +1 (every 2 s), and the shift stays inside a
  // uint32_t with room to spare.
  const int8_t l = profile.logSyncInterval;
  if(l < 0){
    return 1000000u >> (unsigned)(-l);
  }
  return 1000000u << (unsigned)l;
}

int profileSyncTicksPerSecond(const PtpProfile &profile)
{
  const int8_t l = profile.logSyncInterval;
  if(l < 0){
    return 1 << (unsigned)(-l);
  }
  // With a sync every 2 s or slower, fewer than one tick fits in a second.
  // 1 is returned so the PPS loss threshold does not come out zero; the price
  // is that the threshold, expressed in seconds, ends up longer than it says.
  // Late beats a false negative every second.
  return 1;
}
