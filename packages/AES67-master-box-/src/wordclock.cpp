#include "wordclock.h"

#include <EEPROM.h>
#include <QNEthernet.h>

// Pin map, taken from the Teensy core's tables (cores/teensy4/pwm.c), not
// from memory:
//
//   pin 14 = pad AD_B1_02 = QuadTimer3 channel 2, ALT1
//   pin 18 = pad AD_B1_01 = QuadTimer3 channel 1, ALT1
//   pin 19 = pad AD_B1_00 = QuadTimer3 channel 0, ALT1
//   pin 15 = pad AD_B1_03 = QuadTimer3 channel 3 (ALT1) or ENET_1588_EVENT2_IN
//            (ALT4). We want it as the 1588 capture input, that is ALT4, and
//            that is why the divider canNOT put the pulse out on this pin:
//            hence the physical bridge from 19 to 15.

// The QuadTimer's PCS (primary count source): 0..3 select the INPUT pin of
// channels 0..3, and 4..7 the OUTPUT of channels 0..3. That is what makes all
// of this possible: a channel can count another channel's pin without that
// channel having to do anything.
static const uint16_t kPcsCh2InputPin = 2;
static const uint16_t kPcsCh1Output   = 5;

// CM=1: count rising edges of the primary source.
static const uint16_t kCountModeRising = 1;

// OUTMODE=4: count to the compare, restart, and toggle the output alternating
// between COMP1 and COMP2. It is the mode NXP uses to get PWM out of the
// QuadTimer, and the only one that gives a real pulse train: the output is
// high for COMP1+1 counts and low for COMP2+1, that is one rising edge every
// COMP1+COMP2+2 counts.
//
// Do NOT use OUTMODE=6 ("set on compare, cleared on counter rollover"), even
// though it looks like the direct way: there is a documented erratum saying
// the output is NOT cleared on rollover when the counter runs up, which is
// exactly our case. It would stay high forever.
static const uint16_t kOutModeAltCompare = 4;

// Output pulse width, in counts of the first stage. With the word clock at
// 48 kHz and the first stage dividing by 100, each count is 2.08 ms, so
// 2 counts make a pulse of some 4 ms. It scales with the rate: at 192 kHz the
// same 2 counts are about 1 ms. Plenty for the 1588 capture at either end and
// short enough not to come near anything.
static const uint32_t kPulseCounts = 2;

// The QuadTimer's counters are 16 bits, so 48000 cannot just be dropped in
// and left to run. 96000, 176400 and 192000 do not even fit.
//
// That is why the division is done in two cascaded stages: first by 100, then
// by the rest. Every standard rate is a multiple of 100 (441, 480, 882, 960,
// 1764, 1920), and neither factor goes past 16 bits.
static const uint32_t kFirstStage = 100;

// The rates offered. Every one of them is a multiple of kFirstStage and has a
// second factor that fits in 16 bits, which is what the divider needs; the
// list is closed for that reason and not out of caution.
static const uint32_t kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
static const size_t kRateCount = sizeof(kRates) / sizeof(kRates[0]);

// 48000, which is what the code carried hardcoded before the rate could be
// picked.
static const size_t kRateDefaultIndex = 1;

// The block that gets stored, the same shape and the same reasoning as the
// profile selection in src/profiles.cpp: the signature and the version are
// there so that a virgin EEPROM (0xff everywhere) or one written by another
// version of the program is not mistaken for a valid choice.
struct StoredRate
{
  uint32_t magic;
  uint8_t version;
  uint8_t index;
  uint16_t check;
};

static const uint32_t kRateMagic = 0x41455357;  // "AESW"
static const uint8_t kRateVersion = 1;

// The profile selection sits at address 0 and takes eight bytes. This one
// goes at 16 so that adding a field there does not silently land on top of
// this.
static const int kRateEepromAddress = 16;

static uint16_t rateCheck(const StoredRate &s)
{
  return (uint16_t)((s.magic & 0xffff) ^ (s.magic >> 16) ^ (s.version << 8) ^ s.index);
}

size_t wordclockRateCount()
{
  return kRateCount;
}

uint32_t wordclockRateAt(size_t index)
{
  if(index >= kRateCount){
    index = wordclockRateDefaultIndex();
  }
  return kRates[index];
}

size_t wordclockRateDefaultIndex()
{
  return kRateDefaultIndex;
}

size_t wordclockRateLoadSelection()
{
  StoredRate stored;
  EEPROM.get(kRateEepromAddress, stored);

  if(stored.magic != kRateMagic || stored.version != kRateVersion){
    return wordclockRateDefaultIndex();
  }
  if(stored.check != rateCheck(stored)){
    return wordclockRateDefaultIndex();
  }
  if(stored.index >= kRateCount){
    return wordclockRateDefaultIndex();
  }
  return stored.index;
}

bool wordclockRateSaveSelection(size_t index)
{
  if(index >= kRateCount){
    return false;
  }

  StoredRate stored;
  stored.magic = kRateMagic;
  stored.version = kRateVersion;
  stored.index = (uint8_t)index;
  stored.check = rateCheck(stored);

  // put() already avoids rewriting the bytes that do not change, which on
  // the Teensy's emulated EEPROM is not cosmetic: every write wears it.
  EEPROM.put(kRateEepromAddress, stored);

  // And it is read back, because storing without checking is not storing.
  StoredRate back;
  EEPROM.get(kRateEepromAddress, back);
  return back.magic == stored.magic &&
         back.version == stored.version &&
         back.index == stored.index &&
         back.check == stored.check;
}

bool wordclockDividerBegin(uint32_t rateHz)
{
  const uint32_t rate = rateHz;
  if(rate == 0 || (rate % kFirstStage) != 0){
    return false;
  }
  const uint32_t second = rate / kFirstStage;
  if(second == 0 || second > 0x10000UL){
    return false;
  }

  CCM_CCGR6 |= CCM_CCGR6_QTIMER3(CCM_CCGR_ON);

  // Pin 14 as the channel 2 input. Channel 2 does not have to be started for
  // the division: the PCS takes the signal off the pad directly.
  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_02 = 1;
  // Schmitt trigger on the input. The incoming word clock may have soft edges
  // and without hysteresis we would count bounces.
  IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_B1_02 = IOMUXC_PAD_HYS;

  // Pin 19 as the channel 0 output.
  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_00 = 1;

  IMXRT_TMR_t *t = &IMXRT_TMR3;

  // With alternating compares the period is COMP1+COMP2+2, so the values are
  // split like this. The first stage does 50 and 50 to give a rising edge
  // every 100 counts; the second splits them to put out a short pulse.
  if(second < kPulseCounts + 2){
    return false;
  }
  const uint16_t firstComp1 = (uint16_t)(kFirstStage / 2 - 1);
  const uint16_t firstComp2 = (uint16_t)(kFirstStage - kFirstStage / 2 - 1);
  const uint16_t secondComp1 = (uint16_t)(kPulseCounts - 1);
  const uint16_t secondComp2 = (uint16_t)(second - kPulseCounts - 1);

  // First stage: channel 1 divides the word clock by 100. Its output does not
  // reach any pin, it is only used internally as channel 0's source.
  t->CH[1].CTRL = 0;
  t->CH[1].LOAD = 0;
  t->CH[1].CNTR = 0;
  t->CH[1].COMP1 = firstComp1;
  t->CH[1].COMP2 = firstComp2;
  t->CH[1].CMPLD1 = firstComp1;
  t->CH[1].CMPLD2 = firstComp2;
  t->CH[1].SCTRL = 0;
  t->CH[1].CTRL = TMR_CTRL_CM(kCountModeRising)
                | TMR_CTRL_PCS(kPcsCh2InputPin)
                | TMR_CTRL_LENGTH
                | TMR_CTRL_OUTMODE(kOutModeAltCompare);

  // Second stage: channel 0 counts channel 1's output and puts the pulse out
  // on the pin.
  t->CH[0].CTRL = 0;
  t->CH[0].LOAD = 0;
  t->CH[0].CNTR = 0;
  t->CH[0].COMP1 = secondComp1;
  t->CH[0].COMP2 = secondComp2;
  t->CH[0].CMPLD1 = secondComp1;
  t->CH[0].CMPLD2 = secondComp2;
  t->CH[0].SCTRL = TMR_SCTRL_OEN;   // this one does reach the pad
  t->CH[0].CTRL = TMR_CTRL_CM(kCountModeRising)
                | TMR_CTRL_PCS(kPcsCh1Output)
                | TMR_CTRL_LENGTH
                | TMR_CTRL_OUTMODE(kOutModeAltCompare);

  return true;
}

uint32_t wordclockMeasureHz()
{
  CCM_CCGR6 |= CCM_CCGR6_QTIMER3(CCM_CCGR_ON);
  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_02 = 1;
  IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_B1_02 = IOMUXC_PAD_HYS;

  IMXRT_TMR_t *t = &IMXRT_TMR3;

  // Channel 2 free-runs counting its own pin, with no compare. Running it
  // alongside the division does no harm: channel 1 reads the same signal off
  // the pad, not this counter.
  t->CH[2].CTRL = 0;
  t->CH[2].LOAD = 0;
  t->CH[2].CNTR = 0;
  t->CH[2].COMP1 = 0xffff;
  t->CH[2].SCTRL = 0;
  t->CH[2].CTRL = TMR_CTRL_CM(kCountModeRising)
                | TMR_CTRL_PCS(kPcsCh2InputPin);

  // A 200 ms window, not a one-second one. The counter is 16 bits and at
  // 192 kHz one second would give 192000 edges, which wraps three times and
  // cannot be disambiguated. With 200 ms the range runs from 8820 to 38400
  // edges, which fits with room to spare for every standard rate.
  const uint32_t windowMs = 200;

  const uint16_t before = t->CH[2].CNTR;
  const uint32_t t0 = millis();
  while((uint32_t)(millis() - t0) < windowMs){
  }
  const uint16_t after = t->CH[2].CNTR;

  const uint16_t edges = (uint16_t)(after - before);  // the wrap works out
  if(edges == 0){
    return 0;
  }
  return ((uint32_t)edges * 1000UL) / windowMs;
}
