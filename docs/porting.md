# Porting this to other radios

What in this repository is reusable for a radio that is not an MT7996, what a new
radio has to supply, and which candidate silicon is worth the attempt.

Two independent questions, kept apart here because they have different answers:

1. **The software boundary.** Where does chip-specific code end and portable code
   begin, and what exactly does a new radio have to emit? Answered precisely
   below: it is two wire contracts and about 7,900 lines of reusable host code.
2. **The hardware premise.** The MT7996 is not an arbitrary Wi-Fi chip. Three
   properties made this project possible, and most candidates fail at least one.

## 1. What is portable

Measured over the tree (files that name `mt7996`, `mt797x`, `icap`, `wfdma`,
`an7581` or `airoha`):

| Component | Lines | Chip-coupled files | Portable |
| --- | --- | --- | --- |
| `host/spectrum/` | 5,958 | 0 / 23 | **yes** — FFT, waterfall, web UI, controller protocol |
| `host/recv/` | 339 | 0 / 3 | **yes** — recorder |
| `host/stream/` | 1,598 | 1 / 10 | **mostly** — see the couplings below |
| `init/` | 2,951 | 3 / 23 | partly — UART/TFTP/provision logic is generic, layouts are not |
| `host/w1700k/` | 2,259 | 8 / 12 | no — commands are MT7996 firmware commands |
| `kernel/w1700k_sdr/` | 9,084 | 28 / 40 | no — WFDMA, firmware download, MT7996 ABI |
| `firmware/` | 7,940 | 40 / 50 | no — RISC-V images for the MT7996's own MCUs |
| `platform/` | 9,208 | 49 / 102 | no — AN7581 U-Boot, DTS, board patches |
| `include/hw/` | 23,097 | 44 / 45 | no — the register reference itself |

So the portable fraction is roughly **7,900 of 62,500 lines (13 %)**: the host
stack from the UDP socket upward. Everything below it — firmware, driver,
register headers, SoC platform — is per-chip and has no reuse value at all.

The boundary is worth preserving as-is, because it is already clean: the spectrum
server and the recorder never learn what radio produced the samples. They read a
shared-memory ring. Only `w1700k-stream` touches the wire.

### Contract A — the export datagram (radio side → host)

One UDP datagram per capture-ring completion. From `kernel/w1700k_sdr/export.c`
and `host/stream/src/ingest.cpp`:

```
offset  size  field
  0      4    le32  magic = 0x31524453  ("SDR1")
  4      2    le16  offset       byte offset of this piece within the slice
  6      2    le16  slice_bytes  total bytes of the slice
  8      8    le64  seq          completion number, monotonic within a run
 16      N    payload
```

Rules the host receiver enforces:

- `slice_bytes < 4096` ⇒ a **firmware event**, not samples. Published as
  `TAG_EVENT`; the slices either side are treated as contiguous in time.
- Otherwise `slice_bytes` must be exactly 8192 (raw: 2048 samples × 4 bytes) or
  7168 (packed: 2048 samples at 28 bits per I/Q pair). Anything else is counted
  `bad` and dropped.
- Whole slices only on the fast path: `offset == 0 && len == slice_bytes`.
  Fragments are counted `pieces` and dropped.
- `seq` defines runs. A new run starts on: 1 s of silence, a backward jump of
  more than 2²⁰, a forward jump of more than 2³⁹, or a consumer's
  `restart_requests`.
- Datagrams are received into 9216-byte buffers; the link needs MTU ≥ 8236 and
  ~48 MiB socket buffers (`net.core.rmem_max`).
- Source ports are spread by the sender so that `SO_REUSEPORT` across 8 receive
  threads balances.

A new radio that emits exactly this needs **zero changes** to `w1700k-stream`'s
transport layer. The reorder ring, loss accounting, run detection and the
shared-memory publisher all work unchanged.

### Contract B — the shared-memory ring (host service → consumers)

`host/stream/include/w1700k_stream.h`, a single POSIX shm object
(`/dev/shm/w1700k-stream`):

- 4 KiB header: layout, a seqlock-protected `(run, newest)` position, ~25
  counters, a 100 ms heartbeat, and control words consumers write
  (`restart_requests`, `flatten`, `radio_sample_rate`, `radio_capture_rate`,
  `radio_filter_hz`).
- One `u64` tag per slot: `(run << 40) | (index + 1)`, with bit 63 `TAG_EVENT`,
  bit 62 `TAG_LOST`, bit 61 `TAG_FLAT`.
- One slice of 2048 complex samples per slot, `int16` I then `int16` Q, full
  scale 8192 (the radio's 14-bit range), the headroom above it reserved for the
  correction filter's gain.
- Readers use a seqlock per slot: read tag (acquire), read samples, read tag
  again.

This is a good general-purpose IQ transport and is the piece most worth keeping.

### The couplings that need parameterizing

`w1700k-stream` is "mostly" portable rather than fully, because six MT7996 facts
are compiled into it. A port should hoist each into a source descriptor
negotiated at startup:

| Coupling | Where | Today |
| --- | --- | --- |
| Slice size | `W1700K_STREAM_SLICE` | 2048 samples, a compile-time constant used in pointer arithmetic |
| Sample depth | `W1700K_STREAM_FULL_SCALE` | 8192; 14-bit, I in `[29:16]`, Q in `[13:0]`, spares always zero |
| Packed format | `ingest.cpp`, `kPackedBytes` | the WM/WA 28-bit scheme, 8 words → 7 |
| Event threshold | `kMinSliceBytes` | 4096, i.e. "shorter than half a slice" |
| Staleness check | `spare_words`, `stale_slices`, `stale_lines` | compares against one lap of a **1 MiB / 8 KiB = 128-completion** capture ring, and keys on `MT7996_IQ_SPARE = 0xc000c000` |
| Roll-off correction | `host/stream/src/flatten.cpp` | measured tables for exactly 40/80/160/320 Msps on unit A, a 129-tap FIR capped at +24 dB |

The last two are the only ones that are genuinely *measurements* rather than
parameters. `flatten` is per-silicon and per-unit by nature: any new radio needs
its own decimation response characterized on a bench. Treat it as a pluggable
table keyed by `(source_id, rate)`, not as a constant.

### The minimum a port must build

```
         ┌─────────────────────────── per chip ────────────────────────────┐
  radio capture engine → on-chip buffer → egress DMA → transport → Contract A
         └─────────────────────────────────────────────────────────────────┘
                                                              │
         ┌──────────────── reusable as-is (7.9k lines) ────────▼───────────┐
                        w1700k-stream → Contract B → spectrum / recv / …
         └─────────────────────────────────────────────────────────────────┘
```

Plus a control plane. The spectrum server drives the radio through an opaque
subprocess (`--controller "host/bin/w1700k serve"`) speaking a line protocol, so
a port supplies its own controller binary and the UI needs no change — the
cleanest seam in the project. A port's controller must implement
`load/start/band/tune/gain/filter/stop/status`; anything it cannot do should
report as unsupported rather than error.

## 2. The hardware premise

Reading the roadmap backwards, the project rests on three properties. These are
the screening gates for any candidate.

**Gate 1 — a wideband raw-IQ capture engine exists in silicon.**
The MT7996 has three `ICAP` instances with a register surface that includes
`SRC_SEL` (tap the AFE or the DFE), `RING_ARM`/`RING_BUF`/`RING_WRAPPED`, a
trigger block, and a capture-route block that can redirect ICAP0 into the 1 MiB
UMAC SYSRAM pool. The repository's own comments identify this as the stock
**TESTMODE** firmware's route — a factory/characterization facility, not a
product feature.

That is the generalizable insight: **wideband capture blocks exist in most modern
radio silicon because the vendor needs them for their own calibration, DFS and
production test.** They are undocumented, not absent. The ESP32 case below is
the same story with a different vendor.

The crucial sub-property is that ICAP can run as a **continuous ring** rather than
a one-shot snapshot. A snapshot-only block gives a spectrum analyzer; a ring
gives an SDR. Most vendor capture blocks are specified for snapshots, and whether
they can be coerced into a ring is the single highest-risk unknown in any port.

**Gate 2 — you can run your own code on the radio.**
On the MT7996 the ROM authenticates neither the patch's hook table nor, behind a
CRC hook, the RAM images. Three hooks and the WM/WA are yours. Without
equivalent code execution there is no way to service a capture ring at rate — a
host poking registers over a bus cannot follow a write pointer at 320 Msps.

**Gate 3 — enough egress bandwidth to matter.**
320 Msps × 32 bits = 1.28 GB/s. The AN7581's 10 GbE carries ~1.24 GB/s, which is
why half the slices are packed to 28 bits on the radio's own cores. This gate
eliminates more candidates than the other two combined, and it is the one people
consistently underestimate. A capture engine behind SDIO, UART or USB 2 is a
snapshot instrument no matter how good the ADC is.

Two further practical gates, less fundamental but decisive in effort terms:

**Gate 4 — tuning decoupled from the channel plan.** The project's value comes
from treating the LO as "any Hz the synthesizer locks at" rather than a channel
number, which required decoding the synthesizer programming, not just calling a
vendor channel-set routine.

**Gate 5 — a traceable oracle.** The decode worked because MediaTek's own
firmware could be run on the same part and traced register-by-register (69 cold
loads), then the reimplementation diffed against that trace access-for-access on
the host. Without a runnable vendor reference to diff against, calibration
bring-up becomes guesswork. This is why "port to a chip with no Linux driver and
no obtainable firmware" is a much worse proposition than it looks.

## 3. Candidate survey

Verdicts are mine, from public sources; nothing here has been tried on hardware.

| Target | Capture path | Code exec | Egress | Verdict |
| --- | --- | --- | --- | --- |
| **MT7996 siblings** (MT7990/92/93, MT7902/7916/7981/7986) | same ICAP lineage, very likely near-identical | same ROM properties likely | depends on host SoC | **best port target** |
| **MT7921/7922/7925** (M.2/USB client parts) | ICAP presence likely, unverified | mt76 is open; ROM auth unknown | **PCIe to host RAM** — excellent | **highest value/effort ratio** |
| **Broadcom BCM43xx/4366** | `wl sample_collect` is a real raw-ADC path (trigger, pre/post, ADC modes) | **proven** — Nexmon | SDIO/PCIe in phones and routers; weak | plausible, snapshot-biased |
| **Qualcomm ath9k** | spectral scan = FFT **magnitude** bins only (56/128), no phase | open driver, older parts unsigned | PCIe fine | no raw IQ; dead end for SDR |
| **Qualcomm ath10k/11k/12k** | spectral scan; CFR (channel response) landing in ath11k 2025 | signed firmware | PCIe fine | blocked at gate 2 |
| **Intel AX2xx** | nothing public | signed, closed | PCIe fine | blocked at gates 1 and 2 |
| **ESP32 family** | **proven raw IQ**, undocumented wafer-test path | fully open | **the binding constraint** | real, but a different instrument |
| **Nordic nRF52/53/54** | IQ only via AoA/CTE and channel sounding, 1–2 MHz channels | open | plenty for the rate | **not a wideband candidate** |
| **TI CC13xx/26xx** | narrowband, packet-oriented | open | fine | same as Nordic |

### MediaTek siblings — the obvious answer

If the goal is "another chip, same capability", the cheapest path by far is
another part in the same family. The ICAP block, the WFDMA rings, the A-die SPI
scheme and the ROM download protocol are family properties, and `include/hw/`
transfers substantially. Against that, it is also the least interesting answer:
it proves portability within one vendor's lineage, not that the method
generalizes.

The genuinely interesting variant is the **client-side** MediaTek parts
(MT7921/7922/7925, i.e. ordinary laptop M.2 cards, ~$15–25 used). These invert
the W1700K's hardest constraint: instead of a capture ring in router DRAM
exported over 10 GbE, WFDMA lands straight in host RAM over PCIe, where even
Gen2 x1 beats 10 GbE. That removes gate 3 and the entire `platform/` tree —
no U-Boot, no NAND, no custom Linux, no 10 GbE NIC. The unknowns are whether
ICAP exists on those dies with a usable route, and whether their ROM has the
same authentication gap. Both are answerable with a few days of register probing
against a card you can buy, which makes this the best
return-on-investigation in the table.

### Broadcom — the strongest non-MediaTek lead

Not CSI. Nexmon's CSI extractor gives per-frame subcarrier estimates, and
Nexmon's SDR work is **transmit**-only — neither is a receive IQ stream. The
real lead is `wl sample_collect`, visible in mirrored `wlu.c` sources: a command
that collects raw ADC samples with trigger conditions (`now`, `good_fcs`,
`bad_fcs`, `bad_plcp`, `crs`), configurable pre- and post-trigger durations, and
per-PHY ADC modes (HTPHY `0=adc`, `1..3=adc+rssi`; NPHY dual-core or per-core).
That is a vendor raw-capture facility of exactly the ICAP kind, and Nexmon
already clears gate 2 convincingly.

Where it falls down is gates 1-continuous and 3. `sample_collect` is specified as
a triggered snapshot into a modest on-chip buffer, and the parts it exists on sit
behind SDIO (Raspberry Pi, phones) or a PCIe link shared with a router SoC that
has no 10 GbE. Expect a few milliseconds of capture at a time, not a stream.
Still: a Pi 3B+/4B with a BCM43455 is a $40 experiment with a proven code-exec
toolchain, and establishing whether `sample_collect`'s buffer can be re-armed
back-to-back fast enough to approximate a ring is the one question that decides
the whole branch.

### Qualcomm — looks promising, isn't

Worth being explicit because ath9k's spectral scan is widely mistaken for an IQ
path. It reports per-bin **magnitude** (|i|+|q|) for 56 or 128 bins plus a peak
index — phase is discarded in hardware before the driver sees it. You cannot
demodulate, correlate, or recover a waveform from that. ath11k's new CFR capture
is channel-state data, equally not raw samples. Combined with signed firmware on
everything post-ath9k, this family is blocked at both gate 1 and gate 2.

### ESP32 — the real parallel, and a useful reality check

The ESPARGOS team's **ESP-SDR** (`ESPARGOS/esp-sdr`, GPL-3.0-or-later) shows a
whole line of ESP32 parts — ESP32, C2, C3, C5, C6, C61, H2, S2, S3, and the
experimental S31 — bypassing the fixed-function Wi-Fi/BT modem through an
undocumented debug path and capturing raw baseband IQ from the built-in
receiver. The README is candid that the path was found with LLM assistance and
that parts of the firmware are AI-generated: **the same pattern as this project —
an undocumented factory/debug capture path, decoded with machine help — which is
the best public confirmation the method generalizes across vendors.**

It is also the cleanest possible illustration of gate 3. One capture technique
runs across the entire line; capability is set almost entirely by the egress
pipe and the sample width:

| Part | Capture ceiling | Off-chip reality | Result |
| --- | --- | --- | --- |
| Most (ESP32, C2/C3/C6/C61, S2) | 80 MS/s internal | ASCII `CAP16`/`CAP20` snapshots over USB/UART | spectrum analyzer |
| S3 | 80 MS/s internal | continuous, but decimated on the second core to ≤250 kSa/s | narrowband monitor |
| S31 (experimental) | 80 MS/s internal | 20 MSa/s USB / **40 MSa/s Ethernet**, 8-bit I/Q | a real, if modest, receiver |
| — W1700K, for scale — | 320 MS/s | **1.28 GB/s** over 10 GbE, 14-bit | the thing being matched |

Best-case ESP (S31 Ethernet) is 40 MSa/s × 8 bits = **80 MB/s**; the W1700K is
1.28 GB/s at 14 bits. One capture idea, ~16× the throughput and nearly double the
depth between them, and the entire gap is the pipe. Tuning on the ESP line is a
software attempt range of 100–6000 MHz (no guaranteed lock), with 5 GHz verified
on the C5 and an experimental divider mode reaching ~1.84 GHz on several parts;
gain and power are uncalibrated.

Two facts bear directly on the rest of this document:

- **Capture is RX-only, by firmware choice, not silicon.** The README notes a TX
  path exists in the hardware but "this firmware currently implements reception
  only." So ESP32 TX is in the same state as everyone else's (§ TX below): the
  path is there, nobody has driven it.
- **The core split is the REA-lane split.** The RISC-V parts (C2/C3/C5/C6/C61,
  H2) go straight into REA's RISC-V ELF lane, and because they are ordinary
  ESP-IDF ELF builds they need no container unwrap — unlike the MediaTek
  firmware, which does. The Xtensa parts (original ESP32, S2, S3) have **no REA
  lane at all**, which is awkward because the S3 is the best continuous-streaming
  part and the one a decompiler can't touch.

#### What ESP-SDR changes for this repository

It is still the **cheapest end-to-end portability test** — ~$8, no firmware
authentication to defeat, a known-good capture path — but two integration
directions are now visible, and the second is probably the better investment:

1. **ESP → Contract A (a shim).** ESP-SDR's continuous IQS frames already carry
   sample indices, gap flags and a CRC32 — structurally the same information as
   Contract A's `seq`, event/lost tags and integrity checks. A small adapter that
   repackages IQS frames as `SDR1` datagrams would let `w1700k-stream` ingest an
   ESP source unchanged, proving the host stack is source-agnostic in about a
   weekend.
2. **W1700K → SoapySDR (the ecosystem seam).** ESP-SDR ships a `SoapyESPSDR`
   driver, i.e. the host side is already solved through SoapySDR, the lingua
   franca that GQRX, SDR++ and GNU Radio all consume. A reciprocal `SoapyW1700K`
   module over Contract B would plug *this* project into that whole ecosystem and
   reframe the portability story from "port other radios into our wire format" to
   "our receiver is one SoapySDR source among many." Higher leverage than the
   inbound shim, and it makes every future source (ESP, Broadcom, a MediaTek
   client part) interoperable for free.

### Nordic — the honest no

nRF52/53/54 are narrowband 2.4 GHz packet radios. IQ does exist, but only through
the direction-finding (AoA/AoD CTE) and channel-sounding paths: 1–2 MHz channels,
packet-triggered, sized for angle estimation and ranging. There is no wideband
ADC and no capture block to liberate, so no firmware work creates one. Nordic
parts make good 2.4 GHz **RSSI sweep** scanners (the `radio_test` sample sweeps
channels) and good protocol tools; they cannot be made into wideband receivers.
Same reasoning disposes of TI's CC13xx/CC26xx.

## 4. Recommended order of attack

1. **Parameterize the stream service** (1–2 days, no hardware). Hoist the six
   couplings above into a source descriptor; add a synthetic test source that
   emits Contract A from a file. This makes everything below falsifiable and is
   worth doing regardless of which radio wins.
2. **ESP32-S31 → the host stack** (a weekend, ~$8). Proves the host stack is
   source-agnostic end to end and yields a working receiver (up to 40 MSa/s over
   the S31's Ethernet, 8-bit). Do it via SoapySDR rather than the Contract A
   shim if the `SoapyW1700K` module from § ESP-SDR gets built — the ecosystem
   seam is worth more than a one-off adapter.
3. **MT7921/7922 register reconnaissance** (days, ~$20). Probe for an ICAP block
   and a route; check ROM download authentication. This is the one branch that
   could reach W1700K-class performance with *less* system complexity than the
   W1700K itself.
4. **BCM43455 `sample_collect` feasibility** (days, ~$40, Nexmon in hand). One
   question: can the snapshot buffer be re-armed continuously?
5. Only then consider a full port, and only to whichever of 3 or 4 survived.

Note what is *not* on this list: porting to a chip whose vendor firmware you
cannot run and trace. Gate 5 is not a formality — the trace-and-diff oracle is
what made the MT7996 calibration decode tractable at all, and without it the
bring-up is open-ended.

## Sources

Public sources consulted for the survey, October 2026:

- [ath9k spectral scan](https://wireless.docs.kernel.org/en/latest/en/users/drivers/ath9k/spectral_scan.html) and [FFT_eval](https://github.com/simonwunderlich/FFT_eval) — FFT magnitude only
- [nexmon_csi](https://github.com/seemoo-lab/nexmon_csi) and [mobisys2018 Nexmon SDR](https://github.com/seemoo-lab/mobisys2018_nexmon_software_defined_radio) — CSI; transmit-only SDR
- [Broadcom `wlu.c` mirror](https://devel.lulzbot.com/mini/Azalea/electronics/olimex/a13/linux-sunxi/modules/wifi/bcm40181/open-src/src/wl/exe/wlu.c) — `sample_collect` help text and ADC modes
- [Reverse-engineering Broadcom wireless chipsets](https://blog.quarkslab.com/reverse-engineering-broadcom-wireless-chipsets.html)
- [ath11k CFR capture series](https://lkml.iu.edu/2511.0/08082.html) — channel response, not raw IQ
- [ESP-SDR coverage](https://www.cnx-software.com/2026/10/04/esp-sdr-firmware-turns-esp32-into-a-2-4-5-ghz-software-defined-radio-sdr/) and [rtl-sdr.com](https://www.rtl-sdr.com/various-projects-independently-find-hidden/) — ESP32 raw IQ via `librftest` `adctrig`
- [Nordic radio_test](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/v1.7-branch/nrf/samples/peripheral/radio_test/README.html) and DevZone threads on [IQ via channel sounding](https://devzone.nordicsemi.com/f/nordic-q-a/124792/when-running-the-channel-sounding-example-on-the-nrf54l15-how-can-one-obtain-the-iq-values-of-the-ranging-signal/550826)
- [openwrt/mt76](https://github.com/openwrt/mt76) — no ICAP exposure upstream
