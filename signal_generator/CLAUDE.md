# i8088 Slow-Clock Signal Generator — Project Guide

RP2040 (Raspberry Pi Pico 1) firmware that generates a **1–100 Hz clock with a
fixed-width high pulse** for single-stepping an Intel **80C88** CPU, with a 1602 LCD
(PCF8574 I²C backpack) showing frequency / period / pulse-width, a potentiometer for
frequency, and a push-button to switch into manual single-step mode.

> **The CPU MUST be an 80C88 (CMOS), not the NMOS 8088.** See "8088 clock facts"
> below — the original NMOS part physically cannot run this slow.

---

## What it does

- **AUTO mode** (default): the potentiometer sets the clock frequency, linearly
  mapped **1 Hz → 100 Hz**. The clock runs continuously.
- **MANUAL mode**: pressing the **STEP** button emits exactly **one** fixed-width
  pulse; the cycle count advances by one. This is the 80C88 single-stepping workflow.
- The **MODE** button toggles AUTO ⇄ MANUAL.
- The CLK **high pulse is a fixed width** (`PULSE_WIDTH_US`, 100 µs); only the **low**
  time varies, so turning the pot changes only the frequency, not the pulse. This is
  valid because the static 80C88 has no input duty-cycle rule — each phase just has
  to clear its minimum (see facts below).
- LCD displays **frequency (Hz)**, **period (T, seconds)** and the **cycle count**
  (`c=`, clock pulses since the last reset — zeroed when RESET releases). The fixed
  100 µs pulse isn't shown (it's constant).

---

## Architecture (dual-core)

| Core   | Responsibility                                                        |
|--------|----------------------------------------------------------------------|
| Core 1 | **Clock engine** — drives the CLK GPIO. Fixed-width high pulse via `busy_wait_us`; the long low phase polls the shared timing/mode in `LOW_POLL_US` chunks so pot/mode changes apply within a few ms (not after a whole period). |
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
2. **No input duty-cycle rule — only minimum phase times.** The CLK input spec is
   `TCHCL` (high) ≥ **69 ns** and `TCLCH` (low) ≥ **118 ns**; the "33 % duty" is just
   what the 8284 generator produces at rated speed, not a requirement on the CPU's
   CLK pin. Since the 80C88 is static (no max period), the firmware holds the **high
   pulse fixed** (`PULSE_WIDTH_US`, 100 µs ≫ 69 ns) and varies **only the low time**.
   So the pulse width never changes — only the frequency does.
3. **CLK logic-high is unusually high.** `VCH` (NMOS 8088) = **3.9 V min**; `VIHC`
   (80C88) = **Vcc − 0.8 ≈ 4.2 V** at Vcc = 5 V. **A 3.3 V RP2040 GPIO does NOT
   reach this** (short by 0.6–0.9 V). → the CLK output is level-shifted to 5 V by an
   **inverting N-MOSFET (Q1)** stage (gate ← GP15, drain → CLK with a pull-up to
   +5 V, source → GND). A 74HCT244/125 buffer is the non-inverting alternative.
   **Never wire GP15 straight to the 80C88 CLK pin.**
   - Because the MOSFET stage **inverts**, the firmware pre-inverts via
     `CLK_INVERTED 1` (`clk_drive()`), so the CPU's CLK is high during the pulse. Set
     `CLK_INVERTED 0` for a non-inverting buffer.
4. **Edges**: `TCH1CH2`/`TCL2CL1` ≤ 10 ns is the datasheet spec; a resistor-pull-up
   MOSFET stage gives RC-soft edges (hundreds of ns) — irrelevant at 1–100 Hz, and
   the 100 µs pulse is far wider than the edge, but use a 74HCT buffer if you ever
   need fast edges. Don't add extra RC filtering.
5. **RESET (active HIGH) is driven by the Pico** (GP12). The 80C88 RESET must be
   **HIGH for > 4 clock cycles** and is **synchronised to the clock**, so the firmware
   counts clock *pulses* (`RESET_HOLD_CYCLES = 8`), not time — it asserts RESET at
   power-up and releases it once 8 cycles have been clocked out. The high→low edge is
   ≥ 50 µs after power-up (boot + several slow cycles ≫ 50 µs). After release the CPU
   runs ~7 internal cycles, then fetches from FFFF0H. A **RESET button** (GP11)
   re-asserts. In MANUAL you literally clock the reset out by stepping. Hold
   **READY = HIGH** (no wait states). Because the 80C88 is static, ALE/RD/WR/address/
   data stay valid between manual edges so you can latch and inspect the bus.

---

## Pin map (RP2040 / Pico 1)

| Signal            | GPIO  | Notes                                                        |
|-------------------|-------|-------------------------------------------------------------|
| **CLK out**       | GP15  | → Q1 gate (inverting N-MOSFET) → 80C88 CLK (5 V). **Level shifter required.** |
| **MODE button**   | GP14  | to GND, internal pull-up, active-low                        |
| **STEP button**   | GP13  | to GND, internal pull-up, active-low                        |
| **RESET out**     | GP12  | → 80C88 RESET (active HIGH, pin 21). Internal pull-up (ext. 10 k optional) |
| **RESET button**  | GP11  | to GND, internal pull-up, active-low; re-asserts RESET      |
| **Pot wiper**     | GP26  | ADC0. Via voltage divider if pot is on 5 V (see schematic)  |
| LCD SDA           | GP4   | I²C0, to PCF8574 SDA                                         |
| LCD SCL           | GP5   | I²C0, to PCF8574 SCL                                         |
| Status LED        | GP25  | onboard built-in LED; solid on = firmware running           |

PCF8574 address default **0x27** (`LCD_ADDR`); use **0x3F** for PCF8574**A**
backpacks. The 1602 + PCF8574 run at 5 V (VBUS); SDA/SCL are open-drain and pulled
to 5 V — RP2040 GPIO is 5 V-tolerant on the I²C lines via the backpack's own
pull-ups, but if unsure use 3.3 V pull-ups / a level shifter.

**Pin idle states are set in firmware with RP2040 internal pulls** (safe-state
polarity): pull-down on the CLK gate (GP15) and LED (GP25), pull-up on RESET (GP12)
and all buttons (GP11/13/14) and I²C (GP4/5). **GP26 (ADC) is intentionally left with
no pull** — a pull would skew the analog reading; `adc_gpio_init()` disables them.
So external `Rg`/`Rrst`/button/I²C pull resistors are optional (add them only to
define levels during the ROM-boot window before firmware runs).

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
