# This box against the DTS-Player, and whether it is worth continuing

Written on 2026-08-25 after comparing this project with
`JaumeAP/DTS-Player`, which runs on a Waveshare ESP32-P4-ETH. Worth
reading before buying hardware for this box.

## The board comparison

Teensy 4.1: 8 MB of flash, ~1 MB of RAM, one Cortex-M7 at 600 MHz.
ESP32-P4: 32 MB of flash, 32 MB of PSRAM, dual-core RISC-V.

Both have a hardware IEEE 1588 clock in the MAC. This box's only real
advantage was **packet timestamping**: here it works (t41-ptp calls
QNEthernet's `timestampNextFrame()` and `readAndClearTxTimestamp()`),
and on the P4 its own code said it could not be reached.

**That advantage does not exist.** See below.

## What the DTS-Player already has and this does not

- BMCA (`lib/PtpBmca`) and listening for external masters
  (`lib/PtpExternalReference`). There is no election here: this box
  proclaims itself master always.
- AES67 sender, RAVENNA RTSP, RTP packetising. There is no audio here.
- Host unit tests, 27 suites. There are none here.
- `docs/wordclock-input.md`: **the word clock input stage already
  designed**, with a 75 ohm BNC, divider values and the warning never to
  put 5 V on the pin. It is exactly the circuit that appears here as
  pending in `HARDWARE.md`.
- Hardware word clock counting with the PCNT, measuring the frequency
  directly. It is the same idea as the QuadTimer divider here, but
  better solved: there it is measured, here it is configured and warned
  about if it does not add up.
- `clockClass` 13 for a locked word clock. It was arrived at
  independently here and matches.

That document, moreover, cites this project as the origin of the input
stage. So the word clock "design hole" described here as found on
2026-08-25 had already been analysed there.

## The timestamping advantage: gone

The DTS-Player's `lib/PtpHardwareClock/PtpHardwareClock.h` says hardware
packet timestamping needs L2TAP and that the Arduino framework ships
ESP-IDF precompiled with `CONFIG_ESP_NETIF_L2_TAP` disabled.

That is only true for one of its two build paths. That repository also
builds with `idf.py` against its own `sdkconfig.defaults`, and there the
option is its own. On top of that, Espressif documents that the ESP32-P4
supports gPTP with full hardware timestamping in the EMAC, and ESP-IDF
ships an example at `examples/ethernet/ptp`.

And in June 2026 Espressif published AES67 over IP on the ESP32-P4, with
a component in the registry, code and an example: 0.7 to 2.7 ms end to
end, zero losses over 645,000 packets, some 650 KB and 22% of the
internal RAM.

https://developer.espressif.com/blog/2026/06/aes67-audio-over-ip-on-the-esp32-p4/

## Conclusion

The Teensy brings nothing the P4 cannot do, and the P4 does things this
box will never do with 1 MB of RAM.

Recommendation: **do not add the Teensy to the DTS-Player repository**,
and consider whether this project makes sense separately or whether the
effort should go to the P4, where an AES67 component has already been
published.

No decision taken. The code here compiles and works as far as could be
verified, which is without hardware.

## If it is continued anyway

The DTS-Player's word clock input stage in `docs/wordclock-input.md` is
directly reusable and closes the hole left open here.

And one detail from that Espressif article applies to any AES67
implementation, this one included: they had to enable 802.3x flow
control to stop losing multicast frames. AES67 runs entirely over
multicast.

## Not verified

None of this has been built or run. The comparison comes from reading
the DTS-Player's code and documents and Espressif's documentation, not
from measuring either board.
