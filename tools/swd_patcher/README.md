# ESP32 → HT32F52243 SWD one-shot patcher

Standalone Arduino/PlatformIO sketch that bit-bangs the SWD protocol from a
plain ESP32 (no J-Link required) to permanently enable mode-1 / 9600 baud
BM-frame output on a salvaged **Qingping PM sensor add-on module**
(Holtek HT32F52243, firmware V1.0.03.230201).

This is a *one-time* operation. After the patch the module always boots
emitting standard PMS-style BM frames on PA4 at 9600-N-1, and the SWD wires
are no longer needed.

## Wiring

| Module pogo | HT32 pin   | ESP32 GPIO (default) |
|------------:|------------|----------------------|
| 5V          | VDD5V      | 5V (USB / VIN)       |
| GND         | GND        | GND                  |
| SWCLK       | PA12       | **GPIO 25**          |
| SWDIO       | PA13       | **GPIO 26**          |
| PA4 (TX)    | PA4        | *not used for patch* |
| PA5 (RX)    | PA5        | *not used for patch* |

A common ground between the ESP32 and the module is mandatory. The HT32's
internal LDO regulates the 5V rail to 3.3V for the IO pads, so the SWD
lines swing at 3.3V — directly compatible with ESP32 GPIOs, no level shifter.

## Build & flash the ESP32

```bash
cd tools/swd_patcher
pio run -t upload
pio device monitor
```

## Run the patcher

1. Power the module from the same supply you give the ESP32 (5V on the pogo
   5V pad; GND tied to ESP32 GND).
2. Open the serial monitor at 115200.
3. Press the ESP32 reset button.
4. The sketch will:
   - bring up SWD, halt the M0+ core
   - read DPIDR and verify it (`0x0BC11477` is the standard ARM M0+ value)
   - read the existing flash bytes at the two patch sites and confirm they
     match the stock firmware
   - erase + reprogram page 11 (`0x2C00–0x2FFF`) with the 4-byte pointer
     redirect (`B4000020` → `38000020` at `0x2E08`)
   - erase + reprogram page 49 (`0xC400–0xC7FF`) with `01 FF FF FF` at
     `0xC400`
   - read both pages back and verify
   - issue a system reset and release the CPU

A successful run prints `PATCH OK — module is now in mode 1`. From then on
the module sends a 32-byte BM frame on PA4 (USART1_TX) at 9600-N-1
approximately once per second; wire that to any ESPHome `uart:` parser.

## Pin overrides

If you want different GPIOs, edit the two `#define`s at the top of
`src/main.cpp`. Any pin that is push-pull capable, not strapped on boot,
and not used by flash/PSRAM will work. GPIO 25/26 were chosen because
they are free on the standard WROOM-32 DevKit-C.

## Safety / reversibility

The patcher only ever touches two flash pages. To revert, run the same
sketch with `PATCH_REVERT = true` near the top of `src/main.cpp` — it will
restore the original bytes (`B4000020` and `0xFF`).

## What is actually patched and why

See [`../../docs/TECHNICAL.md`](../../docs/TECHNICAL.md) in the repo root
for the full reverse-engineering writeup. Short version:

- The module ships with a persistent mode byte at flash `0xC400` set to
  `0xFF`. On boot, the BSS-zero RAM mode byte stays `0` (= "wait for SWD
  host", original use case is the Qingping monitor body reading SRAM via
  SWD).
- In mode 0 the USART1 peripheral is never initialised; PA4/PA5 are
  configured as bit-banged GPIO+EXTI for a trivial 2-command LED protocol.
- Writing `0x01` to flash `0xC400` makes the boot path take the mode-1
  branch, which calls `FUN_00003918` to set PA4/PA5 to AF6 (USART1) at
  9600 baud and run the full `:`/`BM` command parser, *including*
  continuous emission of 32-byte BM frames whenever new sensor data is
  ready.
- A second 4-byte patch at code address `0x2E08` redirects an indirect
  store so that the firmware actually checks the flash mode byte on every
  boot (stock firmware has a stale pointer that writes the mode byte to a
  dead RAM location instead of `*pcVar3`).
