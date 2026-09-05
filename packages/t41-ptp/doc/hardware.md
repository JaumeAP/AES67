# The hardware this runs on

What to buy, and why each piece is there. Everything below is ordered from
Amazon Spain, which is the channel available for this build — the same
parts exist at PJRC, SparkFun, RS and BricoGeek, and the links here are
simply the ones that can be ordered.

## The board

**Teensy 4.1**, with the pin headers already soldered.
<https://www.amazon.es/Teensy-4-1-Con-Pins/dp/B08CTM3279>

It is the board this library was written for and it stays: the i.MX RT1062
carries an Ethernet MAC with IEEE 1588 timestamping in hardware, which is
the one feature none of this works without. PJRC has nothing newer with
Ethernet — the 4.1 is the top of the line — and since SparkFun became the
sole manufacturer and sales channel it reaches Spanish distributors
through their network.

"With pins" means the header strips along the edges are soldered on, ready
for a breadboard or a socket. The version without them ships with bare
holes.

## Getting it onto a network

**Ethernet Kit for Teensy 4.1**.
<https://www.amazon.es/-/en/Ethernet-Kit-for-Teensy-4-1/dp/B08CTM1L64>

The Teensy 4.1 has the MAC and the PHY but no connector. The kit is the
RJ45 MagJack, a small board, the ribbon cable, two header pins and a
capacitor. **It has to be soldered**, and the six-pin header it plugs into
is not fitted on either version of the board.

Without this the board has no network at all: no PTP, no tone, nothing.

## The rest

Generic parts, listed for completeness rather than for a particular
product:

1. **USB micro-B cable** — programming and power.
2. **Cat5e or Cat6 cable**.
3. **Soldering iron and solder** — for the Ethernet kit, and for the
   headers if the board came without them.
4. **A managed switch with IGMP snooping** — AES67 is multicast, and a
   switch without it floods every port. This is the difference between an
   audio network that works and one that does not, and it matters long
   before any of the fancier switch features do.

## The enclosure

Nineteen inches, in a rack beside the switch.

1. **A 1U metal rack case** — <https://www.amazon.es/s?k=caja+rack+19+1U+chasis>.
   The board sits inside on standoffs.
2. **A panel-mount RJ45 keystone coupler** on the front or back
   — <https://www.amazon.es/s?k=keystone+rj45+cat6+acoplador> — with a
   short patch lead from the MagJack to it
   (<https://www.amazon.es/s?k=latiguillo+cat6+0.25m>). The pull of a
   cable then lands on the coupler and not on a soldered kit board, which
   is the fragile part of the whole assembly.
3. **M3 nylon standoffs and screws**
   — <https://www.amazon.es/s?k=separadores+m3+nylon+placa>.

Leave it ventilation. The crystal's drift is what the servo spends its
time correcting, and a hot, sealed 1U in the middle of a rack gives it
more to correct.

## The reference input

A BNC on the front panel, for a 1 PPS from a GPS or from a house
reference. `PTPBase::ppsInterruptTriggered` is the path it feeds.

1. **Panel-mount BNC socket**
   — <https://www.amazon.es/s?k=conector+bnc+hembra+panel+chasis>, or an
   isolated one
   (<https://www.amazon.es/s?k=conector+bnc+panel+aislado>) to keep the
   screen from making a ground loop with the rack.
2. **A termination resistor** across the input: 50 Ω for a GPS 1 PPS, 75 Ω
   if what arrives is word clock or video.
3. **A level shifter, and this one is not optional.** The Teensy 4.1's
   pins are 3.3 V and NOT 5 V tolerant: a 5 V PPS wired straight to a pin
   destroys it.

   The part to buy is a **74LVC245 in DIP-20**
   (<https://www.amazon.es/s?k=SN74LVC245AN>): an octal buffer run from
   the board's 3.3 V whose inputs take 5 V, through-hole, and cheap
   enough to keep spares. One channel is used and the other seven are
   left alone. Wire `OE` to ground so it is always on, `DIR` to whichever
   direction is wanted, the BNC into `A1` and `B1` out to the Teensy's
   pin, and power the chip from 3.3 V — powering it from 5 V is what
   would make the whole exercise pointless. A DIP-20 socket
   (<https://www.amazon.es/s?k=zocalo+dip+20+pines>) means it can be
   replaced without desoldering.

   A **74LVC1G17** — a single Schmitt-trigger buffer — is the tidier part
   and cleans the edge better, but it is made only in SOT-353 and is not
   sold on a breakout through the channel available here. The alternative
   without any chip is a 1 kΩ / 2 kΩ divider: two resistors, at the cost
   of a slower edge and more jitter. Fine for bringing the thing up, not
   what a house reference deserves.

   What must NOT be used here is a bidirectional BSS138 level-shifter
   module, the cheap four-channel kind. They are made for I2C; the edge
   that comes out is slow, and a slow edge on a timing reference is the
   one thing this input exists to avoid.

## Power

1. **A 5.5 x 2.1 mm panel-mount DC jack**
   — <https://www.amazon.es/s?k=conector+jack+alimentacion+panel+5.5+2.1>
   — and a 5 V supply
   (<https://www.amazon.es/s?k=fuente+alimentacion+5v+2a+5.5x2.1>).
2. **Cut the VUSB-VIN pads** on the back of the Teensy before powering
   VIN from that supply while USB is also plugged in. Uncut, the two
   sources fight each other through the USB port.

## The printed case

`hardware/teensy-case.scad` is the enclosure: a body and a lid, with the
three connectors this thing needs and nothing else — the keystone for the
network, the BNC for the reference, and the board's own USB reachable
through the end wall for power and programming.

    openscad -o teensy-case-body.stl -D part=\"body\" hardware/teensy-case.scad
    openscad -o teensy-case-lid.stl  -D part=\"lid\"  hardware/teensy-case.scad

The Teensy 4.1 has no mounting holes, so it is held the way every sled
holds it: by its long edges, in a pair of slots, with the rails printed
into the floor. Nothing screws to the board.

Every dimension that touches a bought part is a variable at the top of the
file. **Measure these three before printing**, because they are the ones
that vary between suppliers and a case that is half a millimetre out is a
case printed twice: the keystone's opening, the BNC's thread diameter and
its anti-rotation flat, and the room under the board if it is socketed
rather than sitting on its own pins.

The lid is vented on purpose. The crystal's drift is what the servo spends
its time correcting; a sealed box in a warm rack gives it more to correct.

Printing it needs no printer of your own — this is one small part and any
print service will do it — but if one is wanted, an entry-level machine
covers this comfortably.

## How much soldering

Not much, and one fiddly bit:

1. The Ethernet kit is about fifteen through-hole joints — the six-pin
   header, the MagJack's pins and the capacitor. Nothing fine.
2. The headers are already done if the board was bought with pins.
3. The BNC, the DC jack and the wiring are four or five forgiving joints.
4. The level shifter is the only fine-pitch work, and only if the
   74LVC1G17 is not bought ready-mounted on a breakout.

## What is worth adding later

1. **A GPS module with a PPS output**, if this board is to be the house
   reference rather than follow one — the front panel BNC above is what
   it feeds.
2. **An I2S DAC**, if a test tone should leave through an analogue
   output. The tone that goes on the network lives in the sketch that
   owns the audio -- `JaumeAP/aes67-master-box` -- and not in this
   library.
3. **A switch with a transparent or boundary clock**, for the tightest
   lock available. The `correctionField` is carried correctly in both
   directions now, so the residence time such a switch reports is actually
   used.

## What is on the other end

Nothing here is testable on its own: PTP needs something to synchronise
with, and a stream needs somebody to receive it.

1. **A clock to follow, or to be**: a real grandmaster, another Teensy, or
   the Mac running `aes67ptpd` from the aes67_macos_driver project.
2. **A receiver for the tone**: the AES67 macOS driver, or any AES67
   endpoint. What it should measure is 1 kHz at −20 dBFS RMS, one channel,
   48 kHz, one millisecond per packet.
