# i8088 Slow-Clock Signal Generator — Project Guide

RP2040 (Raspberry Pi Pico 1) firmware that generates a **1–100 Hz clock** for
single-stepping an Intel **80C88** CPU, with a 1602 LCD (PCF8574 I²C backpack)
showing frequency / period / pulse-time, a potentiometer for frequency, and a
push-button to switch into manual single-step mode.

> **The CPU MUST be an 80C88 (CMOS), not the NMOS 8088.** See "8088 clock facts"
> below — the original NMOS part physically cannot run this slow.

---

## What it does

- **AUTO mode** (default): the potentiometer sets the clock frequency, linearly
  mapped **1 Hz → 100 Hz**. The clock runs continuously.
- **MANUAL mode**: pressing the **STEP** button emits exactly **one** clock cycle.
  The pot still shapes that single pulse (its high/low widths). A step counter is
  shown on the LCD. This is the 80C88 single-stepping workflow.
- The **MODE** button toggles AUTO ⇄ MANUAL.
- The clock is generated with a **33 % duty cycle** (high = ⅓ of the period) — the
  duty the 8088/8086 family requires (see facts below).
- LCD continuously displays **frequency (Hz)**, **period (ms)** and **pulse/high
  time (ms)**.

---

## Architecture (dual-core)

| Core   | Responsibility                                                        |
|--------|----------------------------------------------------------------------|
| Core 1 | **Clock engine** — tight, dedicated loop driving the CLK GPIO with `busy_wait_us`. Reads shared high/low widths + mode. Nothing else runs here, so the waveform stays jitter-free regardless of LCD/ADC work. |
| Core 0 | **UI** — ADC (pot) read + smoothing, button debounce, frequency math, LCD refresh. Writes the shared timing under a `critical_section`. |

Shared state (`s_high_us`, `s_low_us`, `s_manual`, `s_step`, `s_step_count`) is
guarded by a `critical_section_t` for the high/low pair; single-word flags are
plain `volatile`.

---

## 8088 clock facts (baked into the firmware — do not "fix" these)

Sourced from the Intel 8088 (231456) and Harris/Intersil 80C88 (FN2949)
datasheets. If you change the clock code, keep these invariants:

1. **Use the 80C88 (CMOS), never the NMOS 8088.** The NMOS 8088 AC table gives
   `TCLCL` max = **500 ns**, i.e. a **2 MHz minimum** clock — it is a *dynamic*
   part whose internal state leaks if clocked slower, so it **cannot be
   single-stepped**. The 80C88 is **fully static** (datasheet "Static Operation":
   *"eliminates the minimum frequency restriction… DC to 5 MHz"*), so it can be
   clocked at 1 Hz or stopped indefinitely. **This is the whole reason the project
   works.**
2. **33 % duty cycle, not 50 %.** The CLK pin description states it is *"asymmetric
   with a 33 % duty cycle"*. Firmware: `high_us = period/3`, `low_us = period − high_us`.
3. **CLK logic-high is unusually high.** `VCH` (NMOS 8088) = **3.9 V min**; `VIHC`
   (80C88) = **Vcc − 0.8 ≈ 4.2 V** at Vcc = 5 V. **A 3.3 V RP2040 GPIO does NOT
   reach this** (short by 0.6–0.9 V). → the CLK output is level-shifted to 5 V by a
   **74HCT244/74HCT125** buffer (HCT inputs accept 3.3 V as logic-high; outputs
   swing to ~5 V). **Never wire GP15 straight to the 80C88 CLK pin.**
4. **Edges**: `TCH1CH2`/`TCL2CL1` ≤ 10 ns — trivially met; the 74HCT buffer gives
   sharp edges. Don't RC-filter the clock line.
5. **RESET / READY for stepping**: hold **READY = HIGH** (no wait states) and
   reset the CPU once via a Schmitt-trigger RC (or an 8284A/82C84A). Because the
   80C88 is static, ALE/RD/WR/address/data stay valid between manual edges so you
   can latch and inspect the bus — the point of single-stepping.

---

## Pin map (RP2040 / Pico 1)

| Signal            | GPIO  | Notes                                                        |
|-------------------|-------|-------------------------------------------------------------|
| **CLK out**       | GP15  | → 74HCT244/125 input → 80C88 CLK (5 V). **Buffer required.** |
| **MODE button**   | GP14  | to GND, internal pull-up, active-low                        |
| **STEP button**   | GP13  | to GND, internal pull-up, active-low                        |
| **Pot wiper**     | GP26  | ADC0. Via voltage divider if pot is on 5 V (see schematic)  |
| LCD SDA           | GP4   | I²C0, to PCF8574 SDA                                         |
| LCD SCL           | GP5   | I²C0, to PCF8574 SCL                                         |
| Status LED        | GP25  | onboard; on = MANUAL mode                                    |

PCF8574 address default **0x27** (`LCD_ADDR`); use **0x3F** for PCF8574**A**
backpacks. The 1602 + PCF8574 run at 5 V (VBUS); SDA/SCL are open-drain and pulled
to 5 V — RP2040 GPIO is 5 V-tolerant on the I²C lines via the backpack's own
pull-ups, but if unsure use 3.3 V pull-ups / a level shifter.

---

## Build & flash (mirrors the magner150 project)

Self-contained: the SDK 2.2.0, ARM-GCC 14.2 and picotool are **reused from the
sibling `magner150_vga_rp2350/toolchain/`** via absolute paths in `CMakeLists.txt`
and `Makefile` (override `REF_TOOLCHAIN` / `PICOTOOL` to relocate). Nothing is
downloaded. `cmake`/`ninja` come from Homebrew.

```bash
make            # build → build/clkgen.uf2 (+ bin/hex/elf)
make flash      # rebuild + flash over the USB cable (picotool load -fx -u)
make flash-bootsel   # if -u reset fails: hold BOOTSEL + plug USB, then run this
make reboot     # reboot the running chip
make clean
```

`make flash` uses `picotool load -fx -u` — the `-u` resets the running chip into
BOOTSEL over USB, so **no BOOTSEL button press is needed** (USB stdio is enabled
for exactly this). Identical flash mechanism to the reference VGA project, minus
the secure-boot/OTP provisioning (this project is unsigned/unencrypted).

---

## Files

| File                 | Purpose                                            |
|----------------------|----------------------------------------------------|
| `main.c`             | Dual-core clock engine + UI (ADC, buttons, LCD)    |
| `lcd1602_i2c.{c,h}`  | HD44780 1602 driver over PCF8574 I²C, 4-bit mode   |
| `CMakeLists.txt`     | RP2040 (`PICO_BOARD=pico`) build, vendored toolchain|
| `Makefile`           | One-command build/flash (picotool over USB)        |
| `SCHEMATIC.md`       | Wiring tables + ASCII schematic                    |
| `schematic.svg`      | Drawn schematic                                    |
| `pico_sdk_import.cmake` | SDK locator (copied from reference)             |

---

## Conventions

- No magic numbers — all pins, thresholds and timing constants are `#define`d at
  the top of `main.c`.
- Descriptive iteration/variable names (no single-letter loop vars).
- Comments describe **current** behaviour only (no ticket/lineage narration).
- Keep core 1 doing *nothing but* clocking — don't add I²C/printf there or the
  waveform jitters.
