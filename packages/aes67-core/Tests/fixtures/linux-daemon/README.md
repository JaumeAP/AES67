# SDP from the Linux daemon

Six SDP documents produced or accepted by `aes67-linux-daemon`, the mature
Linux AES67 implementation, taken from its own README, its test suite, its
session manager and its latency test.

They are here as an **oracle**. This library's SDP parser is checked against
RFC 4566 by its own suite, which proves it agrees with a reading of the
standard. These fixtures prove something the standard cannot: that it agrees
with an implementation that real devices already interoperate with.

That distinction has already paid once. `aes67-core` carries a commit —
`fix(sdp): accept RFC 7273 bare-domain ts-refclk` — for a form this daemon
emits and the parser rejected. Nothing in RFC 4566 required fixing; the
disagreement was with reality.

## Provenance

Upstream is
[`bondagit/aes67-linux-daemon`](https://github.com/bondagit/aes67-linux-daemon),
GPL-3.0 — the same licence this repository carries. They are session
descriptions: data the program prints, not the program.

They were taken through a private copy of that project whose only changes were
a submodule bump and some editor configuration — no change to the daemon
itself. That copy has since been deleted, which costs nothing here: the copy
was never load-bearing, these fixtures are files now, and re-deriving or
extending them should go to upstream, which is the implementation devices
actually interoperate with.

Addresses and clock identifiers are whatever the source files had. They are
example values from documentation and tests, not a capture of anyone's network.
