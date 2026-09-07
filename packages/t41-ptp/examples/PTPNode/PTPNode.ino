#include <t41-ptp.h>
#include "Profiles/PtpProfiles.h"
#include <QNEthernet.h>

byte mac[6];
// One address per board, so this is the line to change on the second
// one. The two sketches this replaced carried .211 and .210.
IPAddress staticIP{192, 168, 0, 211};
IPAddress subnetMask{255, 255, 255, 0};
IPAddress gateway{192, 168, 0, 6};

IntervalTimer syncTimer;
IntervalTimer announceTimer;

// Both roles at once. The port announces itself and, if a better clock
// turns up on the segment, follows that one instead of going quiet --
// which is what a node that may or may not be the best clock present
// should do. Configured master only, it would stand aside and say
// nothing; configured slave only, it could never take over.
bool p2p=false;
bool master=true;
bool slave=true;

l3PTP ptp(master,slave,p2p);
//l2PTP ptp(master,slave,p2p);

// Eight Sync a second, which is what an AES67 endpoint expects, and the
// interval the Announce has to agree on -- setLogSyncInterval(-3) below
// is the same number written the way the wire wants it, and a slave that
// reads one and is fed the other times its exchanges against a rate that
// is not happening.
constexpr unsigned long SYNC_INTERVAL_US = 125000;

// Ticks of the timer above with no reference edge before the node calls
// itself free-running: six seconds, as it was when the timer ran at one
// second and this was 6. The count is in ticks and the timer moved, so
// the number had to move with it.
constexpr int NO_REFERENCE_TICKS = 48;

void setup()
{
Serial.begin(2000000);
  pinMode(13, OUTPUT);

  // The AES67 media profile, by name, from the profiles package this
  // repository keeps as data: eight Sync a second (matching
  // SYNC_INTERVAL_US), one Announce, domain 0. The library holds no table of
  // its own; this is the five numbers it is handed, and the same five the
  // macOS driver reads from the same file.
  const AES67::PtpProfile* profile = AES67::ptpProfileByName("aes67");
  ptp.applyProfile({profile->settings.domainNumber,
                    profile->settings.majorSdoId,
                    profile->settings.logSyncInterval,
                    profile->settings.logAnnounceInterval,
                    profile->settings.logMinDelayReqInterval});

  // Setup networking
  qindesign::network::Ethernet.setHostname("t41ptpnode");
  qindesign::network::Ethernet.macAddress(mac);
  qindesign::network::Ethernet.begin(staticIP, subnetMask, gateway);
  qindesign::network::EthernetIEEE1588.begin();
  
  qindesign::network::Ethernet.onLinkState([](bool state) {
    Serial.printf("[Ethernet] Link %dMbps %s\n", qindesign::network::Ethernet.linkSpeed(), state ? "ON" : "OFF");
    if (state) {
      ptp.begin();
      syncTimer.begin(syncInterrupt, SYNC_INTERVAL_US);
      announceTimer.begin(announceInterrupt, 1000000);
    } else {
      // Nothing can go out with the link down, and the timers would keep
      // asking. The clock keeps running: begin() only zeroes it the first
      // time, so a link that bounces no longer throws the grandmaster
      // back to the epoch.
      syncTimer.end();
      announceTimer.end();
      ptp.end();
    }
  });

  Serial.printf("Mac address:   %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print( "IP:            "); Serial.println(qindesign::network::Ethernet.localIP());
  Serial.println();

  // PPS Out
  // peripherial: ENET_1588_EVENT1_OUT
  // IOMUX: ALT6
  // teensy pin: 24
  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B0_12 = 6;
  qindesign::network::EthernetIEEE1588.setChannelCompareValue(1, NS_PER_S-60);
  qindesign::network::EthernetIEEE1588.setChannelMode(1, qindesign::network::EthernetIEEE1588.TimerChannelModes::kPulseHighOnCompare);
  qindesign::network::EthernetIEEE1588.setChannelOutputPulseWidth(1, 25);
  

  // Reference in: a word clock at the audio sampling rate, divided down
  // to one reference edge a second by interrupt_1588_timer().
  // peripherial: ENET_1588_EVENT2_IN
  // IOMUX: ALT4
  // teensy pin: 15
  attachInterruptVector(IRQ_ENET_TIMER, interrupt_1588_timer); //Configure Interrupt Handler
  NVIC_ENABLE_IRQ(IRQ_ENET_TIMER); //Enable Interrupt Handling
  IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_03 = 4; 
  qindesign::network::EthernetIEEE1588.setChannelMode(2, qindesign::network::EthernetIEEE1588.TimerChannelModes::kCaptureOnRising); //enable Channel2 rising edge trigger
  qindesign::network::EthernetIEEE1588.setChannelInterruptEnable(2, true); //Configure Interrupt generation
}

// Shared with the interrupt, hence volatile: without it the compiler
// may keep these in a register inside loop() and never see what
// interrupt_1588_timer() writes.
//
// Each is set by an interrupt and taken by loop(), never the other way
// round, and taking one clears it in the same breath -- see takeFlag().
// Reading and then clearing as two steps loses whatever the interrupt
// raised in between.
volatile bool syncTick=false;
volatile bool announce=false;
volatile bool pps=false;

// Sync timer ticks with no PPS to pace them. Written and read by loop()
// alone: it used to be incremented in one interrupt and zeroed in
// another, which is a read-modify-write across two priorities, so an
// increment could vanish under the write that cleared it.
int noPPSCount=0;

// Whether loop() owes a Sync. Only loop() touches it.
bool sendSync=false;

// When the last reference edge arrived, and whether one ever has. The
// timestamp alone cannot answer the second question: zero is a value
// millis() genuinely passes through, and a node that has never seen pin
// 15 would read it as an edge at boot. Unsigned subtraction is what makes
// the comparison below survive the wrap at 49.7 days.
unsigned long lastPPSMillis=0;
bool referenceSeen=false;

// When the state was last printed.
unsigned long lastReportMillis=0;

// Names for the port state, for the line below.
static const char *portStateName(PortState state)
{
  switch (state) {
    case PortState::Initializing: return "init";
    case PortState::Listening:    return "listening";
    case PortState::Master:       return "master";
    case PortState::Passive:      return "passive";
    case PortState::Slave:        return "slave";
  }
  return "?";
}


// Reads a flag and clears it as one step, so an interrupt landing in the
// middle cannot have its event dropped.
static bool takeFlag(volatile bool &flag)
{
    noInterrupts();
    const bool taken = flag;
    flag = false;
    interrupts();
    return taken;
}

// These too, but volatile alone is not enough: they are int64_t, and
// reading 64 bits is NOT ATOMIC on a 32-bit Cortex-M7. An interrupt
// landing between the two halves leaves loop() with a torn value --
// half of one second and half of the next -- which is a full second of
// error on a device built for sub-microsecond accuracy.
//
// They are read as a snapshot with interrupts off, in takePPS().
volatile NanoTime interrupt_s=0;
volatile NanoTime interrupt_ns=0;
volatile NanoTime pps_s=0;
volatile NanoTime pps_ns=0;

// The flag and a coherent copy of the four timestamps, taken together.
// Taking the flag first and the timestamps after leaves a window where a
// pulse arriving in between is counted twice: once with the values it
// wrote, and again on the next pass with the flag it left set.
//
// The window with interrupts disabled is a bool and four 64-bit reads.
static bool takePPS(NanoTime &ppsTs, NanoTime &localTs)
{
    noInterrupts();
    const bool taken = pps;
    pps = false;
    const NanoTime ps = pps_s, pn = pps_ns, is = interrupt_s, in = interrupt_ns;
    interrupts();

    if (!taken)
    {
        return false;
    }

    ppsTs = (ps*NS_PER_S)+pn;
    localTs = (is*NS_PER_S)+in;
    return true;
}

// How long a clock that has lost its reference is still worth more than a
// bare crystal. adjustFreq() is a standing correction, so the oscillator
// keeps whatever frequency it was last told to run at and drifts from
// there with temperature, which is a slow way to be wrong compared with
// never having been disciplined at all. A minute of that is a few tens of
// microseconds; past it the claim stops being worth making.
constexpr unsigned long HOLDOVER_MS = 60000;

// Table 5, the three the sketch can honestly be in: 13 is a clock
// synchronized to an application-specific source of time -- the word
// clock on pin 15 -- 14 is that same clock in holdover, and 248 is a
// free-running crystal with no history worth mentioning. Announcing one
// of them from setup() would have the node claim the reference it had at
// boot for as long as it ran, so the callers below move it with the
// input instead: 13 on a reference edge, 14 once six ticks have gone by
// without one -- the same moment the sketch starts pacing its own Sync --
// and 248 when the holdover above has run out.
//
// The comparison is what makes this cheap enough to sit on a path that
// runs every second: setClockClass() re-runs the port state decision, and
// only a change is worth that. 248 to start with, because that is the
// library's own default and nothing has arrived on pin 15 yet.
static void announceClockClass(uint8_t value)
{
  static uint8_t announced = 248;
  if (value != announced) {
    announced = value;
    ptp.setClockClass(value);
  }
}

void loop()
{
  if(takeFlag(announce)){
    ptp.announceMessage();
  }

  // The counting the sync timer used to do inside its own interrupt.
  //
  // One tick, one Sync. The reference edge used to pace the Sync, which
  // held the rate to whatever the input ran at -- one a second here --
  // however fast this timer ticked. It now only disciplines the clock,
  // and the two conditions that decide whether this node has any business
  // announcing time are the ones that were already here: either it is
  // free-running, or the servo is locked to the reference it has.
  if(takeFlag(syncTick)){
    if(noPPSCount > NO_REFERENCE_TICKS){
      sendSync=true;   // free running: nothing else is pacing us
      const bool holdover =
        referenceSeen && (millis() - lastPPSMillis) < HOLDOVER_MS;
      announceClockClass(holdover ? 14 : 248);
    }else{
      noPPSCount++;
      if(ptp.getLockCount() > 5){
        sendSync=true;
      }
    }
  }

  if(sendSync){
    sendSync=false;
    ptp.syncMessage();
  }

  NanoTime ppsTs, localTs;
  if(takePPS(ppsTs, localTs)){
    noPPSCount=0;
    lastPPSMillis=millis();
    referenceSeen=true;
    announceClockClass(13);

    ptp.ppsInterruptTriggered(ppsTs, localTs);
  }
  ptp.update();

  // The LED says the port is doing its job, whichever job the BMCA has
  // given it. As master: on air, either disciplined by the reference or,
  // with no reference at all, free-running on its own crystal, which is
  // what a master with nothing to follow is -- the lock count means
  // nothing there. As slave: locked to a master still being heard.
  const bool freeRunning = noPPSCount >= NO_REFERENCE_TICKS;
  const PortState state = ptp.getPortState();
  const bool working =
      (state == PortState::Master && (freeRunning || ptp.getLockCount() > 5)) ||
      (state == PortState::Slave && ptp.getLockCount() > 5 && ptp.isSyncReceiptValid());
  digitalWrite(13, working ? HIGH : LOW);

  if (millis() - lastReportMillis >= 1000) {
    lastReportMillis = millis();
    Serial.printf("[ptp] state=%s pps=%s sync=%s master=%s offset=%ldns delay=%ldns lock=%d tx_fail=%lu bind_fail=%lu\n",
                  portStateName(state),
                  freeRunning ? "none" : "yes",
                  ptp.isSyncReceiptValid() ? "ok" : "lost",
                  ptp.hasSelectedMaster() ? "chosen" : "none",
                  (long)ptp.getOffset(), (long)ptp.getDelay(), ptp.getLockCount(),
                  (unsigned long)ptp.getTxFailureCount(),
                  (unsigned long)ptp.getBindFailureCount());
  }
}

void syncInterrupt() {
  syncTick=true;
}

void announceInterrupt() {
  announce=true;
}

// The reference on the capture input is a word clock: a square wave at
// the sampling rate of the digital audio it clocks, not one pulse a
// second. One rising edge in WORD_CLOCK_DIVISOR is the reference edge
// and the other 47999 exist only to be counted, which is all the
// interrupt may do with them: at 48 kHz the edges are about 12500 CPU
// cycles apart, and the body below -- a timer read, the second-boundary
// arithmetic and four 64-bit volatile stores -- has no business running
// 48000 times a second.
static const uint32_t WORD_CLOCK_DIVISOR = 48000;

// Nominal spacing between two consecutive edges. Truncated: the exact
// figure is 20833.33... ns. That is deliberate, because the value is
// only ever used to tell one elapsed period from two, and the half
// period of margin that decision has swallows a third of a nanosecond
// for as many edges as this counter will ever see.
static const uint32_t WORD_CLOCK_PERIOD_NS = (uint32_t)(NS_PER_S / WORD_CLOCK_DIVISOR);

// The 1588 nanosecond counter, and so every value captured from it,
// wraps here.
static const uint32_t CAPTURE_WRAP_NS = (uint32_t)NS_PER_S;

// Touched by interrupt_1588_timer() alone, so unlike the timestamps
// above they need neither volatile nor a critical section: loop() never
// looks at them.
static uint32_t wordClockCount = 0;
static uint32_t wordClockLastCapture = 0;
static bool wordClockLastCaptureValid = false;

static void interrupt_1588_timer() {
  uint32_t t;
  if (!qindesign::network::EthernetIEEE1588.getAndClearChannelStatus(2)) {
    asm("dsb"); // allow write to complete so the interrupt doesn't fire twice
    return;
  }
  // Without the capture there is no pulse to report: t would go into the
  // arithmetic below uninitialised.
  if (!qindesign::network::EthernetIEEE1588.getChannelCompareValue(2,t)) {
    asm("dsb");
    return;
  }

  // How many edges went by, rather than assuming this interrupt is one
  // of them. The capture register is single buffered, so an edge landing
  // before the previous one has been read is lost, and a lost edge moves
  // the divider's output by a whole sample period -- 20833 ns, twenty
  // times the error budget of a clock built for sub-microsecond work --
  // silently, and for the rest of the run. The capture says how many
  // there were: the hardware latches the timer at the edge itself, so
  // the spacing stays exact however late the interrupt gets to run.
  //
  // What this cannot recover is a gap of a second or more, which the
  // wrap at CAPTURE_WRAP_NS aliases to a shorter one and which does move
  // the phase. That is a reference that went away rather than an
  // interrupt that was missed, and loop() already answers a silent input
  // by letting the clock free-run.
  if (!wordClockLastCaptureValid) {
    wordClockLastCapture = t;
    wordClockLastCaptureValid = true;
    asm("dsb");
    return;
  }
  const uint32_t elapsed = (t + CAPTURE_WRAP_NS - wordClockLastCapture) % CAPTURE_WRAP_NS;
  wordClockLastCapture = t;

  const uint32_t edges = (elapsed + WORD_CLOCK_PERIOD_NS/2) / WORD_CLOCK_PERIOD_NS;
  if (edges == 0) {
    // The same capture read twice: there is no new edge to count.
    asm("dsb");
    return;
  }

  wordClockCount += edges;
  if (wordClockCount < WORD_CLOCK_DIVISOR) {
    asm("dsb");
    return;
  }
  // Subtracted, not zeroed. Edges recovered above can carry the count
  // past the divisor, and zeroing would drop that remainder -- shifting
  // the phase by exactly the amount the counting was there to hold.
  wordClockCount -= WORD_CLOCK_DIVISOR;

  t = ((NanoTime)t+NS_PER_S-60)%NS_PER_S;
  
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
  // The reference edge is declared to be the top of a second. A GPS
  // pulse is one; a divided word clock is not, because nothing in a word
  // clock says where a second begins -- it carries a rate, not an epoch.
  // The clock therefore locks to the right frequency with a fixed and
  // arbitrary offset in the seconds.
  pps_ns=0;

  pps=true;
  
  asm("dsb"); // allow write to complete so the interrupt doesn't fire twice
}
