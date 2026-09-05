// AES67-MasterBox -- PTP grandmaster on a Teensy 4.1.
//
// Starting point: the t41-ptp library's PTPMaster example, adapted from an
// .ino sketch to C++ (prototypes are needed, the Arduino IDE generates them
// on its own). It has nothing AES67-specific yet: it is the bare PTP master,
// disciplined by the PPS coming in on pin 15.
//
// Pins (fixed by the i.MX RT1062 hardware, they cannot be chosen):
//   pin 15 -> PPS IN   (ENET_1588_EVENT2_IN,  IOMUX ALT4)
//   pin 24 -> PPS OUT  (ENET_1588_EVENT1_OUT, IOMUX ALT6)
//   pin 13 -> LED, on when the clock is locked

#include <Arduino.h>
#include <t41-ptp.h>
#include <QNEthernet.h>
#include <util/atomic.h>

#include "net_config.h"
#include "profiles.h"
#include "webconfig.h"
#include "wordclock.h"

// Critical section: it saves and restores PRIMASK instead of calling a bare
// __enable_irq(), which would switch interrupts back on even if the caller
// had them off already.
//
// __get_primask/__set_primask come from the Teensy core's <util/atomic.h>.
// The core also carries cmsis_gcc.h with the upper-case versions, but imxrt.h
// never includes it. These are the ones QNEthernet already uses.
static inline uint32_t enterCritical()
{
  const uint32_t primask = __get_primask();
  __disable_irq();
  return primask;
}

static inline void exitCritical(uint32_t primask)
{
  __set_primask(primask);
}

// Reads a flag and clears it leaving no gap: done with an if followed by an
// assignment, an ISR could set it in between and we would wipe its request
// without ever having served it.
static inline bool takeFlag(volatile bool &flag)
{
  const uint32_t primask = enterCritical();
  const bool taken = flag;
  flag = false;
  exitCritical(primask);
  return taken;
}


byte mac[6];

IntervalTimer syncTimer;
IntervalTimer announceTimer;

bool master=true;
bool slave=false;

l3PTP ptp(master,slave,false);
//l2PTP ptp(master,slave,p2p);

// The sync rate, the advertised interval, the domain, the priorities and the
// lock window are no longer chosen here: they come from the profile, which is
// picked at run time from the web page and stored in EEPROM. See
// src/profiles.h; the default profile is the values that used to be
// hardcoded (8 sync per second, announce every second, domain 0,
// priorities 128).
//
// The advertised interval and the real rate both come out of logSyncInterval,
// so they can no longer come apart: a static_assert used to guard that, and
// now it is guaranteed by there not being two numbers.
static size_t activeProfile = 0;

// The word clock rate is picked the same way, from the same page, and stored
// in its own EEPROM slot. It is not part of the PTP profile: what the
// generator puts out has nothing to do with how the box presents itself to
// the network. See src/wordclock.h.
static size_t activeWordclockRate = 0;

// noPPSCount is counted in sync timer ticks, NOT in seconds. With sync at
// 8 Hz and PPS at 1 Hz, eight ticks fit between two pulses, so a fixed
// threshold of 5 would trip every second with a perfectly healthy PPS. That is
// why the threshold is expressed in seconds and converted, and the conversion
// depends on the profile: it is recomputed every time one is picked.
static const int kNoPPSSeconds = 5;
static int noPPSLimit = kNoPPSSeconds * 8;

// How many measurements in a row the servo has to string together before it
// counts as locked.
static const int kLockThreshold = 5;

// What we announce depending on whether we really are locked.
//
// The box is disciplined by a word clock: it gives frequency and one edge per
// second, but no traceable absolute time. That is why we do NOT use
// clockClass 6, which means locked to a primary reference such as GPS and
// would be a lie. 13 means synchronised to an application-specific source,
// which is the case here.
static const uint8_t kClockClassLocked = 13;
static const uint8_t kClockClassFree   = 248;   // default, not traceable
static const uint8_t kTimeSourceLocked = 0x90;  // OTHER
static const uint8_t kTimeSourceFree   = 0xa0;  // INTERNAL_OSCILLATOR

// clockAccuracy and offsetScaledLogVariance stay at "unknown". Putting a
// figure there would be claiming a precision we have never measured on real
// hardware.

// TAI-UTC offset, in seconds. 37 since 2017 and still current.
//
// We announce it as NOT valid, and that is deliberate, not neglect. This box
// has no source of absolute time: the word clock gives frequency and one edge
// per second, but nobody tells it what time it is, so the PTP seconds counter
// is arbitrary and the offset from UTC does not mean much. On top of that, a
// leap second would change the 37 and we would have no way of finding out.
// Sending the number with the validity bit false says "here, but do not rely
// on it", which is exactly right.
//
// For AES67 audio this does no harm: only relative synchronisation matters.
// If a genuinely traceable reference is ever added (a GPS), then
// kUtcOffsetValid does have to go true AND kUtcOffset has to be kept
// current.
static const int16_t kUtcOffset = 37;
static const bool kUtcOffsetValid = false;

// The most drift accepted before declaring the reference invalid, in
// nanoseconds per second. 100000 ns/s is 100 ppm, and the Teensy's crystal
// drifts on the order of 30 ppm: the margin is there because a large jump
// means what is arriving is not trustworthy, not that the clock has gone mad.
//
// This is the value the library carried hardcoded. It is put here because it
// is a statement about THIS hardware, and whoever builds the box with another
// oscillator has to be able to change it without touching the library.
static const double kMaxDriftNsps = 100000;

// Servo gains. They are the upstream ones, set explicitly because inheriting
// them without having chosen them is not the same as choosing them: if they
// are ever touched, they get touched here and it shows in the diff. See
// COMPARATIVA-SERVO.md.
static const double kServoKp = 1.0;
static const double kServoKi = 0.5;

// What locked means, in nanoseconds of offset. While the clock stays inside
// this window the lock count climbs, and once the count passes kLockThreshold
// we announce clockClass 13 instead of 248.
//
// Note: the lock window (what being locked means, in nanoseconds) is per
// profile, because it is what the network reads as "this box is here". See
// src/profiles.h.

// The servo mode boundaries, the upstream ones: above 1000 ns/s of drift it
// corrects frequency, and above 1000 ns of offset it steps instead of
// steering. They go with kServoKp and kServoKi, which is tuning.
static const double kFreqModeThresholdNsps = 1000;
static const NanoTime kCoarseModeThresholdNs = 1000;

// Delay of the 1588 compare channel, in nanoseconds. The output pulse is
// advanced by this much so that it lands on the exact second, and the input
// capture undoes it to get back to the real value.
//
// It comes from upstream and is a property of the silicon, not of our
// profile: it should live in the library, but the library does not touch
// these channels, the sketch configures them. At least it now has a name and
// sits in one place, instead of being the literal 60 repeated twice.
static const NanoTime kCompareChannelDelayNs = 60;

void syncInterrupt();
void announceInterrupt();
static void interrupt_1588_timer();

// Applies a profile: tells the library about it, redoes the sync timer's rate
// and recomputes the PPS loss threshold, which is counted in ticks of that
// same timer.
//
// It is called from setup() with whatever is stored and, later, from the web
// server when somebody picks another one. Being the same function in both
// cases is what makes a profile picked over the web leave the box exactly as
// if it had started up with it.
static void applyProfile(size_t index)
{
  activeProfile = index;
  const PtpProfile &p = profileAt(index);

  ptp.setDomainNumber(p.domainNumber);
  ptp.setLogSyncInterval(p.logSyncInterval);
  ptp.setLogAnnounceInterval(p.logAnnounceInterval);
  ptp.setPriority1(p.priority1);
  ptp.setPriority2(p.priority2);
  ptp.setLockThresholdNs(p.lockThresholdNs);

  const int ticks = profileSyncTicksPerSecond(p);
  const int limit = kNoPPSSeconds * ticks;

  // noPPSLimit is read by loop() and compared against a counter an ISR
  // touches. Writing it with interrupts off does not make it atomic by magic
  // -- it is an int and already is -- but it does stop a sync tick from
  // landing between the rate change and the threshold change and seeing the
  // pair mismatched.
  const uint32_t primask = enterCritical();
  noPPSLimit = limit;
  exitCritical(primask);

  // If the timer has not been started yet (there has been no link), this does
  // nothing: begin() starts it with the right period when the time comes.
  if(syncTimer){
    syncTimer.update(profileSyncIntervalUs(p));
  }

  Serial.printf("[Profile] %s: sync log %d, announce log %d, domain %u, "
                "priority1 %u, locked < %d ns\n",
                p.id,
                (int)p.logSyncInterval,
                (int)p.logAnnounceInterval,
                (unsigned)p.domainNumber,
                (unsigned)p.priority1,
                (int)p.lockThresholdNs);
}

// Applies a word clock rate: restarts the divider for it. As with
// applyProfile, it is the same function at start-up and from the web server,
// so a rate picked over the web leaves the box exactly as if it had started
// up with it.
//
// The frequency is NOT measured here. The measurement blocks for 200 ms and
// this runs out of loop(), where the one thing that must never stop is PTP.
// The check at start-up is where the warning comes from.
static void applyWordclockRate(size_t index)
{
  activeWordclockRate = index;
  const uint32_t rate = wordclockRateAt(index);

  if(wordclockDividerBegin(rate)){
    Serial.printf("[Clock] divider started for %lu Hz\n", (unsigned long)rate);
  }else{
    Serial.printf("[Clock] ERROR: %lu Hz cannot be divided with this hardware\n",
                  (unsigned long)rate);
  }
}

// Starts the network: DHCP if it is enabled, and a static configuration if
// nothing comes of it. It says over serial which path it took, because from
// outside there is no telling.
static void startNetworking()
{
  qindesign::network::Ethernet.setHostname(AES67_HOSTNAME);

#if AES67_USE_DHCP
  if(qindesign::network::Ethernet.begin()){
    // begin() only says the DHCP client started, not that we have an address.
    if(qindesign::network::Ethernet.waitForLocalIP(AES67_DHCP_TIMEOUT_MS)){
      Serial.println("[Network] address obtained over DHCP");
      return;
    }
    Serial.printf("[Network] DHCP gave no address in %lu ms\n",
                  (unsigned long)AES67_DHCP_TIMEOUT_MS);
  }else{
    Serial.println("[Network] could not start the DHCP client");
  }
  Serial.println("[Network] falling back to the static configuration");
  qindesign::network::Ethernet.end();
#endif

  const IPAddress staticIP{AES67_STATIC_IP};
  const IPAddress staticNetmask{AES67_STATIC_NETMASK};
  const IPAddress staticGateway{AES67_STATIC_GATEWAY};

  if(!qindesign::network::Ethernet.begin(staticIP, staticNetmask, staticGateway)){
    Serial.println("[Network] ERROR: could not start the network");
  }
}

void setup()
{
Serial.begin(2000000);

  // Without waiting for the port, the first lines are lost over USB, and
  // those are exactly the ones saying which network configuration we took.
  const uint32_t serialDeadline = millis() + AES67_SERIAL_WAIT_MS;
  while(!Serial && (int32_t)(millis() - serialDeadline) < 0){
  }

  pinMode(13, OUTPUT);

  ptp.setClockClass(kClockClassFree);
  ptp.setTimeSource(kTimeSourceFree);
  ptp.setCurrentUtcOffset(kUtcOffset);
  ptp.setUtcOffsetValid(kUtcOffsetValid);
  ptp.setMaxDriftNsps(kMaxDriftNsps);
  ptp.setFreqModeThresholdNsps(kFreqModeThresholdNsps);
  ptp.setCoarseModeThresholdNs(kCoarseModeThresholdNs);

  // The stored profile, or the default one if there is none.
  applyProfile(profileLoadSelection());

  // These two reset() the servo state, so they go before anything has been
  // started.
  ptp.setKp(kServoKp);
  ptp.setKi(kServoKi);

  // Setup networking
  startNetworking();
  qindesign::network::EthernetIEEE1588.begin();
  qindesign::network::Ethernet.macAddress(mac);
  
  webconfigBegin(applyProfile, applyWordclockRate);

  qindesign::network::Ethernet.onLinkState([](bool state) {
    Serial.printf("[Ethernet] Link %dMbps %s\n", qindesign::network::Ethernet.linkSpeed(), state ? "ON" : "OFF");
    if (state) {
      ptp.begin();
      syncTimer.begin(syncInterrupt, profileSyncIntervalUs(profileAt(activeProfile)));
      announceTimer.begin(announceInterrupt, 1000000);
    }
  });

  Serial.printf("Mac address:   %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print( "IP:            "); Serial.println(qindesign::network::Ethernet.localIP());
  Serial.print( "Netmask:       "); Serial.println(qindesign::network::Ethernet.subnetMask());
  Serial.print( "Gateway:       "); Serial.println(qindesign::network::Ethernet.gatewayIP());
  Serial.print( "Hostname:      "); Serial.println(AES67_HOSTNAME);
  Serial.println();

  // PPS Out
  // peripherial: ENET_1588_EVENT1_OUT
  // IOMUX: ALT6
  // teensy pin: 24
  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B0_12 = 6;
  qindesign::network::EthernetIEEE1588.setChannelCompareValue(1, NS_PER_S-kCompareChannelDelayNs);
  qindesign::network::EthernetIEEE1588.setChannelMode(1, qindesign::network::EthernetIEEE1588.TimerChannelModes::kPulseHighOnCompare);
  qindesign::network::EthernetIEEE1588.setChannelOutputPulseWidth(1, 25);
  

  // Word clock to PPS divider, at the stored rate or the default one if there
  // is none. The signal comes in on pin 14 and the pulse goes out on pin 19,
  // which has to be bridged to pin 15 with a wire. See wordclock.h.
  applyWordclockRate(wordclockRateLoadSelection());

  // We check that what actually comes in looks like what we configured. We do
  // NOT change anything automatically: if the box reconfigured itself
  // silently, one day PTP would change behaviour and nobody would know why.
  const uint32_t measured = wordclockMeasureHz();
  if(measured == 0){
    Serial.println("[Clock] WARNING: no word clock arriving on pin 14");
  }else{
    // A 2% tolerance: the measurement is made with millis(), nothing precise.
    const uint32_t nominal = wordclockRateAt(activeWordclockRate);
    const uint32_t margin = nominal / 50;
    if(measured + margin < nominal || measured > nominal + margin){
      Serial.printf("[Clock] WARNING: measured %lu Hz but configured %lu Hz\n",
                    (unsigned long)measured, (unsigned long)nominal);
      Serial.println("[Clock] the PPS will not run at 1 Hz and PTP will be offset");
    }else{
      Serial.printf("[Clock] word clock measured: %lu Hz\n",
                    (unsigned long)measured);
    }
  }

  // PPS-IN
  // peripherial: ENET_1588_EVENT2_IN
  // IOMUX: ALT4
  // teensy pin: 15 (the divider's pulse gets here over the bridge from pin 19)
  attachInterruptVector(IRQ_ENET_TIMER, interrupt_1588_timer); //Configure Interrupt Handler
  NVIC_ENABLE_IRQ(IRQ_ENET_TIMER); //Enable Interrupt Handling
  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_03 = 4; 
  qindesign::network::EthernetIEEE1588.setChannelMode(2, qindesign::network::EthernetIEEE1588.TimerChannelModes::kCaptureOnRising); //enable Channel2 rising edge trigger
  qindesign::network::EthernetIEEE1588.setChannelInterruptEnable(2, true); //Configure Interrupt generation
}

// All of this is written by the ISRs and read by loop(). volatile is needed
// so the compiler does not keep them in registers. It happened to work without
// it, but only because the calls into ptp.* are in another translation unit
// and force a reread: with LTO on, or with code moved around, that would stop
// being true.
//
// Careful: volatile does NOT give atomicity. The NanoTimes are int64_t and
// this is a 32-bit micro, so they are read in two pieces. That is why loop()
// copies them inside a critical section.
volatile bool sync=false;
volatile bool announce=false;
volatile bool pps=false;
volatile int noPPSCount=0;

volatile NanoTime interrupt_s=0;
volatile NanoTime interrupt_ns=0;
volatile NanoTime pps_s=0;
volatile NanoTime pps_ns=0;

// Really locked: the servo has converged AND we are still getting PPS.
static inline bool isLocked()
{
  return ptp.getLockCount() > kLockThreshold && noPPSCount < noPPSLimit;
}

void loop()
{
  if(takeFlag(announce)){
    // We announce the real state. If we lose the PPS we have to stop saying we
    // are locked: otherwise the other devices' BMCA cannot know, and a
    // receiver cannot tell a live clock from one that has lost its
    // reference.
    const bool locked = isLocked();
    ptp.setClockClass(locked ? kClockClassLocked : kClockClassFree);
    ptp.setTimeSource(locked ? kTimeSourceLocked : kTimeSourceFree);
    ptp.announceMessage();
  }
  if(takeFlag(sync)){
    ptp.syncMessage();
  }

  // The four timestamps have to be read as one block. If the 1588 ISR fires
  // in the middle of the read, we would mix pulse N with pulse N+1 and inject
  // up to a second of error into the clock servo.
  NanoTime l_pps_s, l_pps_ns, l_interrupt_s, l_interrupt_ns;
  const uint32_t primask = enterCritical();
  const bool gotPPS = pps;
  if(gotPPS){
    pps=false;
    l_pps_s = pps_s;
    l_pps_ns = pps_ns;
    l_interrupt_s = interrupt_s;
    l_interrupt_ns = interrupt_ns;
  }
  exitCritical(primask);

  if(gotPPS){
    ptp.ppsInterruptTriggered((l_pps_s*NS_PER_S)+l_pps_ns, (l_interrupt_s*NS_PER_S)+l_interrupt_ns);
    if(ptp.getLockCount() > kLockThreshold){
      sync=true;  // writing a bool: one instruction, no protection needed
    }
  }
  webconfigUpdate();
  ptp.update();
  digitalWrite(13, isLocked() ? HIGH : LOW);
}

void syncInterrupt() {
  // noPPSCount++ is a read, a modify and a write. The 1588 ISR sets it to
  // zero, and if it preempts us in between we would wipe that zeroing by
  // writing back the old value plus one.
  const uint32_t primask = enterCritical();
  if(noPPSCount > noPPSLimit){
    sync=true;
  }else{
    noPPSCount++;
  }
  exitCritical(primask);
}

void announceInterrupt() {
  announce=true;
}

static void interrupt_1588_timer() {
  uint32_t t;
  if (!qindesign::network::EthernetIEEE1588.getAndClearChannelStatus(2)) {
    asm("dsb"); // allow write to complete so the interrupt doesn't fire twice
    return;
  }
  qindesign::network::EthernetIEEE1588.getChannelCompareValue(2,t);

  t = ((NanoTime)t+NS_PER_S-kCompareChannelDelayNs)%NS_PER_S;
  
  timespec ts;
  qindesign::network::EthernetIEEE1588.readTimer(ts);

  if(ts.tv_nsec < 100*1000*1000 && t > 900*1000*1000){
    pps_s=ts.tv_sec;
    interrupt_s=ts.tv_sec-1;    
  }else{
    interrupt_s=ts.tv_sec;      
    if(ts.tv_nsec < 500*1000*1000){
      pps_s=ts.tv_sec;
    }else{
      pps_s=ts.tv_sec+1;
    }
  }

  interrupt_ns=t;
  pps_ns=0;

  pps=true;
  noPPSCount=0;
  
  asm("dsb"); // allow write to complete so the interrupt doesn't fire twice
}
