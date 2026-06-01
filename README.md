# Qingping PM Sensor → ESPHome

Reuse a [Qingping particulate-matter sensor add-on module](docs/TECHNICAL.md#1-hardware-overview)
(Holtek HT32F52243, firmware `V1.0.03.230201`) as a standalone PM
histogram sensor in [ESPHome](https://esphome.io/), driven by any
ESP32 over a single GPIO.

The repository ships:

- **`components/qingping_pm/`** — drop-in ESPHome [external component](https://esphome.io/components/external_components.html)
  that decodes the module's 32-byte `BM`-framed output as **11 raw
  histogram bins + detector baseline + diagnostic counters**.
- **`tools/swd_patcher/`** — a single-shot ESP32 PlatformIO sketch
  that bit-bangs SWD to apply three small firmware patches (no J-Link
  required) so the module actually emits frames on power-up. Fully
  reversible.
- **`tools/patch_firmware.py`** — same patches applied offline to the
  flash dump, useful if you have a real SWD programmer.
- **`firmware/V1.0.03.230201/`** — the original 64 KiB flash dump,
  ELF, SRAM dump, and the pre-patched image for reference / re-flash.
- **`docs/TECHNICAL.md`** — full reverse-engineering writeup (frame
  layout, thresholds, patch derivation, command set).
- **`examples/`** — runnable ESPHome YAMLs.

> **Why patch the firmware at all?** The Qingping monitor body
> normally retrieves measurements from this module by acting as an
> **SWD master** on PA12/PA13 and reading the histogram straight out
> of the HT32's SRAM. The module *also* implements a full UART
> protocol on PA4/PA5 — the entire BM-frame transmitter, command
> parser, and response builder are present in the stock firmware —
> but mode selection at boot defaults to an internal bitbang UART
> mode that never finishes its TX path, so PA4 sits idle-high
> forever. Implementing an SWD master on an ESP32 just to grab the
> SRAM data would be substantially harder than consuming the UART
> protocol that's already there, so this project flips three flash
> bytes to enable the hardware USART1 path instead: (1) re-purpose
> an existing config byte as a persistent mode selector, (2) disable
> a baked-in 7-second boxcar filter so you get raw 1-second windows,
> and (3) set the mode byte to "USART1 enabled". Full derivation in
> [`docs/TECHNICAL.md`](docs/TECHNICAL.md#11-the-three-swd-patches).

---

## Hardware required

- A Qingping PM sensor add-on module (HT32F52243-based).
- An **ESP32** (any classic ESP32 with two free GPIOs works; the
  patcher is written for a generic ESP32 DevKit-C / WROOM-32).
- 5 V power for the module — the ESP32's USB / VIN rail is fine.
- A way to make temporary contact with the module's 6-pin pogo
  connector (jig, alligator clips, or solder five wires). Once the
  firmware patches are applied the SWD pads are no longer needed.

You do **not** need a J-Link or any other SWD programmer — the ESP32
patcher does everything.

The pogo pinout (barcode side on the left):

```
1: SWCLK   → runtime data path used by the original monitor body;
              also used by our SWD patcher (ESP32 GPIO 25, default)
2: SWDIO   → same; ESP32 GPIO 26 during patching
3: PA4 TX  → ESPHome host RX   (e.g. ESP32 GPIO 27)
4: PA5 RX  → ESPHome host TX   (optional, for sending commands)
5: GND     → common GND
6: 5V      → 5V
```

Once the firmware patches are applied the SWD pads can be left
floating — all measurements are then delivered over PA4 (UART TX)
and the patched module no longer expects an SWD master to be polling
it.

Full pinout, electrical notes and the BM frame format are in
[`docs/TECHNICAL.md`](docs/TECHNICAL.md).

---

## Step 1 — patch the module's firmware (one time)

Skip this step if you have already patched your module.

1. Wire the module to an ESP32 per the patcher README:

   | Module pogo | ESP32 GPIO (default) |
   |:------------|:---------------------|
   | 5V          | 5V (USB / VIN)       |
   | GND         | GND                  |
   | SWCLK (pin 1) | **GPIO 25**        |
   | SWDIO (pin 2) | **GPIO 26**        |

   PA4 / PA5 are not used during patching.

2. Build and flash the patcher onto the ESP32:

   ```bash
   git clone https://github.com/h0m3us3r/qingping-pm
   cd qingping-pm/tools/swd_patcher
   pio run -t upload
   pio device monitor
   ```

3. Power-cycle the module while the monitor is running. You should
   see the patcher detect SWD, read the original bytes, write the
   three patches, and read back to verify.

4. Disconnect the SWD wires. **Keep the 5 V / GND / PA4 (and
   optionally PA5)** wires — they go to the host ESP32 in step 2.

Full details (alternative pin assignments, revert procedure, how the
FMC programming works) are in
[`tools/swd_patcher/README.md`](tools/swd_patcher/README.md).

> **Offline alternative.** If you'd rather use a J-Link, run
> `python3 tools/patch_firmware.py` to produce
> `firmware/V1.0.03.230201/ht32_flash_64k.patched.bin` and flash that
> image with your tool of choice.

After patching, the module powers up emitting `42 4D 00 1C ...` BM
frames on PA4 at 9600 8N1, ~1 Hz.

---

## Step 2 — wire the host ESP32

Any ESP32 with a free UART works. The minimum wiring is:

| Module pogo | Host ESP32     |
|:------------|:---------------|
| 5V (pin 6)  | 5V             |
| GND (pin 5) | GND            |
| PA4 (pin 3) | **UART RX** (e.g. GPIO 27) |
| PA5 (pin 4) | UART TX (optional, only needed to send commands) |

A common GND is mandatory. The module's outputs swing at 3.3 V — no
level shifter required.

---

## Step 3 — install the ESPHome component

There are three equivalent ways to make ESPHome see the
`qingping_pm` component.

### Option A — pull straight from GitHub (recommended)

Add this to your ESPHome YAML:

```yaml
external_components:
  - source: github://h0m3us3r/qingping-pm
    components: [qingping_pm]
```

ESPHome will clone the repo and pick up `components/qingping_pm/`
automatically — nothing else to install.

To pin to a particular commit or tag:

```yaml
external_components:
  - source: github://h0m3us3r/qingping-pm@main      # branch
  # or: github://h0m3us3r/qingping-pm@v1.0.0        # tag
  # or: github://h0m3us3r/qingping-pm@<commit-sha>
    components: [qingping_pm]
```

### Option B — local clone

```bash
git clone https://github.com/h0m3us3r/qingping-pm
cd qingping-pm
esphome run examples/basic.yaml
```

`examples/basic.yaml` already uses the GitHub source by default; flip
the commented block to use the local clone instead.

### Option C — ESPHome dashboard / Home Assistant add-on

Copy the `components/qingping_pm/` folder so it sits next to your
device YAML, then add:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [qingping_pm]
```

---

## Step 4 — minimal YAML

```yaml
uart:
  id: pm_uart
  rx_pin: GPIO27        # ← connect to pogo pin 3 (PA4)
  baud_rate: 9600
  rx_buffer_size: 256

external_components:
  - source: github://h0m3us3r/qingping-pm
    components: [qingping_pm]

qingping_pm:
  uart_id: pm_uart
  bin_0:  { name: "PM bin 0 (3100+)" }
  bin_1:  { name: "PM bin 1 (2430+)" }
  bin_2:  { name: "PM bin 2 (2030+)" }
  bin_3:  { name: "PM bin 3 (1630+)" }
  bin_4:  { name: "PM bin 4 (1330+)" }
  bin_5:  { name: "PM bin 5 (1030+)" }
  bin_6:  { name: "PM bin 6 (810+)"  }
  bin_7:  { name: "PM bin 7 (650+)"  }
  bin_8:  { name: "PM bin 8 (450+)"  }
  bin_9:  { name: "PM bin 9 (100+)"  }
  bin_10: { name: "PM bin 10 (50+)"  }
  baseline:        { name: "PM detector baseline" }
  frames_received: { name: "PM frames received" }
  bad_checksums:   { name: "PM bad-checksum count" }
```

All bin entries are optional — omit any you don't care about. The
suffixes in the names refer to the per-bin amplitude threshold (a
peak above `baseline + N` is counted in that bin); see
[`docs/TECHNICAL.md` §7](docs/TECHNICAL.md#7-histogram-bin-thresholds).

A more complete example with shared filter pipelines, a derived
`particle_rate` (counts/s) template sensor, and Wi-Fi/API/OTA scaffolding
lives in [`examples/basic.yaml`](examples/basic.yaml).
A diagnostic-only YAML that exposes the raw UART stream is in
[`examples/debug.yaml`](examples/debug.yaml).

---

## Configuration reference

The `qingping_pm` component takes:

| Key                | Type      | Default | Notes |
|--------------------|-----------|---------|-------|
| `uart_id`          | id (UART) | —       | Required. The UART receiving PA4. |
| `bin_0` … `bin_10` | sensor    | —       | Optional. One ESPHome sensor per histogram bin. |
| `baseline`         | sensor    | —       | Optional. Low byte of the detector's rolling baseline. |
| `frames_received`  | sensor    | —       | Optional diagnostic counter (`total_increasing`). |
| `bad_checksums`    | sensor    | —       | Optional diagnostic counter (`total_increasing`). |

Each `bin_N` is a standard ESPHome `sensor` and accepts the usual
keys (`name`, `id`, `filters`, `icon`, etc.).

### Understanding the bins

- **Bins are mutually exclusive.** Each detected particle peak is
  counted in exactly one bin — the highest one whose threshold it
  exceeds. `bin_i` covers the half-open amplitude band
  `(T_{i+1}, T_i]`.
- **`bin_0` carries the largest particles**, `bin_10` the smallest.
- **In clean air `bin_10` dominates.** A practical "total dust"
  proxy is `sum(bin_0 … bin_10)`; its time derivative is a clean
  particle-rate signal (this is what `examples/basic.yaml` exposes
  as `Particle rate`).
- Reported counts are raw 1-second-window counts thanks to the
  un-smoothing patch.

The component does **not** compute µg/m³ — that conversion is not
present in the module firmware either. If you need it, derive it
yourself in YAML using a lambda.

---

## Reverting

To go back to a stock module: re-flash
`firmware/V1.0.03.230201/ht32_flash_64k.bin` with any SWD programmer,
or run the SWD patcher with `PATCH_REVERT = true` in
`tools/swd_patcher/src/main.cpp`.

---

## Documentation & credits

- **[`docs/TECHNICAL.md`](docs/TECHNICAL.md)** — the full
  reverse-engineering writeup: frame format, threshold derivation,
  histogram pipeline, command set, flash map, and patch internals.
- **[`tools/swd_patcher/README.md`](tools/swd_patcher/README.md)** —
  patcher pin map, build instructions, FMC details.
- **[`firmware/V1.0.03.230201/`](firmware/V1.0.03.230201/)** —
  original flash + SRAM dumps and pre-patched image (md5
  `73b304332c4bc9b9817f764ca616a10b`).
- **[`docs/datasheets/`](docs/datasheets/)** — Holtek HT32F52243/53
  datasheet (vendor PDF, for reference).

Reverse engineering done with Ghidra against the J-Link-dumped
firmware. The module, datasheet, and trademarks belong to their
respective owners; this project is an independent interoperability
effort under the [MIT license](LICENSE).
