# i8088 Slow-Clock Signal Generator (RP2040 / Pico 1)

A Raspberry Pi Pico that produces a **1–100 Hz, fixed-pulse clock** for
single-stepping an Intel **80C88** CPU, with a 1602 LCD readout, a potentiometer
for frequency, and a button to drop into manual single-step.

## Quick start

```bash
make            # build → build/clkgen.uf2
make flash      # rebuild + flash over the USB cable (no BOOTSEL button)
```

`make flash` uses `picotool load -fx -u`; if the USB reset fails, hold BOOTSEL +
plug USB and run `make flash-bootsel`.

## Use

- **AUTO** (default): turn the pot — clock sweeps 1 → 100 Hz. The **high pulse is a
  fixed width** (100 µs); only the low time (frequency) changes.
- **MODE button** (GP14): toggle AUTO ⇄ MANUAL.
- **MANUAL**: each **STEP button** (GP13) press emits exactly one fixed-width pulse;
  the cycle count ticks up by one each press.
- **Cycle count** `c=` is the number of clock pulses since the last reset (zeroed when
  RESET releases), so it tracks the CPU's clocks from the reset vector.
- Onboard LED is solid on while the firmware is running.

LCD layout (16×2):

```
AUTO f= 47.00Hz      MAN  f= 47.00Hz
c=     128 T0.021    c=      42 T0.021
```

`c=` = clock cycles since reset, `T` = period in **seconds**. (The high pulse is a
fixed 100 µs — constant, so it isn't shown.)

## Hardware

See **`SCHEMATIC.md`** / **`schematic.svg`**. Critical points:

- **Target must be the 80C88 (CMOS)** — the NMOS 8088 has a 2 MHz minimum clock
  and physically cannot be slow-clocked/stepped.
- **CLK (GP15) goes through a 74HCT125/244 buffer to 5 V** — the 80C88 needs
  ~4.2 V logic-high; the Pico's 3.3 V output is too low to drive CLK directly.
- Pot reads via a 4.7 k/8.2 k divider from 5 V (or power the pot from 3V3 and skip
  the divider).

Datasheet basis for all of the above is in **`CLAUDE.md`**.

## Build setup

SDK 2.2.0, ARM-GCC 14.2 and picotool are reused from the sibling
`magner150_vga_rp2350/toolchain/` via absolute paths in `CMakeLists.txt` /
`Makefile` (override `REF_TOOLCHAIN`). `PICO_BOARD=pico` (RP2040).
