# Schematic & Wiring — i8088 Slow-Clock Signal Generator

Generates a 1–100 Hz fixed-pulse clock for single-stepping an Intel **80C88
(CMOS)** CPU. See `schematic.svg` for the drawn version.

> ⚠️ **Use the 80C88 (CMOS), not the NMOS 8088.** The NMOS part has a 2 MHz
> minimum clock and cannot run/step this slowly. ⚠️ **The CLK line MUST be level
> shifted to 5 V** — the 80C88 needs ~4.2 V logic-high; the Pico's 3.3 V GPIO is too
> low to drive CLK directly. Here a single **inverting N-MOSFET (Q1)** does it; the
> firmware pre-inverts (`CLK_INVERTED=1`) so the CPU's CLK is high during the pulse.
> (Datasheet basis in `CLAUDE.md`.)

---

## Bill of materials

| Ref     | Part                                   | Notes                                  |
|---------|----------------------------------------|----------------------------------------|
| U1      | Raspberry Pi Pico (RP2040 / Pico 1)    | the generator                          |
| U2      | 1602 LCD + PCF8574 I²C backpack         | addr 0x27 (or 0x3F for PCF8574A)        |
| Q1      | N-MOSFET (NCE6050; or 2N7000/BSS138)    | inverting 3.3 V → 5 V level shift for CLK |
| Rd      | 4.7 kΩ (1 kΩ for a logic-level FET)     | Q1 drain pull-up to +5 V                |
| Rg      | 100 kΩ                                  | Q1 gate pull-down (defines gate at boot)|
| RV1     | 10 kΩ linear potentiometer              | frequency control (powered from 3V3)    |
| SW1     | momentary push-button                   | MODE (AUTO ⇄ MANUAL)                    |
| SW2     | momentary push-button                   | STEP (one clock pulse in MANUAL)        |
| —       | Intel **80C88** target CPU              | the device being clocked               |
| C1      | 100 nF                                  | decoupling on the 80C88 / 5 V rail     |

Power: feed the rig from a single **5 V** supply. The Pico's **VBUS (pin 40)** is
5 V when USB-powered; use that for the 5 V rail, and the Pico's **3V3 (pin 36)** for
3.3 V. Tie **all grounds together** (Pico, Q1 source, LCD, pot, 80C88).

> **Q1 = NCE6050 note:** it's a power FET, not logic-level (Vgs(th) up to ~4 V), so
> 3.3 V only weakly turns it on. With Rd = 4.7 k–10 k it still pulls a clean low
> (it sinks <1 mA). Edges become RC-soft (hundreds of ns) — fine at 1–100 Hz.
> Verify: GP15 high → drain < ~0.4 V. A true logic-level FET (2N7000/BSS138) lets
> you use Rd = 1 k for sharper edges.

---

## Connection table

### Clock output (the important one) — inverting MOSFET level shifter

| From            | To                          | Notes                                       |
|-----------------|-----------------------------|---------------------------------------------|
| GP15 (pin 20)   | Q1 **gate**                 | 3.3 V logic in                              |
| Q1 gate         | Rg (100 kΩ) → GND           | holds gate defined while GP15 is hi-Z       |
| Q1 **drain**    | Rd (4.7 kΩ) → +5 V          | pull-up; drain swings 0–5 V                 |
| Q1 **drain**    | **80C88 CLK (pin 19)**      | inverted 0–5 V; firmware un-inverts the pulse |
| Q1 **source**   | GND                         |                                             |

Because the stage inverts, the firmware drives GP15 inverted (`CLK_INVERTED=1`), so
the 80C88's CLK goes **high for the fixed pulse width** (100 µs) each cycle; only the
low time varies with the frequency knob.

### Potentiometer (frequency) — powered from 3V3, no divider

The potentiometer is itself a voltage divider, so wiring it across 3V3→GND keeps
the wiper within the 3.3 V ADC range with no extra parts and no loading error.

| From            | To                          | Notes                                   |
|-----------------|-----------------------------|-----------------------------------------|
| 3V3 (pin 36)    | RV1 top terminal            | full-scale = ~3.3 V                     |
| GND             | RV1 bottom terminal         |                                         |
| RV1 wiper       | GP26 / ADC0 (pin 31)        | 0–3.3 V → mapped to 1–100 Hz            |

> If you must power the pot from **5 V** instead, add a divider on the wiper
> (e.g. R1 = 4.7 kΩ in series, R2 = 8.2 kΩ to GND → 0–~3.18 V) so the ADC input
> never exceeds 3.3 V. Not needed with the 3V3 wiring above.

### Buttons (active-low, internal pull-ups — no external resistors)

| From (Pico)     | To        | Function                                  |
|-----------------|-----------|-------------------------------------------|
| GP14 (pin 19)   | SW1 → GND | MODE: toggle AUTO ⇄ MANUAL                |
| GP13 (pin 17)   | SW2 → GND | STEP: emit one clock cycle (MANUAL only)  |

### LCD (PCF8574 I²C backpack)

| From (Pico)     | To            | Notes                                  |
|-----------------|---------------|----------------------------------------|
| GP4  (pin 6)    | PCF8574 SDA   | I²C0 data                              |
| GP5  (pin 7)    | PCF8574 SCL   | I²C0 clock                             |
| VBUS 5 V (pin 40)| LCD/PCF8574 VCC | 1602 contrast/backlight want 5 V    |
| GND             | PCF8574 GND   |                                        |

The backpack has its own SDA/SCL pull-ups; the firmware also enables the RP2040
internal pull-ups. If your backpack pulls to 5 V and you want to be strict about
levels, add a 2-channel I²C level shifter — most boards work fine as-is because
the RP2040 I²C pins tolerate it and the PCF8574 reads 3.3 V as a valid high.

### 80C88 support (single-stepping basics — not exhaustive)

| 80C88 pin       | Tie to        | Why                                       |
|-----------------|---------------|-------------------------------------------|
| CLK (19)        | Q1 drain      | the slow clock (inverted by Q1)           |
| Vcc (40)        | 5 V           |                                           |
| GND (1, 20)     | GND           |                                           |
| READY (22)      | 5 V (HIGH)    | no wait states                            |
| RESET (21)      | RC + Schmitt / 82C84A | power-on reset, held ≥4 clocks    |
| MN/MX (33)      | per your bus design | min/max mode select                 |

---

## ASCII overview

```
                              +5V ──[Rd 4.7k]──┐
                                               │
   3V3 ─┐ RV1 10k pot                          ├──► 80C88 CLK (pin 19)
        │                                      │
       [ ]── wiper ──► GP26/ADC0      GP15 ──►┤ D
        │                              gate    │ Q1  (NCE6050, N-MOSFET)
   GND ─┘                               │      │ S
                                  Rg 100k│      └──► GND
                                        GND   (drain inverts; firmware un-inverts)

   GP4 ─── SDA ┐                 GP14 ──[SW1 MODE]── GND
   GP5 ─── SCL ┤ PCF8574 ─ 1602  GP13 ──[SW2 STEP]── GND
   5V  ─── VCC ┘
   GND ─── GND
```

The 80C88 also needs RESET (RC/Schmitt or 82C84A) and READY = HIGH; data/address
latches (8282) and bus inspection LEDs are up to your target board.
