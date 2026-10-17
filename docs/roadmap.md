# Roadmap

## Where it stands

The receiver works end to end with only our firmware on the MT7996: up to
320 Msps of 14-bit IQ from any of the three bands, packed on the radio to fit
one 10 GbE link, received completely by the host, shown live in the browser.
Mid-stream controls tune the LO, set AGC or manual gain, set the analog filter
and switch bands. A load brings a power-cycled radio up in about 2 s; streams
start and stop without a reload. No MediaTek code is loaded, and nothing
transmits.

## How the firmware became ours

1. **Our code on the MT7996.** The ROM does not authenticate the patch's hook
   table; a hook that makes the RAM-image CRC check pass lets our WM and WA
   load. A third hook counts the DSP as started, so no DSP image is needed.
2. **Discovery.** A minimal WM of ours could replay register state captured
   from MediaTek's initialisation, and a radio warmed by MediaTek's firmware
   received; a power-cycled one received once the ROM patch's radio init and
   the A-die writes of MediaTek's load were replayed too. That showed what
   matters, and was not the product.
3. **Trace and decode.** MediaTek's firmware was traced on a power-cycled unit,
   every D-die register access (the WM core's own debug triggers) and every
   A-die SPI access (the ROM's SPI slots) in one ordered log with the code
   address of each, one calibration item at a time over 69 cold loads. Every
   procedure of the load was then regenerated record for record from
   MediaTek's code and tables, taking only read values and the EEPROM as
   inputs: the ROM patch's radio init, the common and per-band PHY init, the
   A-die RF init, every calibration item, the channel sets, and the capture
   set-up of the SPECTRUM command. Their registers and fields are in
   `include/hw/`.
4. **Reduce.** Measured on the bench against MediaTek's firmware: reception
   needs the RF init, the R-cal and RC calibrations and the synthesizer; the
   DC-offset calibration lowers the DC spur; the TTG IQ calibration's filters
   sit after the capture tap and do not touch the stream; every TX calibration,
   PHY table A, the 320 MHz background receiver and the channel set's MAC words
   can go. The MAC's receive path must be enabled (its transmit path stays
   off).
5. **Write it.** Each procedure was written in C for our WM from the headers,
   and checked access for access against the trace on the host before it ran
   on hardware. On a power-cycled unit the result leaves the radio as
   MediaTek's load does, register for register apart from measured
   calibration codes, and receives as well on every band.
6. **Commands.** Streams, band set-up, rate and RF control (tune, gain,
   filter, trim, band switch) are commands to our firmware, one per parameter,
   with no channel numbers; the host writes no radio
   register, and the vendor path (MediaTek's firmware, the replay, the
   SPECTRUM prime, the REG_ACCESS edit, the vendor download) is gone.

## Next

- **Unit-side control daemon.** The control plane is small (load, configure,
  start, stop, RF commands). A daemon on the unit can serve it on a socket, so
  the host no longer needs ssh.
- **A dedicated command ring.** Commands use words in the firmware's mailbox
  page, relayed by the stream pump while it runs. A WFDMA command/event ring
  pair of its own (or the MT7996's second PCIe host interface) would give
  events and queueing.
- **Sample rate mid-stream.** A new sample rate is a RATE between streams; it
  could run inside a stream as a resumable program.
- **The tables as formulas.** SETUP still applies MediaTek's per-frequency-group
  and per-bandwidth-class tables (and a TUNE does not reload the group tables);
  each should become a computed function of the LO and the sample rate.
- **Frequency coverage.** Measured LO coverage is 2.081–2.748 GHz and
  4.647–7.576 GHz (unit A; above 7.41 GHz by the lock flag only). Below and between needs external conversion, or capture of the
  2.4 GHz monitor receiver (chain 4 of band 0), whose own PLL is known but whose
  capture path is not.
- **Calibrated levels.** dBFS to dBm per band and gain setting.
- **More of the radio understood.** Many A-die registers of the RF init tables
  have no known function yet; they carry offset names and MediaTek's values.
- **Long runs.** A 24-hour stream with the export validated.

## Release checklist

- [x] Only our firmware on the MT7996, bringing the radio up from cold by
      itself, controlled only through commands.
- [x] Our firmware reaches registers only by their header names.
- [ ] Every known register named in `include/hw/` (offset names stand in where
      the function is unknown).
- [x] A clean clone builds (`make`) and passes `make check`; its firmware is
      byte-identical to the development tree's.
- [ ] The same on a fresh machine (only the development machine has built it).
- [x] Install and return to stock run on a second unit (a W1700K2).
- [ ] Update and recovery (`init/flash`) run on a second unit.
- [ ] A W25N04LV unit (some W1700K2s) read raw and installed: the part, its
      spare layout and its vendor bad-block reserve are supported from three
      community driver tables and the W1700K's stock U-Boot, none read on one.
