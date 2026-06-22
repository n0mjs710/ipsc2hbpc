## PROJECT: IPSC to HomeBrew Protocol Translator (C) ##

**NOTICE:** This software only supports Group Voice traffic at this time. IPSC is not an open standard. Supporting it involves painstaking reverse engineering of IPSC packets, and much is unknown. IPSC is owned by and heavily protected by Motorola Solutions, Inc. Please do not ask for features that require further deciphering of IPSC without providing verified correct, legally obtained information about the protocol necessary to support a feature.

**PURPOSE:** A single, small native daemon that connects a Motorola MOTOTRBO IPSC system to one upstream DMR network server running the open HomeBrew Repeater Protocol (BrandMeister, DMR+, FreeDMR, HBlink4, etc.).

`ipsc2hbpc` is a complete, feature-for-feature reimplementation of `ipsc2hbp` in C. It speaks the same protocols, reads the same configuration file, and produces the same wire output — but builds to a single dependency-free binary with no Python runtime, virtual environment, or third-party libraries.

Two IPSC modes are supported:
- **MASTER** (default) — ipsc2hbpc acts as the IPSC master; up to 14 MOTOTRBO repeaters register with it as peers.
- **PEER** — ipsc2hbpc registers with an existing IPSC master (e.g. a repeater configured as master). All traffic on that IPSC system is forwarded to HBP.

**WHY A C VERSION:**

The Python `ipsc2hbp` is a single asyncio process — already simple and effective. The C reimplementation keeps that exact design while removing the runtime dependencies entirely: there is no interpreter, no `pip`, no `dmr-utils3`, no `bitarray`. `make && ./ipsc2hbpc` is the whole story. The result is trivial to deploy on small/embedded systems and starts instantly with a tiny memory footprint.

**DESIGN GOALS:**

- **Transparent translation only.** No routing, bridging, talkgroup filtering, or rewriting. IPSC peers in, one network out, pass everything through unchanged.
- **Single event loop, single thread.** A `poll()`-based loop multiplexes the IPSC socket, the HBP socket, and all timers — directly mirroring the original's single asyncio loop. No threads, no locks, no inter-process communication.
- **One config file.** TOML, identical to the Python version. An existing `ipsc2hbp.toml` works unchanged.
- **Correct over clever.** Protocol behavior is derived from the DMRlink and HBlink source — not the published specs, which contain several errors. Where the spec disagrees with working code, the code wins.
- **No external dependencies.** Everything — the TOML parser, SHA-1/SHA-256/HMAC, and the DMR DSP/FEC code — is in this repository and compiled into the binary.
- **TRACKING mode by default.** The HBP connection follows the repeater: it comes up when the repeater registers and drops when the repeater goes away. PERSISTENT mode is available if you'd rather keep the upstream connection up regardless.
- **Native call timing on the IPSC side.** The whole HBP→IPSC call (headers, voice, terminator) is clocked out through one jitter buffer at the exact 60 ms DMR cadence, so the repeater sees a continuous grid identical to real MOTOTRBO equipment — no header→voice gap and no clipped tail. Depth is tunable per link via `[hbp] jitter_buffer_depth` (default 2 = 120 ms; raise it for marginal/high-latency RF backhaul).

**ARCHITECTURE:**

- `src/dmr/` — self-contained DMR DSP/FEC library (BPTC(196,96), embedded LC, AMBE 49↔72-bit conversion, RS(12,9), Hamming, Golay). This module has **no dependency on the rest of the project** and is validated bit-for-bit against the reference `dmr_utils3` implementation. It is structured so it can be lifted into a standalone library unchanged.
- `src/` — the application: config loader, logging, event loop, UDP helpers, crypto, the IPSC master/peer protocol, the HBP client, and the bidirectional call translator.
- `tests/` — a DSP self-test against golden vectors, and a byte-for-byte parity test comparing the C translator output against the Python reference.

**VERIFICATION:**

`make test` runs two suites:
- The DSP self-test confirms every FEC/DSP primitive matches `dmr_utils3` bit-for-bit.
- The parity test confirms the C translator emits byte-identical DMRD (IPSC→HBP) and GROUP_VOICE (HBP→IPSC) frames versus the Python `ipsc2hbp`.

**WHAT IT IS NOT:**

This is not a general-purpose bridge, reflector, or network controller. It does not route between talkgroups, filter, or rewrite calls. It works with HBlink4 and should work with any HBP-speaking network server.

**REQUIREMENTS:**

- A C11 compiler and `make`
- One Motorola MOTOTRBO repeater configured with this host as its IPSC master (MASTER mode), or an existing IPSC master to register with (PEER mode)
- One upstream HBP server (BrandMeister, DMR+, HBlink4, etc.)

**GETTING STARTED:**

```
make
cp ipsc2hbp.toml.sample ipsc2hbp.toml
# edit ipsc2hbp.toml for your repeater and network
./ipsc2hbpc -c ipsc2hbp.toml
```

See `INSTALL.md` for the systemd service and a note on coexisting with the Python `ipsc2hbp` during cutover.

**PROPERTY:**

This work represents the author's interpretation of the Motorola MOTOTRBO IPSC protocol and the HomeBrew Repeater Protocol. IPSC protocol behavior is derived from reverse-engineering work originally done in DMRlink. HBP behavior is derived from HBlink, HBlink3, and HBlink4. The DMR DSP/FEC code is a C port of `dmr_utils3` (N0MJS, with original AMBE work by Mike Zingman N4IRR and FEC routines after Jonathan Naylor G4KLX). Motorola and MOTOTRBO are registered trademarks of Motorola Solutions, Inc. This project is not affiliated with Motorola Solutions in any way.

Additional IPSC packet structure knowledge was derived from study of **node-dmr-lib** by rick51231: https://github.com/rick51231/node-dmr-lib

**WARRANTY:** None. Use at your own risk.

***0x49 DE N0MJS***

Copyright (C) 2026 Cortney T. Buffington, N0MJS <n0mjs@me.com>

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
