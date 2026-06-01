# Qingping PM Sensor Module — Technical Reference

Reverse-engineered protocol and firmware notes for the Qingping particulate
matter sensor add-on module (Holtek HT32F52243, firmware
`V1.0.03.230201 A6001-016 M R`, image `ht32_flash_64k.bin`, md5
`73b304332c4bc9b9817f764ca616a10b`).

This document is the *why* behind the [ESPHome component](../components/qingping_pm/)
and the [SWD patcher](../tools/swd_patcher/). The README in the repository
root covers installation and usage.

## Contents

1. [Hardware overview](#1-hardware-overview)
2. [Pogo connector pinout](#2-pogo-connector-pinout)
3. [Power & wiring](#3-power--wiring)
4. [MCU & memory map](#4-mcu--memory-map)
5. [Two UART modes (stock vs. patched)](#5-two-uart-modes-stock-vs-patched)
6. [BM output frame format](#6-bm-output-frame-format)
7. [Histogram bin thresholds](#7-histogram-bin-thresholds)
8. [Histogram pipeline (boxcar vs. un-smoothed)](#8-histogram-pipeline-boxcar-vs-un-smoothed)
9. [Host command protocol](#9-host-command-protocol)
10. [Flash memory layout](#10-flash-memory-layout)
11. [The three SWD patches](#11-the-three-swd-patches)
12. [Logic-analyzer verification](#12-logic-analyzer-verification)

---

## 1. Hardware overview

The Qingping PM module is a self-contained laser-scatter particulate
sensor designed to plug into a Qingping monitor body via a 6-pin pogo
connector. It contains a laser-detector front end, a fan, and an HT32F52243
Cortex-M0+ that runs the entire histogram pipeline.

The module has **no temperature or humidity sensor**; those readings on
the parent monitor come from a separate chip on the main board.

In normal use the monitor body collects measurements by acting as an
**SWD master** against the module's PA12/PA13 pins and reading the
histogram and baseline directly out of the module's SRAM. The module
also implements a full UART command/response protocol on PA4/PA5, but
the stock firmware never enables it (see [§5](#5-two-uart-modes-stock-vs-patched)).

Reusing the module as a standalone PM sensor requires (a) understanding
the BM frame format the firmware *would* emit if its UART were enabled
and (b) patching three flash bytes to actually enable that UART path,
because consuming the existing serial protocol from an ESP32 is far
simpler than implementing an SWD master in software.

## 2. Pogo connector pinout

Six pogo pads on the sensor module edge (barcode side on the left, hook
side on the right):

```
barcode | 1  2  3  4  5  6 | hook
          |  |  |  |  |  |
          |  |  |  |  |  +-- 5V supply (from monitor body)
          |  |  |  |  +----- GND
          |  |  |  +-------- PA5  (RX — sensor receives host commands)
          |  |  +----------- PA4  (TX — sensor transmits BM frames)
          |  +-------------- PA13 (SWDIO — runtime SWD readout by monitor body)
          +----------------- PA12 (SWCLK — runtime SWD readout by monitor body)
```

| Pin | Signal | Direction (sensor POV) | Notes |
|-----|--------|------------------------|-------|
| 1   | SWCLK  | input                  | SWD clock, MCU PA12 |
| 2   | SWDIO  | bidirectional          | SWD data, MCU PA13 |
| 3   | PA4    | **output** (TX)        | Sensor → host, 9600 8N1 |
| 4   | PA5    | **input** (RX)         | Host → sensor commands |
| 5   | GND    | —                      | |
| 6   | 5V     | input (power)          | Powers the module |

PA4/PA5 are routed through series resistors on the PCB for current
limiting / ESD protection.

**SWD is the production data path, not a debug port.** The original
Qingping monitor body acts as the SWD *master*: it clocks the
HT32's SWD interface from its own MCU and reads the histogram and
baseline straight out of the module's SRAM at runtime. PA4/PA5 carry
a complete BM/ASCII command-and-frame protocol in firmware — but
the stock V1.0.03.230201 image boots into an unused mode that never
brings that UART path up (see [§5](#5-two-uart-modes-stock-vs-patched)),
so a stock module is silent on PA4 even though the protocol is fully
implemented. The body never needs the UART path because it already
has direct SRAM access via SWD.

For an ESPHome / ESP32 reuse, implementing an SWD master in software
is substantially harder than just adding a UART RX pin, so this
project patches three bytes of flash to enable the existing UART
path instead (see [§11](#11-the-three-swd-patches)).

## 3. Power & wiring

- 5V rail powers the module. The HT32's internal LDO regulates it down
  to 3.3 V for the IO pads.
- SWD lines and UART lines both swing at **3.3 V**, directly compatible
  with ESP32 GPIOs — no level shifter required.
- For ESPHome use, only **PA4**, **GND** and **5V** need to be connected
  to the host (PA4 → host RX). PA5 is optional and only needed if you
  want to send commands.

## 4. MCU & memory map

- **MCU**: Holtek HT32F52243 (Cortex-M0+).
- **Flash**: 64 KiB readable (0x0000–0xFFFF), page size 1 KiB. The
  datasheet advertises larger parts but only 64 KiB responds on this
  module.
- **SRAM**: 8 KiB readable (0x20000000–0x20001FFF).
- **Vector tables**: two are present.
  - `0x00000000`: bootloader / IAP updater. Initial SP `0x20000450`,
    reset vector `0x000000C1`.
  - `0x00001000`: application. Initial SP `0x20000CA8`, reset vector
    `0x000010C1`.
- **Bootloader range**: `0x00000000–0x00000D5B`.
- **Application range**: `0x00001000–0x00004C07`.
- **Calibration / config region**: starts at `0x0000C000` (see [§10](#10-flash-memory-layout)).
- **Flash Memory Controller** (FMC) is at `0x40080000`. Register
  offsets used by the SWD patcher: `TADR`=0, `WRDR`=4, `OCMR`=0xC
  (command 4=word-program, 8=page-erase), `OPCR`=0x10/0x14. Success
  is signalled by bits [3:2] of OPCR reading `11`.

## 5. Two UART modes (stock vs. patched)

The firmware contains two completely separate UART implementations,
selected at boot by the byte at SRAM `0x20000038`:

### Mode 1 — hardware USART1 (used after patching)

| Parameter         | Value |
|-------------------|-------|
| Peripheral        | USART1 (base `0x40040000`) |
| PA4 alternate fn  | AF6 = USART1_TX |
| PA5 alternate fn  | AF6 = USART1_RX |
| Baud              | **9600** |
| Frame             | 8N1, LSB first |

### Mode 0 — bitbang software UART (stock default)

| Parameter         | Value |
|-------------------|-------|
| PA4 alternate fn  | AF0 = GPIO output (TX) |
| PA5 alternate fn  | AF0 = GPIO input (RX) |
| Baud              | 9600 (DMA-assisted timing) |
| Receive method    | EXTI edge interrupts on PA4/PA5 |
| Timer channel     | DMA channel 6 |

Both modes share the same wire-level framing (9600 8N1, idle high).
The mode is selected by the byte at SRAM `0x20000038` — but **in the
stock V1.0.03.230201 image, nothing writes to this byte**. BSS-zero
is the only value it ever holds, so a stock unit always boots in mode 0,
and mode 0 never finishes bringing up its TX side, so PA4 stays
idle-high forever and no frames are emitted. This is by design: the
monitor body retrieves measurements over SWD ([§2](#2-pogo-connector-pinout))
and has no need for the UART output.

[The patches in §11](#11-the-three-swd-patches) re-purpose an existing
flash-loaded config byte to drive the mode selector instead, so that
`flash[0xC400] = 0x01` makes the module always boot in mode 1 (hardware
USART1 on PA4/PA5).

## 6. BM output frame format

The patched module transmits a 32-byte `BM`-framed packet on PA4 every
second. The outer framing (`'B' 'M'`, 16-bit length `0x001C`, 16-bit
additive checksum at offset `0x1E..0x1F`) is identical in both modes,
but the 28-byte payload has **two possible interpretations** depending
on which patches are applied.

**Checksum** (both modes): unsigned 16-bit additive sum of the 30
bytes from offset `0x00` to `0x1D`, stored big-endian at
`0x1E..0x1F`. The receiver must verify it; invalid frames are silently
dropped by the sensor's own command parser too.

### 6a. Plantower PMSX003-compatible frame (default, patches 1+3 only)

With only patches #1 and #3 applied, `FUN_00004154` reaches the
`state[0x12] != 0` branch on every emit (state[0x12] is hardcoded to 1
at boot and never cleared), runs the on-module µg/m³ pipeline, and
calls **`FUN_00001c5c`** with `param_1 = 0` to build a frame that
matches the standard Plantower PMSX003 wire format byte-for-byte:

```
Offset  Len  Field                                Source
------  ---  -----------------------------------  ----------------------------
0x00    2    Magic                'B' 'M'         literal
0x02    2    Length               0x00 0x1C       literal
0x04    2    PM1.0  CF=1 (µg/m³)  u16 BE          low 16 bits of state[0x48]
0x06    2    PM2.5  CF=1 (µg/m³)  u16 BE          low 16 bits of state[0x44]
0x08    2    PM10   CF=1 (µg/m³)  u16 BE          low 16 bits of state[0x4c]
0x0A    2    PM1.0  atm  (µg/m³)  u16 BE          same as 0x04 (state[0x48])
0x0C    2    PM2.5  atm  (µg/m³)  u16 BE          same as 0x06 (state[0x44])
0x0E    2    PM10   atm  (µg/m³)  u16 BE          same as 0x08 (state[0x4c])
0x10    2    "≥0.3 µm / 0.1 L"    u16 BE          state[0x50] = mem[0x20000026]*5
0x12    4    Device ID            32-bit LE       state[0x58] (flash 0xC000)
0x16    2    Temperature scaled   u16 BE          float(state[0x60]) * 50 * 100
                                                  then >>8 — see notes
0x18    2    Humidity scaled      u16 BE          float(state[0x64]) * 100
0x1A    2    Baseline             u16 BE          state[0x34] (FIR average)
0x1C    1    Constant 0x80 ... overwritten        low byte of state[0x34]
0x1D    1    Constant 0x00                        literal
0x1E    2    Checksum             u16 BE additive sum 0x00..0x1D
```

Notes on the Plantower-style fields:

- **The CF=1 block (0x04..0x09) and the atm block (0x0A..0x0F) are
  bit-for-bit identical** — the firmware writes the same three values
  twice. There is no separate atmospheric correction.
- The three µg/m³ values are derived from only **three** of the eleven
  amplitude bins: bin[10] (peak > baseline+100, smallest particles),
  bin[8] (peak > baseline+650), and the folded `bin[5] += bin[2..4]`
  (largest particles). Bins 1, 3, 4, 6, 7, 9, 11 do not contribute to
  any transmitted field in this mode.
- The "≥0.3 µm count per 0.1 L" field at offset `0x10` is computed as
  `(raw_peak_counter >> 1) × 30 × 5 = raw × 15` of internal counts, then
  rounded to a u16. **Every unit step in the internal counter changes
  the transmitted value by exactly 150.** This is the source of the
  "quantised in steps of 150" behaviour the field exhibits.
- The temperature and humidity slots (`0x16` and `0x18`) read floats
  out of `state[0x60]` and `state[0x64]`. The module has **no
  temperature or humidity sensor of its own** — these slots stay at
  whatever was last written by the host (via the calibration / `:s`
  command path), normally zero on a re-used module.
- The "device ID" at `0x12..0x15` is the 32-bit value programmed into
  flash `0xC000` (`:ids` / BM `0x26`).

In short: **the default-patched module emits a Plantower-shaped frame
whose PM2.5/PM10 fields are real (calibrated) µg/m³ values and whose
remaining fields are mostly redundant, scaled, or dead.** Treat
PM2.5/PM10 (atm or CF=1, they are identical) as the only useful
quantities; ignore everything else.

### 6b. Raw 11-bin histogram frame (patches 1+2+3+4)

With **Patch 4 ("histogram mode")** also applied, the µg/m³ pipeline
is skipped and the BM-emit sub-block at flash `0x48F2` builds a frame
that carries the raw amplitude histogram instead:

```
Offset  Len  Field
------  ---  --------------------------------------------------
0x00    2    Magic                'B' 'M' (0x42 0x4D)
0x02    2    Length (big-endian)  0x001C = 28 bytes following
0x04    2    bin[0]               STALE — never written, ignore
0x06   22    bin[1..11]           11 × u16 big-endian histogram counts
0x1C    1    Detector baseline    low byte of running baseline
0x1D    1    Reserved             always 0x00
0x1E    2    Checksum             u16 big-endian sum of bytes 0x00..0x1D
```

In this mode the eleven bins are the raw `running_sum[1..11]` slots.
With Patch 2 also applied, each is the count of peaks classified into
that amplitude band during the most recent ~1-second window (no
boxcar). Without Patch 2 they are 7-second boxcar sums.

`bin[0]` is an artefact of an off-by-one in the firmware's per-bin
buffer (12 slots, only 1..11 are populated by the cascade). Always
discard it.

## 7. Histogram bin thresholds *(histogram-mode frame only)*

When Patch 4 is active and the module emits the 32-byte raw histogram
frame (§6b), the eleven bins are filled by the classifier in
`FUN_00004154`. They are **mutually exclusive**: an `if / else if`
chain over decreasing thresholds counts each detected peak in
exactly one bin — the highest one whose threshold it exceeds.
`bin[i]` therefore covers a half-open amplitude band
`(T_{i+1}, T_i]`, **not** "all peaks ≥ T_i".

Peaks below `baseline + 50` are dropped entirely.

| Bin   | Description           | Threshold (verified from flash) |
|-------|-----------------------|---------------------------------|
| 1     | LARGEST particles     | peak > **3100** (absolute)      |
| 2     |                       | peak > baseline + 2430          |
| 3     |                       | peak > baseline + 2030          |
| 4     |                       | peak > baseline + 1630          |
| 5     |                       | peak > baseline + 1330          |
| 6     |                       | peak > baseline + 1030          |
| 7     |                       | peak > baseline + 810           |
| 8     |                       | peak > baseline + 650           |
| 9     |                       | peak > baseline + 450           |
| 10    |                       | peak > baseline + 100           |
| 11    | SMALLEST particles    | peak > baseline + 50            |

Because the chain is highest-to-lowest, **bin[11] always carries the
bulk of the count in clean air**. A practical "total dust" proxy is
simply `sum(bin[1..11])`; differentiating that sum versus time gives
a particle-rate sensor (this is what the example YAML does).

## 8. Histogram pipeline (boxcar vs. un-smoothed)

```
current[12 u16]      SRAM 0x20000324   bins accumulated this 1-second window
history[7][12 u16]   SRAM 0x20000354   ring buffer of the last 7 windows
running_sum[12 u16]  SRAM 0x2000033C   what gets serialised into the BM frame
d  = state[0x0d]                       ring index, advances 0..6 every frame
```

Per emit:

1. Per-sample analogue-front-end pulses populate a 0x80-sample FIR
   window (5-tap centre-weighted, weights `1·2·2·2·1`, divided by 8).
2. A rising/falling state machine over the smoothed window emits one
   "peak height" value whenever the slope reverses.
3. Each peak is classified through the descending if/else-if cascade
   above and increments **exactly one** bin in `current[]`.
4. Every 500 windows (~1 s) the current buffer is rotated into the
   7-deep history, then `running_sum[bin] = Σ history[i][bin]` for
   `i = 0..6` is rebuilt and serialised as the frame payload.

**Stock firmware** therefore transmits a 7-second boxcar low-pass
over `running_sum[]` — which the host cannot undo, because the
individual 1-second windows are not transmitted. This affects
**both** emit paths: in the default Plantower-style frame (§6a) the
µg/m³ pipeline reads `running_sum[]`, and in the histogram-mode
frame (§6b) it is serialised directly.

**Patch #2 below** turns the inner sum loop into a single-iteration
pass-through so the serialised bins are the raw count from the most
recent ~1-second window. This is what makes the bin values
meaningfully responsive to events like a candle being lit nearby.

The byte at frame offset `0x1C` is the low byte of the rolling
baseline (`state[0x34]`, a 128-sample average of the FIR window
right-shifted by 7), useful for diagnostics. (It is overwritten by
the same value in both emit paths.)

Conversion to µg/m³ is **not** performed on-module. The host can
either convert in software (the ESPHome example exposes the raw bins
plus a derivative) or read the calibration constants at flash
`0xC000` / `0xD000` if those are non-default.

## 9. Host command protocol

PA5 (host → sensor) accepts two formats. The parser only runs in
mode 1, i.e. after the SWD patches are applied.

### ASCII (5 bytes, `:` prefix, no terminator)

| Command    | Hex                  | Action                              | Persists to |
|------------|----------------------|-------------------------------------|-------------|
| `:m1`      | `3A 6D 31 xx xx`     | Enable PM data output               | —           |
| `:m0`      | `3A 6D 30 xx xx`     | Disable PM data output              | —           |
| `:pNNN`    | `3A 70 d d d`        | Set PM threshold (000..999)         | `0xC800`    |
| `:NNNp`    | `3A d d d 70`        | Alternate threshold set             | `0xC800`    |
| `:sNNN`    | `3A 73 d d d`        | Set sensitivity (000..999)          | `0xD000`    |
| `:r125`    | `3A 72 31 32 35`     | Enable mode flag (same as `:m1`)    | —           |
| `:ver?`    | `3A 76 65 72 3F`     | Query firmware version              | —           |
| `:bv?`     | `3A 62 76 3F xx`     | Query board / hardware version      | —           |
| `:id?`     | `3A 69 64 3F xx`     | Query 32-bit device ID              | —           |
| `:idsHHHH` | `3A 69 64 73 h h h h`| Set device ID (4 hex nibbles)       | `0xC000`    |
| `:para`    | `3A 70 61 72 61`     | Read back threshold + sensitivity   | —           |
| `:axxxx`   | `3A 61 xx xx xx`     | Start/restart calibration sequence  | —           |

`:ver?` response is a 21-byte packet with the string
`V1.0.03.230201 A6001-016 M R` embedded at offset 10.

### Binary BM (Plantower-style framing, 7 or 9 bytes)

```
Short (7 bytes):    'B' 'M' cmd d1 d2 ck_hi ck_lo       ck = sum(bytes 0..4)
Extended (9 bytes): 'B' 'M' '&' d1 d2 d3 d4 ck_hi ck_lo ck = sum(bytes 0..6)
```

| `cmd`  | `d1`  | `d2`  | Action                                                     | Persists to |
|--------|-------|-------|------------------------------------------------------------|-------------|
| `0x12` | any   | any   | Query device ID (same as `:id?`)                           | —           |
| `0x20` | `01`  | any   | Disable auto-output flag                                   | `0xC400`    |
| `0x20` | `02`  | any   | Enable auto-output flag                                    | `0xC400`    |
| `0x21` | HI    | LO    | Set PM threshold = `(d1<<8)|d2`                            | `0xD000`    |
| `0x22` | `AA`  | `55`  | **Bootloader jump** — disables IRQs, writes SCB->VTOR, spins | —         |
| `0x23` | `01`  | `01`  | Reset state, restart calibration                           | —           |
| `0x23` | `02`  | `01`  | Query internal state (→ 7-byte BM reply)                   | —           |
| `0x24` | `01`  | `01..04` | Trigger sensor sub-actions (laser, fan, reset, etc.)    | —           |
| `0x25` | HI    | LO    | Set secondary threshold = `(d1<<8)|d2`                     | `0xD400`    |
| `0x26` (`&`) | ID0 | ID1 | + d3=ID2, d4=ID3: set 32-bit device ID big-endian        | `0xC000`    |
| `0xE1` | any   | any   | Query operating mode (→ 8-byte BM reply)                   | —           |
| `0xE2` | any   | any   | Set internal state flag                                    | —           |
| `0xE4` | any   | any   | Hardware state query + reset (→ 8-byte BM reply)           | —           |

8-byte status response format:
```
'B' 'M' 0x00 0x04 cmd_echo state ck_hi ck_lo     ck = sum(bytes 0..5)
```

The `BM 0x22 AA 55` bootloader jump is the over-the-air firmware
update entry point — useful if you bricked it, dangerous otherwise.

## 10. Flash memory layout

```
0x00000000–0x00000D5B   Bootloader / IAP updater
0x00000D5C–0x00000FFF   Gap (0xFF)
0x00001000–0x00004C07   Application firmware
0x00004C08–0x0000BFFF   Blank (0xFF)
0x0000C000              Startup config + 32-bit device ID
                        (read at boot; written by BM 0x26 / :ids)
0x0000C400              In stock firmware: auto-output flag (BM 0x20).
                        After patch #1: mode selector — 0x01 = mode 1.
0x0000C800              PM threshold calibration (`:p` / BM 0x21)
0x0000CC00              Unused / not analysed
0x0000D000              Computed PM average + sensitivity
                        (`:s` / calibration run)
0x0000D400              Secondary threshold (BM 0x25)
```

## 11. The four SWD patches

All four are minimal-intervention halfword/byte edits, fully
reversible by re-flashing the stock image. They are implemented in
both [`tools/swd_patcher/`](../tools/swd_patcher/) (on-target via
ESP32) and [`tools/patch_firmware.py`](../tools/patch_firmware.py)
(offline, produces `ht32_flash_64k.patched.bin`).

### Patch 1 — code-pointer redirect (`flash 0x2E08`)

`FUN_00002d88` reads `flash[0xC400]` at boot and stores the value
through a pointer kept in the literal pool at flash `0x2E08`. That
pointer originally targets `pcVar3[0x7C] = 0x200000B4` (the
auto-output flag, useless in mode 0). Redirecting it to the mode byte
at `0x20000038` re-purposes the existing persistent byte at `0xC400`
as the mode selector.

```
flash 0x2E08:  B4 00 00 20  →  38 00 00 20
```

### Patch 2 — un-smoothing (`flash 0x4570` and `0x45E8`)

Two adjacent halfword edits in flash page `0x4400` turn the 7-window
boxcar in the BM-frame builder into a pass-through:

```
flash 0x4570 (loop iter source):
  ldrh r3,[r3,#0x1e]   DB 8B   →   ldrb r3,[r3,#0x0d]   5B 7B

flash 0x45E8 (loop bound):
  cmp  r3,#0x7         07 2B   →   cmp  r3,#0x1         01 2B
```

After the patch the sum loop runs once starting at `i = d`, so
`running_sum[bin] = history[d][bin]` — the raw count from the most
recent ~1-second window. `state[0x1e]` becomes unused but is still
written, which is harmless.

### Patch 3 — persistent mode byte (`flash 0xC400`)

With patches 1 and 2 in place, programming the config byte makes
every cold boot land in mode 1: USART1 comes up on PA4/PA5 at 9600
8N1 and BM frames stream out on PA4 at ~1 Hz.

```
flash 0xC400:  FF  →  01
```

Without Patch 4 the streamed payload is the Plantower-style µg/m³
frame (§6a).

### Patch 4 — histogram mode (`flash 0x460E`)

The BM-emit routine `FUN_00004154` contains two complete frame
builders selected by a runtime flag at `state[0x12]`:

```
    if (state[0x12] == 0)  build raw histogram   → transmit (§6b layout)
    else                   run µg/m³ pipeline → FUN_00001c5c (§6a layout)
```

`state[0x12]` is forced to `1` at flash `0x4220` during boot and is
never modified afterwards, so stock + patched firmware always takes
the second arm. Inverting one conditional branch flips this:

```
flash 0x460E (state[0x12]==0 gate):
  beq 0x470C        7D D0   →   b 0x470C         7D E0
```

The byte at `0x460F` changes from `0xD0` (cond=EQ in the Thumb `b<c>`
encoding) to `0xE0` (unconditional `b imm11` with the same offset).
The µg/m³ pipeline still runs upstream of this branch, but its
final serialise/transmit step is bypassed. `FUN_00001a4c(buf, 0x20)`
at `0x4930` is reached every emit cycle with the raw 32-byte
histogram payload, giving the §6b layout on the wire.

This patch lives in flash page `0x4400` alongside Patch 2 and is
applied in the same page-rewrite pass.

### Reverting

Re-flash the stock image, or simply set `flash[0xC400] = 0xFF` to
make the patched firmware boot back into the inert mode-0 path.

## 12. Logic-analyzer verification

For bench validation on real hardware:

| Parameter | Value |
|-----------|-------|
| Probe pin | Pogo pin 3 (PA4) for TX, pin 4 (PA5) for RX |
| Decoder   | UART / async serial |
| Baud      | **9600** |
| Frame     | 8 data, no parity, 1 stop |
| Bit order | LSB first |
| Voltage   | 3.3 V logic |

Expected behaviour after power-on with a patched module: both pins
idle high, then ~1 Hz bursts of 32 bytes on PA4 starting with
`42 4D 00 1C`. An unpatched module will leave PA4 idle high forever.

To send a command, drive PA5 with a 9600-baud UART transmitter:

- ASCII: `:m1\x00\x00` to (re-)enable output.
- Binary: full 7- or 9-byte BM frame with valid checksum.
