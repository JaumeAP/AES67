# Hardware decision

## Board chosen: Teensy 4.1

This project's hardware platform is the **Teensy 4.1** (PJRC), based on the
NXP i.MX RT1062 microcontroller (ARM Cortex-M7 at 600 MHz).

Decision taken on 2026-08-24. It replaces the earlier approach based on a
Raspberry Pi 5 with Linux.

## Why this board

Requirements that had to be met at the same time:

1. Hardware PTP (IEEE 1588) timestamping, equivalent to an Intel I210's.
2. No Linux and no full operating system.
3. With a real TCP/IP stack, for full network access.
4. A common, cheap, easy to find board.

The Teensy 4.1 meets all of them:

- The i.MX RT1062 carries an Ethernet MAC with IEEE 1588 v2 support built into
  the silicon, with 4 1588 timer channels allowing input capture and output
  compare, including PPS signal generation.
- It runs bare metal or with FreeRTOS, no Linux.
- The lwIP stack (through QNEthernet) gives it full TCP/IP.
- It is a very widespread maker board, inexpensive and available from many
  distributors.

## Bill of materials

Everything the box needs, with the reference designators used in the rest of
this file. Nothing here has been bought or built: the values come from the
sections that specify each part, and those say what is still to be confirmed
against a catalogue.

**Boards**

| Ref | Item | Qty | Notes |
|---|---|---|---|
| - | Teensy 4.1 | 1 | Main board, ~30 USD. The Ethernet controller and PHY are already in the chip. |
| - | Ethernet Kit for Teensy 4.1 | 1 | Adds the physical RJ-45 connector (MagJack, ribbon cable, 2 pins, capacitor). Soldering required. |
| - | SOT-23-6 breakout | 1 | U1 is SOT-23-6, not 5: the DBV package carries a shutdown pin. The rest of this is hand built, so without an adapter there is nowhere to put it. |

The Teensy 4.1 does NOT come with the RJ-45 connector, despite having the
controller and the PHY built in: hence the kit.

Distributors: PJRC (official shop), SparkFun, Amazon, eBay. `SHOPPING.md` has
the links, what was found priced and what was not, and what the whole box comes
to.

**Word clock conditioning**, section "Signal conditioning: the circuit"

| Ref | Item | Qty | Notes |
|---|---|---|---|
| U1 | TLV3501AIDBVR | 1 | Comparator, 4.5 ns, rail-to-rail inputs, push-pull output. Rail-to-rail is not optional: the input node reaches 3.15 V. |
| R1 | 75 ohm 1%, 0.1 W | 1 | Termination, mounted at the BNC. |
| R2, R3, R4, R5 | 10 kohm 1% | 4 | Bias to 1.65 V, and the same again for the reference input. |
| Rf | 4.7 kohm | 1 | Positive feedback: this is the 51 mV of hysteresis. |
| R6 | 33 ohm | 1 | Series damping on the run to pin 14. |
| C1 | 100 nF X7R 50 V | 1 | AC coupling. |
| C2 | 1 uF X7R | 1 | Holds the reference node still. |
| C3, C4 | 100 nF and 1 uF | 2 | Supply decoupling at U1, within millimetres of the part. |
| D1 | BAT54S | 1 | Clamps the input node to the rails. |

**Wiring inside**, sections "The bridge, specified" and "PPS output"

| Ref | Item | Qty | Notes |
|---|---|---|---|
| R7 | 220 ohm | 1 | The bridge from pin 19 to pin 15, at the pin 19 end. |
| R8 | 75 ohm | 1 | Source termination at pin 24, feeding the PPS test point. |
| TP1, TP2 | Header pin or pad | 2 | PPS test point and the ground pin beside it. |
| - | Solid core wire, AWG 24 to 30 | - | The bridge and the short runs. |
| - | RJ-45 patch lead, short | 1 | From the etherCON's inner side to PJRC's kit. |

**Front panel**, sections "The lock LED" and "The console USB"

| Ref | Item | Qty | Notes |
|---|---|---|---|
| LED1 | Green LED, 3 or 5 mm | 1 | Lock indicator. |
| - | Panel LED holder | 1 | To match the LED. |
| R9 | 330 ohm | 1 | 3.9 mA through a green one. 220 ohm if it looks dim. |
| J4 | USB type B panel socket | 1 | Console. Type A facing out would say host, which this is not. |
| - | USB B to micro-B lead, short | 1 | Inside, with its strain relief anchored to the box. |

**Rear panel and power**, sections "Panel" and "Power"

| Ref | Item | Qty | Notes |
|---|---|---|---|
| J1 | 75 ohm panel BNC | 1 | Word clock input. Part number from the catalogue. |
| J2 | Neutrik etherCON `NE8FDP` | 1 | Or a plain RJ-45 feedthrough, which is smaller for a printed box. |
| J3 | Barrel jack, 2.1 x 5.5 mm | 1 | Panel mount, centre positive, marked on the panel. |
| D2 | SS14 or similar Schottky | 1 | In series behind J3, against a supply plugged in backwards. |

**Outside the box**

| Item | Notes |
|---|---|
| 5 V adapter, 1 A or more | Certified. Mains never enters the enclosure. |
| RG59 75 ohm coax with BNCs | From the Nanosyncs. |
| USB lead for the console | Whatever the laptop has, to type B. |

**Enclosure and assembly**

| Item | Notes |
|---|---|
| Printed enclosure | See "Enclosure": https://www.thingiverse.com/thing:4717500 |
| Heat-set inserts and screws | For a printed box. |
| Standoffs for the Teensy | Also where the assembly trap gets solved: the MagJack collides with the Teensy's own pins. |

One thing this list cannot carry: the **cut between the VUSB and VIN pads**, on
the underside of the Teensy, without which the adapter and a connected laptop
fight each other. It is a knife, not a part.

### Enclosure

Decided: a small printed enclosure, and it is modelled in this repository at
`enclosure/aes67-masterbox.scad`. Two parts, base and lid, M3 into heat-set
inserts, with the cutouts and the internal layout this file specifies.

    openscad -o base.stl -D 'part="base"' enclosure/aes67-masterbox.scad
    openscad -o lid.stl  -D 'part="lid"'  enclosure/aes67-masterbox.scad

Everything in it is a parameter, and the ones marked `CONFIRM` in the file have
NOT been measured on real parts: the height of the Teensy with the MagJack
mounted offset, the panel cutouts of each connector, the Teensy's own mounting
holes. They are defaults taken from catalogues and they are where this model
will be wrong. Measure before printing.

As it stands the box comes out 78.5 x 87.8 x 29.0 mm outside, and both parts
export as watertight, manifold meshes.

Prior art, and the fallback if modelling it here turns out not to be worth it:

- https://www.thingiverse.com/thing:4717500, for a Teensy with the kit and
  nothing else: no panel cutouts, no room for a second board.
- ProtoSupplies acrylic, if a bought one is ever preferred:
  https://protosupplies.com/product/project-system-for-teensy-4-1-acrylic-enclosure/

Printing it is what decides the power supply: mains stays outside the box. See
"Power" further down.

**Printing.** PETG or PLA, 0.2 mm layers; the walls are 2.4 mm, which is six
perimeters at a 0.4 mm nozzle. Print the base floor down. The one thing that
needs thinking about is the Ethernet cutout: 24 mm of hole in a vertical wall
bridges badly, so either give it support or accept the first layers over it
coming out rough and clean them up. Everything else is 10 mm or smaller and
bridges without help.

**This is NOT the final enclosure, and that is deliberate.** Nothing has been
tested on hardware yet. Until PTP works on a real network there is no telling
whether the design is good, nor how many connectors the panel will end up
wanting. Drilling an aluminium panel is irreversible; a desktop enclosure
commits to nothing.

Once validated, the right path is rack. Specified in full further down.

### Assembly trap

The Ethernet connector is the same width as the Teensy and the pins collide.
The MagJack has to be mounted offset, in front of or behind the USB, or raised
on long pins. It drives the interior height needed, which in a 1U will be a
problem the day this moves to rack.

### Panel connectors

Three, and they are decided. Part numbers, grounding and the reasoning are in
the enclosure specification further down; this is the short list.

- **Word clock input**, 75 ohm BNC. Mandatory: it is the reference, and it goes
  to pin 14 through the conditioning circuit. Pin 15 is the 1588 capture, which
  never leaves the board.
- **Ethernet**, Neutrik etherCON. Mandatory.
- **Power**, a 5 V barrel jack. Mains never enters the box: see "Power" below,
  the enclosure is printed.

Plus the console USB and the lock LED on the front, and, inside, a test point
for the pin 24 PPS: that one is not a panel connector on purpose, see "PPS
output" below.

## Starting software

- **t41-ptp** (IMS-AS-LUH): high precision PTP synchronisation made
  specifically for the Teensy 4.1. Its master mode is disciplined by an
  external PPS signal, exactly the scheme this project uses. It is the main
  starting point, there is no need to start from scratch.
  https://github.com/IMS-AS-LUH/t41-ptp
- **QNEthernet**: lwIP-based Ethernet library for the Teensy 4/4.1. It is the
  TCP/IP stack giving real network access. We use OUR fork,
  `JaumeAP/QNEthernet` branch `multicast-ttl` (see below):
  https://github.com/JaumeAP/QNEthernet/tree/multicast-ttl

  The chain of forks, because it matters: t41-ptp does NOT work with
  ssilverman's official QNEthernet, it needs the IEEE 1588 patches HedgeHawk
  added to it. And HedgeHawk's fork, in turn, does not compile as it stands.
  - original upstream: https://github.com/ssilverman/QNEthernet
  - fork with the PTP patches: https://github.com/HedgeHawk/QNEthernet
- **AN12149** (NXP): official application note on implementing IEEE 1588 v2 on
  the i.MX RT with PTPd, FreeRTOS and lwIP. Low-level reference for the
  hardware timestamping engine.
  https://www.nxp.com/docs/en/nxp/application-notes/AN12149.pdf

NXP's SDK also includes the `enet_txrx_ptp1588_transfer` example.

## Libraries inside the repository

Under `lib/`, which is the library directory PlatformIO looks in by default,
there hangs one entry, and it is not a submodule any more:

- `lib/t41-ptp` → a symbolic link to `packages/t41-ptp`, the sibling package in
  this monorepo.

It used to be a submodule pointing at `JaumeAP/t41-ptp`. The monorepo move left
a stale copy of the pre-fork library in its place instead — no `ptp-servo.*`,
no `test/`, no `libraries/` — which is to say neither the firmware nor the host
tests could build. The link is what makes `lib/t41-ptp` mean the package that
is actually maintained here.

QNEthernet does not hang off this repository. t41-ptp is what includes it, so
it arrives with the link:

- `lib/t41-ptp/libraries/QNEthernet`, which is `packages/t41-ptp/libraries/QNEthernet`
  → the fork `JaumeAP/QNEthernet`, branch `multicast-ttl`.

Two practical consequences:

- Nothing has to be initialised: the link resolves inside the monorepo. There
  is one submodule left in the whole tree and it is doctest, at the root.
- PlatformIO's `lib_dir` does not descend into the libraries it finds, which is
  why `platformio.ini` carries `lib_extra_dirs = lib/t41-ptp/libraries`.
  Without that line QNEthernet is not seen and it does not compile. The
  QNEthernet it must find is that one and not `packages/QNEthernet`: the fork's
  additions, `setMulticastTTL()` and `setOutgoingDiffServ()`, are what
  `src/audio/tone-sender.cpp` calls, and the two copies are kept identical for
  that reason.

### Why t41-ptp is a fork and not upstream

It was code vendored into the repository until that stopped being necessary.
The reason for vendoring it was that we had changes of our own with nowhere to
put them; the fork now exists and carries them, and there are more of them than
there used to be.

What the fork carries over upstream is the sum of two things:

1. **Nine fixes** out of two audit passes over the original code. The most
   important, for us, is that two loops on the send path waited for the
   hardware timestamp **with no time limit at all**: if it never arrived, the
   firmware span inside `loop()` forever. The vendored copy we had carried
   them.

   Also: the length of a received message was not checked before its fields
   were read, receive buffers were sized by what the network said, and the
   result of sending was discarded.

2. **Our two long-standing changes**: the configurable ANNOUNCE dataset and
   `currentUtcOffsetValid`. Without the first, a grandmaster disciplined by an
   external reference announces itself as free-running and misleads every other
   device's BMCA.

**All eleven have been offered upstream**, one pull request each, to
`IMS-AS-LUH/t41-ptp`. As they land, the fork converges with upstream and stops
being needed.

t41-ptp's licence is MIT and the authors ask to be cited in scientific work;
see `lib/t41-ptp/LICENSE` and `CITATION.cff`. The root `NOTICE` sets out which
licence covers which subtree of the monorepo.

t41-ptp ships its example in `lib/t41-ptp/examples/PTPNode`.

## Alternatives ruled out

- **Intel I210-T1**: excellent hardware PTP support and very common, but it is
  only a PCIe network controller. It has no CPU of its own and no network
  stack: it requires a Linux host (a Raspberry Pi 5 with a PCIe adapter HAT,
  for instance). Ruled out by the requirement not to use Linux.
- **Raspberry Pi 5** (built-in ethernet): it has its own PHC and hardware
  timestamping on `eth0`, verifiable with `ethtool -T`, but it requires Linux.
- **BeagleY-AI**: built-in ethernet with a hardware clock, all on one board
  with no adapters, but it also requires Linux.
- **TI DP83640, Microchip KSZ8463, Microchip LAN9353/4/5**: PHY or switch chips
  with a hardware PTP engine, drivable from a small MCU with no operating
  system. Technically valid, but they force designing custom hardware instead
  of using a common board.
- **Standalone commercial boxes** (Sonifex AVN-GMCS, ESE ES-185F/PTP, Microchip
  TimeProvider 5000, Valiant): they work on their own without touching firmware
  or Linux, but they are expensive professional equipment, not a common maker
  board.

## The patch in the QNEthernet fork

`JaumeAP/QNEthernet` is a fork of `HedgeHawk/QNEthernet` that exists for a
single commit, `3a66886`: adding `#include <math.h>` to `src/lwip_t41.c`.

Without it the project does NOT compile. HedgeHawk's commit `5084666`
introduced a call to `round()` inside `enet_ieee1588_adjust_freq()` and left
out the include. With no declaration in scope, C assumes `round()` returns
`int` when it actually returns `double`, so the second argument of
`enet_ieee1588_adjust_timer()` is read from the wrong register. And that
happens on PTP's fine frequency adjustment path, that is at the heart of the
clock discipline.

GCC 14, which is what the Teensy toolchain on PlatformIO uses, rejects it
outright: `-Wimplicit-function-declaration` is an error there by default. With
older compilers it built with a mere warning and the call worked because GCC
recognises `round()` as a builtin, that is correct by accident, not by
declaration.

The patch is worth sending to HedgeHawk as a PR: it affects them just the same.

## PTP profile the box announces

Sync runs at 8 per second (`logMessageInterval` -3) and announce at 1 per
second (log 0). Both are inside the range AES67 allows, which runs from log -4
to log +1; -3 is also the sector's usual value. The library came at 1 sync per
second, which is legal but slow.

The real rate and the announced value both come out of the profile's
`logSyncInterval` (`src/profiles.h`), because if they come apart the
announcement lies. It was a `static_assert` in `src/main.cpp` while both were
compile-time constants; now there is only one number to derive them from.

Watch out for an interaction that is not obvious: the PPS loss counter is
measured in sync timer ticks, not in seconds. With sync at 8 Hz and PPS at
1 Hz, eight ticks fit between two pulses, so a fixed threshold would have
tripped every second with a perfectly healthy PPS. That is why the threshold is
expressed in seconds and converted to ticks.

On what it announces as clock quality: when locked it says `clockClass` 13 and
`timeSource` OTHER; when it loses the PPS it goes back to 248 and internal
oscillator. We do NOT say `clockClass` 6, which is what usually gets put there,
because it means locked to a primary reference such as GPS: the word clock
gives frequency and one edge per second, but no traceable absolute time, and
claiming it would be lying to the network. 13 means synchronised to an
application-specific source, which is exactly the case.

`clockAccuracy` and `offsetScaledLogVariance` stay at unknown on purpose.
Putting a figure there would be claiming a precision that has never been
measured on real hardware.

## Production enclosure, specification

For when bring-up is validated. None of this has been bought or tested.

Read it in two halves. "Chassis" is the rack box for later, and nothing below
depends on it. Everything else -- the panel connectors, the power supply, the
grounding and the panel itself -- is the box being built now, the printed one,
and says where it would differ in metal.

### Chassis

Hammond, RM series, half-width 1U, extruded aluminium.
https://www.hammfg.com/electronics/small-case/rack-mount/rm

The exact part number depends on the depth and has to be taken from the
catalogue: the half-width one could NOT be confirmed and I am not making it up.
The series pattern can be seen in `RM1U1918SBK`, which is 1U, full 19 inches
wide, 18 deep, black.

Full-1U alternative if more room to drill is wanted: Redco CH1.
https://www.redco.com/Redco-CH1-1U-Rackmount-Chassis-Box.html

### Ethernet on the panel

Neutrik etherCON D series, which is the de facto standard in professional
audio: mountable front or rear, with a latch.

- `NE8FDP`, CAT5e. https://www.neutrik.com/en/product/ne8fdp
- `NE8FDX-P6`, CAT6A, if margin is wanted.

**A detail that saves work**: the `NE8FDP` is a feedthrough, that is an RJ45 on
each side. That means PJRC's Ethernet kit can be KEPT inside and joined to the
panel with a short patch lead. No board has to be made.

Only if the internal patch lead is to be eliminated is the etherCON adapter for
the Teensy 4.1 needed, which is open electronics and replaces PJRC's kit:
https://github.com/tommag/Teensy4.1_Ethercon_adapter

### Clock input, and the problem it hides

See the "From word clock to PPS" section further down: **it is not just a
BNC.**

The connector is a 75 ohm BNC, the only one on the box, and the cable has to be
75 ohm coax (RG59). The
input has to be terminated at 75 ohms, as audio equipment does.

The termination resistor goes at the connector, not next to the comparator. Any
panel BNC does while the enclosure is printed: with no metal around them there
is nothing for the shell to be isolated from. See "Grounding" below.

### PPS output: a test point, not a connector

The pin 24 PPS does not come out to the panel. It goes to a **test point inside
the box**: a pad or a header pin next to the Teensy, with a ground pin beside
it so a scope probe has somewhere to clip.

That is the trade taken deliberately. The panel stays at three connectors,
which is what a small printed box wants, and the accuracy can still be measured
by opening it, which is a thing done on a bench and not in service.

Keep **R8, the 75 ohm series resistor at the pin**, anyway. It costs one component,
it damps whatever gets clipped to the test point, and it is what makes the
point usable with a real cable if the measurement ever needs one.

### Power

**The enclosure is printed, so mains does not come in.** A certified 5 V
adapter outside, a barrel jack on the panel, and nothing in this box that a
plastic part has to insulate. There is no chassis to bond a protective earth
to, the clearances of a printed part are not qualified for anything, and PLA
and PETG are not self-extinguishing. An IEC inlet with a supply inside is the
right answer in a metal chassis and the wrong one here.

- **Adapter**: 5 V, 1 A or more, a certified one. The load is a couple of
  hundred milliamps for the Teensy with its PHY plus 3.2 mA for the comparator,
  so the rating is about having margin, not about need.
- **J3**: 2.1 x 5.5 mm barrel jack, panel mounted, centre positive. Say which
  polarity it is on the panel, next to the jack, in ink that will still be
  there in five years.
- **D2, a Schottky in series** right behind the jack, an SS14 or the like. A 5 V
  adapter plugged in the wrong way round kills the Teensy, and the diode costs
  0.3 V of the 5 V, which leaves 4.7 V against a VIN that accepts 3.6 to 5.5 V.

The Teensy can be powered over USB or over VIN, but **not both at once**:
internally there is no switching and the two supplies would be shorted
together. The **track between the VUSB and VIN pads** has to be cut, on the
underside, under the USB connector.
https://www.pjrc.com/teensy/external_power.html

Once cut, USB still serves for console and programming without powering the
board, which is exactly what is wanted: the console lead can go in and out with
the box running.

### The lock LED

Green, 3 or 5 mm, in a panel holder. Anode to pin 13 through **R9, 330 ohms**,
cathode to 0 V, with the resistor at the board end so the pad is protected
along the whole run.

A green LED drops about 2 V, which leaves 1.3 V across 330 ohms, that is
3.9 mA: bright enough behind a panel and nowhere near what the pad can give. If
it turns out dim behind a diffused lens, 220 ohms takes it to 5.9 mA. Pin 13
also drives the LED on the Teensy itself, so both light together and the pad
carries both; that is still under ten milliamps.

Pin 13 doubles as SCK, which nothing in this project uses.

**What it means**, because a lamp that means the wrong thing is worse than no
lamp. `loop()` lights it from `isLocked()`, which is the servo having strung
more than five measurements together AND the PPS still arriving. So:

- On: the box is disciplined and announcing `clockClass` 13.
- Off: free-running, announcing 248. Either it has not converged yet, or the
  reference is gone.

It does not distinguish those two, and on a one-lamp panel that is the right
call: what a person in front of the rack needs to know is whether to trust the
clock.

### The console USB

The Teensy's socket is micro-B, which has no business on a panel. Put a
**panel-mount type B socket** on the front and a short B-to-micro-B lead inside.
Type B is the rugged one and it is what audio gear uses; type A facing out
would say host, which this is not.

Neutrik's D-series `NAUSB-W` feedthrough is the alternative if the panel is
ever drilled to match the etherCON. Check which type faces outward before
ordering, and note that a D cutout is large for a small printed box.

- **With the VUSB track cut, that lead carries no power.** Console and
  programming only, and the box can be plugged into a laptop and unplugged
  again while it runs.
- **Strain relief on the internal lead**, secured to the box, not to the
  Teensy. Micro-B sockets tear off boards, and this one is soldered to the part
  that is the whole product.
- **Leave the program button reachable.** With the lid on there is no way to
  press it, and it is what recovers a board whose sketch has taken USB down.
  The cheap answer is a lid that comes off without tools; the other is a hole
  and a plunger.

### Grounding

A printed box has no chassis, so there is no third thing for grounds to meet
at, and most of what is usually said about this does not apply.

- **No protective earth**, because there is no mains and nothing conductive to
  bond.
- **The coax braids land on the board's 0 V**, which is the return the 75 ohm
  termination needs. Whether the BNC shells are the isolated kind stops
  mattering: there is no metal for them to be isolated from.
- **One 0 V, and keep the termination return off the digital ground.** The
  input stage's ground goes back to the board by its own path, not shared with
  the Ethernet's return.
- **USB is a path of its own.** The console lead ties the laptop's ground to
  the board's 0 V. It does not matter for programming; if the box is ever
  measured for jitter with a laptop attached, that lead is the first thing to
  unplug before believing the numbers.

**If this ever moves to the metal rack chassis specified above**, all of it
changes: protective earth to the chassis at the inlet, isolated BNCs so the
braid does not sit in parallel with the mains earth, and 0 V meeting the
chassis at exactly one point near the connectors.

### Panel

**Front**, two things, specified below.

- Lock LED.
- Console USB.

**Rear**, three connectors.

- etherCON `NE8FDP`, with a short patch lead to PJRC's Ethernet kit inside. In
  a printed box the alternative is worth weighing: a plain RJ45 feedthrough, or
  a slot that lets PJRC's own socket reach the outside, saves a D-series cutout
  that is large next to a small box. The etherCON earns its size on a panel
  that gets plugged and unplugged; behind a desk it may not.
- Word clock input, 75 ohm BNC, with the conditioning circuit behind it.
- 5 V barrel jack, 2.1 x 5.5 mm, centre positive, with the Schottky behind it.

Inside, next to the Teensy: the PPS test point and its ground pin.

The BNC part number has to come out of the catalogue. Neutrik's D series
(`NBB75DFI` and relatives) shares the etherCON cutout, which is worth having if
the panel is ever drilled in metal; printed, the hole is whatever the model
says, so a plain bulkhead BNC is just as good and smaller.

### Inside the box: how it goes together

The model at `enclosure/aes67-masterbox.scad` is built around this layout, so
the two move together: change where a connector goes here and the parameter in
the model changes with it.

Orientation decides most of the rest, and it follows from the board itself. The
Teensy's USB is at one end and its Ethernet pads at the other, and the pins the
divider uses, 14, 15 and 19, sit at the Ethernet end of the right-hand header.

**So: USB end to the front, Ethernet end to the rear.** That puts the console
lead and the program button where the front panel is, the MagJack right behind
the etherCON with a short patch lead, and pins 14 to 19 in the rear corner,
which is where the word clock arrives.

Plan view, looking down:

    FRONT
    +--------------------------------------------------+
    |  LED1                    J4 USB-B                |
    |                                                  |
    |        +----------------------------+            |
    |        |  Teensy 4.1   USB end      |            |
    |        |                            |            |
    |        |  pin 19 --R7-- pin 15      |            |
    |        |  pin 14 <-------------+    |            |
    |        |  Ethernet end         |    |            |
    |        +---[ MagJack ]---------|----+            |
    |                                |                 |
    |                          [ conditioning ]        |
    +----------[ J2 ]---------------[ J1 ]---[ J3 ]----+
    REAR

**The one conflict in the layout, said plainly.** The two things that most want
to be apart are at the same end: the Ethernet magnetics and the comparator.
That is not a choice, it is where pin 14 is. What can be done is done here: J1
in the rear corner with the conditioning board immediately behind it, the
MagJack offset the other way, and the comparator's output taken to pin 14 along
the outer wall rather than across the middle. Keep the two as far apart as the
box allows and expect that to be tens of millimetres, not more.

**Placement rules, in the order they matter:**

1. **R1 and C1 at J1.** The termination belongs at the connector, and the
   conditioning board goes immediately behind it. This is the whole reason for
   putting J1 in a corner.
2. **The bridge stays on the Teensy.** Pin 19 to pin 15 through R7, about
   10 mm, flat against the board. Nothing about it leaves the header.
3. **R8 and the test point at pin 24**, with TP2 to 0 V beside it, somewhere a
   probe reaches with the lid off.
4. **Power in the far corner from the comparator.** J3, D2 and the run to VIN
   along the wall the input stage is not on.
5. **The USB lead down the opposite side** from the input stage, with its
   strain relief anchored to the box.
6. **0 V meets at the conditioning board**, and the braid from J1 goes straight
   there. See "Grounding".

**Mounting.** Teensy on standoffs with heat-set inserts, which is also where
the assembly trap gets solved: the MagJack is as wide as the Teensy and its
pins collide, so it is mounted offset or on long pins, and that sets the
interior height. Conditioning board on its own standoffs, not on tape: it has a
coax braid pulling on it.

**Assembly order**, because two of these cannot be undone later:

1. **Cut the VUSB to VIN track first**, before anything is mounted. It is on
   the underside, under the USB connector, and it is far easier with a bare
   board in hand.
2. Solder the Ethernet kit to the Teensy.
3. Build the conditioning board and test it on the bench, before it is inside
   anything: feed it a signal, look at its output.
4. Fit the panel parts, then the boards, then wire between them.
5. The bridge last, since it is the one thing that can be added with everything
   else already in place.
6. Lid off, power up, and walk the three checks in "The bridge, specified"
   before closing it.

## From word clock to PPS

The Rosendahl Nanosyncs puts out **word clock**, not PPS: 75 ohm BNC outputs,
3 Vpp, 40 to 200 kHz. That is the sampling rate, not one pulse per second. The
code expects 1 PPS. A division is missing.

On the old Raspberry Pi project a separate ATtiny85 did it. Here it is done
with a peripheral the Teensy already carries, adding no hardware and spending
no CPU: QuadTimer 3. Implementation in `src/wordclock.{h,cpp}`.

### No GPS: the reference is the word clock

The usual grandmaster takes its PPS from a GPS receiver, and that is what
t41-ptp is built around: its `README.md` describes the reference setup as an
Adafruit Ultimate GPS wired to the master over PPS. The GPS is the wiring, not
the software: the library carries no GPS code at all, only
`ppsInterruptTriggered(pps_ts, local_ts)`, which is disciplined by whatever
pulse arrives and never asks where it came from.

In this box the GPS disappears and the word clock takes its place. The
Nanosyncs is already the house reference and everything hangs off it; bringing
in a second reference would mean two of them in one room.

What that costs is absolute time, not precision. A GPS gives frequency, one
edge per second AND traceable time of day; the word clock gives the first two
and nothing else. Hence `clockClass` 13 with `timeSource` OTHER instead of 6
with GPS, and the UTC offset sent with the validity bit false. For AES67 audio
only relative synchronisation matters, so nothing is missing in practice.

Adding a GPS later is not a matter of wiring one in. Its PPS and the divider
pulse both land on pin 15, which is the only 1588 capture input, so it is one
or the other. That path means picking the source, reading NMEA over a serial
port for the time of day, and only then are `clockClass` 6 and a valid UTC
offset the truth.

### How it works

Two cascaded stages inside the same QuadTimer 3:

1. Channel 1 divides the word clock by 100.
2. Channel 0 counts channel 1's output and divides by the rest
   (frequency / 100), and puts the pulse out on a pin.

The cascade is not a whim: **the QuadTimer's counters are 16 bits**. 48000
would fit, but 96000, 176400 and 192000 do not. Every standard rate is a
multiple of 100 and neither factor goes past 16 bits.

This works because a channel's PCS field can pick as its source another
channel's input pin (values 0 to 3) or its output (4 to 7). Channel 1 counts
channel 2's pin without channel 2 having to do anything.

### Wiring, essential

    conditioned word clock  ->  pin 14
    pin 19  ->  pin 15          PHYSICAL BRIDGE

The bridge is needed because the QuadTimer output and the 1588 capture input
are different pads and cannot be joined internally. Pin 15 (pad AD_B1_03) can
be QuadTimer3 channel 3 (ALT1) or ENET_1588_EVENT2_IN (ALT4), and we want it as
the capture input, so the divider cannot put the pulse out there.

Pins and ALT values taken from the Teensy core's tables
(`cores/teensy4/pwm.c`), not from memory: pin 14 = AD_B1_02 = QuadTimer3
channel 2 ALT1; pin 19 = AD_B1_00 = QuadTimer3 channel 0 ALT1.

### The bridge, specified

It is one wire and it still deserves a specification, because the edge it
carries is the edge the 1588 capture timestamps. Everything upstream of it, the
comparator included, exists to make that edge clean.

    pin 19 ---[ R7 220 ]--- pin 15

Both pins are on the same header row (14 to 23 down the right-hand side) and
sit four positions apart, so the run is about 10 mm.

- **R7, 220 ohm, at the pin 19 end.** Two jobs. It damps the run, and it caps
  the current if firmware ever leaves pin 15 configured as an output and the
  two drive against each other: 3.3 V over 220 ohms is 15 mA, which neither pad
  minds. A 33 ohm would allow 100 mA, which they would.
- The resistor costs nothing in edge speed. Against the pad's input
  capacitance, on the order of 10 pF, 220 ohms gives a time constant of about
  2 ns on an edge that only has to be resolved to a nanosecond, and the
  captured pulse is 4 ms wide at 48 kHz.
- **Solid core wire, AWG 24 to 30**, run flat against the board and as short as
  it goes. On a board, a trace.
- **Keep it away from the Ethernet magnetics, the crystal and the word clock
  input.** It is a 3.3 V edge with fast rise going right past the analogue
  front end that everything else depends on.

**When it is missing**, and this is worth knowing before hunting for it: the
serial warning about the word clock only covers the input on pin 14, so a
missing bridge says nothing. The symptom is a box that measures its word clock
correctly, reports the divider started, and then never locks: `clockClass`
stays at 248, the LED on pin 13 stays off, and no PPS interrupt ever arrives.

**Checking it**, in this order, because each step depends on the one before:

1. A scope on pin 19: one pulse per second, some 4 ms wide at 48 kHz.
2. The same pulse on pin 15, with the bridge in place.
3. The LED on pin 13 lighting up once the servo has strung five measurements
   together.

### Signal conditioning: the circuit

**The word clock canNOT be connected directly to pin 14.** It is 3 Vpp into
75 ohms, the pin wants 3.3 V logic and the Teensy 4.1 is not 5 V tolerant.
Connecting it directly will not work and may do damage.

The pin 14 input is configured with hysteresis (Schmitt trigger) in
`src/wordclock.cpp`. That stays, because it costs nothing, but it is no
substitute for what follows: the pad's own hysteresis window is not
specified tightly enough to build a clock reference on.

What decides the design: the divider takes one edge in 48000 and the jitter of
that one edge goes straight through to the PPS. Nothing averages it out. That
is why this is a fast comparator with deliberate hysteresis and not a logic
gate.

Signal path:

    BNC -> R1 (75, to GND) -> C1 -> node A -> U1 IN+ -> U1 OUT -> R6 -> pin 14

Netlist, by node:

    A       C1, R2 (to 3V3), R3 (to GND), D1 (both diodes), Rf, U1 IN+
    REF     R4 (to 3V3), R5 (to GND), C2 (to GND), U1 IN-
    OUT     U1 OUT, Rf, R6
    V+      U1 V+, C3 (to GND), C4 (to GND), 3V3 from the Teensy
    SHDN    U1 SHDN, straight to GND: the part runs only while this pin is
            held well below V+, so it is not a pin to leave floating

Rf goes from OUT back to A: that is the hysteresis, and it is the only feedback
in the circuit.

**U1**: TLV3501AIDBVR, package DBV, **SOT-23-6 and not 5**: the sixth pin is
the shutdown. Single supply 2.7 to 5.5 V, 3.2 mA typical, 4.5 ns propagation
delay, push-pull output, and an input common-mode range that goes 0.2 V beyond
both rails. Fed from the Teensy's 3V3 pin.

**SHDN goes to ground.** It is specified in thresholds rather than logic
levels: within 0.9 V of V+ the part is disabled, and more than 1.7 V below V+
it runs. The datasheet's own instruction for not using the feature is to
connect the pin to the most negative supply, which here is ground. Left
floating it is anyone's guess, which on the comparator that carries the box's
clock reference is not a thing to find out in service.

**Pin assignment**, DBV / SOT-23-6:

    1 IN-    2 V-    3 IN+    4 V+    5 OUT    6 SHDN

Three sources agree on it, and one of them is a concrete artifact rather than a
summary: the KiCad symbol at
`kwikius/quantracker/kicad/lib/tlv3501.lib`, which numbers the pins
1 `-`, 2 `V-`, 3 `+`, 4 `V+`, 5 output, 6 `E`. It still is not TI's own
datasheet, which cannot be reached from here, so **check it before laying
anything out**: IN+ and IN- the wrong way round gives a board that looks dead
rather than one that looks wrong, and the netlist above is written by function
so that it survives being checked.

The inputs have to be rail-to-rail and that is not a preference. Node A sits at
1.65 V and swings 1.5 V either way, so 0.15 to 3.15 V. The TLV3501's range goes
0.2 V past both rails, so it covers that with room to spare; a cheaper
comparator whose common mode stops about a volt below V+, such as the LMV7219,
does not reach it and will misbehave at the top of every cycle.

**It also has 6 mV of internal hysteresis of its own.** That does not replace
Rf: 6 mV is noise immunity at the die, and the 51 mV Rf gives is what holds the
output still on a soft edge. The two add, the external one dominates, and
neither is large enough to move the trip point anywhere that matters on a 3 Vpp
signal.

**Where these figures come from.** TI's site, and every datasheet mirror tried,
is unreachable from the environment this repository is worked in: the egress
proxy refuses them. So the numbers above were confirmed from secondary sources,
two or more agreeing for each, and the pin assignment is left open precisely
because they did not agree about it. Anything here marked as needing the
datasheet needs the datasheet, not another search.

| Ref | Value | What it is for |
| --- | --- | --- |
| R1 | 75 ohm 1%, 0.1 W | Termination, mounted AT the BNC. 3 Vpp square across 75 ohms is 30 mW, so a 0603 has margin to spare. |
| C1 | 100 nF X7R 50 V | AC coupling, so the source's DC offset does not matter. Against the 10k/10k divider (5 kohm) the high-pass corner is 318 Hz, more than two decades below the lowest rate. |
| R2, R3 | 10 kohm 1% | Bias node A to 3V3/2 = 1.65 V. They also load the signal, but 5 kohm against the 75 ohm termination costs 1.5% of amplitude. |
| D1 | BAT54S | Clamps node A to the rails. A 5 Vpp source would put node A at 4.15 V peak; the diode conducts at about 3.6 V and passes some 7 to 15 mA depending on whether the source impedance is counted. Well inside the part. |
| Rf | 4.7 kohm | Positive feedback, IN+ from OUT. This is the hysteresis that matters, on top of the part's own 6 mV, and without it a slow or noisy edge makes the output chatter. |
| R4, R5 | 10 kohm 1% | Reference divider, IN- at 1.65 V. |
| C2 | 1 uF X7R | Holds the reference node still. The reference is DC, any noise on it lands straight on the trip point. |
| C3, C4 | 100 nF + 1 uF | Supply decoupling at V+, within a few millimetres of the part. |
| R6 | 33 ohm | Series damping at the output, at the comparator end, for the run to pin 14. |

**Hysteresis, worked out.** The source impedance at IN+ is the 75 ohm
termination through C1, in parallel with the 5 kohm divider: about 74 ohms. The
window is then 3.3 V x 74 / (74 + 4700) = 51 mV total, that is +/-26 mV about
the trip point. On a 3 Vpp signal that is a fortieth of the amplitude: enough to
kill chatter, small enough not to move the edge anywhere that matters.

The trip point sits at the signal's own average, which is what AC coupling
buys. A word clock with a markedly asymmetric duty cycle moves it off centre,
and with 3 Vpp against 26 mV there is room for a lot of asymmetry before that
becomes a problem.

**Grounding.** The BNC shell ties the Nanosyncs chassis to this box's. Both are
rack gear on the same mains, so it is a short bonded path and not a loop worth
losing sleep over, but keep R1 at the connector and bring that ground back to
the Teensy by one path, so the termination return does not share copper with
the digital ground.

**Layout.** R1 at the BNC. Node A short. Rf next to the comparator, not next to
the connector, or the feedback picks up what the input picks up. R6 at the
comparator end of the run to pin 14.

**What is not verified.** Nothing has been built or measured; this is a design
on paper. Two things to check with a scope before trusting it: the real
amplitude the Nanosyncs puts out (the clamp margins assume 3 Vpp and survive
5 Vpp, but not more), and that node A really does swing symmetrically about
1.65 V once the cable is on.

### Sampling rate

It is picked at run time from the web page, out of a closed list (44100, 48000,
88200, 96000, 176400, 192000), and the choice is stored in EEPROM at address 16
(the PTP profile has address 0). 48000 with a blank EEPROM, which is what the
code carried hardcoded before. The list is closed because the divider only
takes rates that are a multiple of 100 and whose second factor fits in 16 bits.

It has to match the generator: if it does not, the PPS will not run at 1 Hz.
Changing it restarts the divider on the spot, with no restart of the box.

At start-up the real frequency is measured and a warning goes out over serial
if it does not add up, but **it is not changed automatically**. A box that
reconfigures itself silently ends up with somebody hunting for hours for why
PTP moved.

The measurement uses a 200 ms window, not a one-second one, because the counter
is 16 bits: at 192 kHz one second would give 192000 edges and would wrap three
times.

### What is NOT verified

None of this has been tested on hardware, and there is more risk here than in
the rest of the project because it is register code.

One specific trap that nearly slipped through: `OUTMODE` 6 looks like the
natural mode ("set on compare, cleared on counter rollover") but it has a
documented erratum saying the output is not cleared on rollover when the
counter runs up, which is our case, and it would stay high forever. `OUTMODE` 4
is used instead, alternating compares, which is what NXP uses to get PWM out of
the QuadTimer.

Once there is a board, the first thing to do is to look at pin 19 with an
oscilloscope and check there is one pulse per second before believing anything
else.
