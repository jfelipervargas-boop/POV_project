# POV_project
Propeller that shows animations and a real-time clock using Persistence Of Vision (POV).

## About
This project was born from looking deeper into microcontrollers, specifically timers and the communication speed between the MCU and external hardware. Pushed to its limit, that turns into a display with its own twist: a spinning column of LEDs that draws an image in the air.

## Repository structure
| Branch | Content |
|---|---|
| `main` | Final product firmware and documentation |
| `tests` | Standalone programs that verify each component |

Layout of the `tests` branch:
```
POV_project/
├── Animacion_Prueba_Arcoiris/   # main.c, crt0.S, linker_script.ld, Makefile
├── Control_NLeds_XColor/        # main.c, crt0.S, linker_script.ld, Makefile
├── Rotacion_Colores_RGB/        # main.c, crt0.S, linker_script.ld, Makefile
├── cmsis_core/                  # CMSIS Core (core_cm4.h)
├── cmsis_device_f4/             # CMSIS Device F4 (stm32f4xx.h, stm32f411xe.h)
└── README.md
```

## Hardware
- MCU: STM32F411xE (Cortex-M4F), running at 84 MHz from the internal HSI + PLL
- LEDs: 2 boards x 8 WS2812/WS2812B LEDs, chained (16 LEDs)
- Motor + driver: We first used an external standalone fan that runs at over 3000 RPM, which was a problem. A motor running at around 1500 RPM works properly; you only need to watch out for the inertia after adding the components.
- Position sensor: HW-870 (infrared sensor), tested separately with an Arduino (see Tests)
- Power: 4.4 V for the LEDs; 3.3 V logic from the MCU (a 3.3 V → 5 V level shifter is recommended)

### Wiring / pinout on test
| Signal | MCU pin | Notes |
|---|---|---|
| LED DIN | PA0 | Push-pull, very high speed. Define `INVERTED_OUTPUT` if the level shifter inverts |
| Status LED | PC13 | On-board LED, used as a diagnostic in `Rotacion_Colores_RGB` |

### Wiring / pinout
| Signal | MCU pin | Notes |
|---|---|---|
| LED DIN | PA9 | Push-pull, very high speed. Define `INVERTED_OUTPUT` if the level shifter inverts |
| Position Sensor | PA10 | Digital input from HW-870 OUT (active low) |

## Tests (`tests` branch)
All STM32 tests share the same startup code (`crt0.S`), linker script and Makefile, and drive the LEDs by bit-banging PA0 with the DWT cycle counter (1.25 µs per bit, T0H 0.36 µs, T1H 0.75 µs, latch ≥ 280 µs).

| Test | Verifies | Expected result |
|---|---|---|
| `Rotacion_Colores_RGB` | SYSCLK = 84 MHz, WS2812 timing, GRB order | PC13 blinks 3 times in 1.2 s (≈6 s means the PLL is not running), then LEDs 1–8 cycle red → green → blue every 0.5 s |
| `Control_NLeds_XColor` | Addressing each of the 16 LEDs across both boards, color and brightness | LEDs selected in `LED_MASK` show `COLOR_HEX`; the rest stay off |
| `Animacion_Prueba_Arcoiris` | Frame buffer and refresh rate | Selected animation (`ANIMATION`): static, blink, chase, breathe or rainbow (default) |
| Position sensor (HW-870) | Rotation detection | Verified separately on an Arduino; no STM32 test yet |

### Editable parameters
- `Control_NLeds_XColor`: `COLOR_HEX` (0xRRGGBB), `LED_MASK` (bit 0 = LED 1 … bit 15 = LED 16), `BRIGHTNESS` (0–255)
- `Animacion_Prueba_Arcoiris`: `ANIMATION`, `COLOR_HEX`, `LED_MASK`, `BRIGHTNESS`, `ANIM_PERIOD_US`, `BLINK_STEPS`, `BREATHE_STEPS`, `RAINBOW_SPREAD`, `LATCH_US`

## Build & flash
Requirements: `arm-none-eabi-gcc`, GNU Make and STM32CubeProgrammer (`STM32_Programmer_CLI`).

Each test is built from its own folder; the Makefile finds CMSIS at `../cmsis_core` and `../cmsis_device_f4`.

```bash
cd Rotacion_Colores_RGB        # or Control_NLeds_XColor / Animacion_Prueba_Arcoiris
make                           # builds main.elf and main.bin
make flash                     # flashes over USB (DFU) at 0x08000000
make clean
```
If `STM32_Programmer_CLI` is not in your PATH:
`make flash STM_PROGRAMMER=/path/to/STM32_Programmer_CLI`

## Usage
Power the LEDs from 3 V, flash a test and check the expected result in the table above. To change the color, mask or animation, edit the parameters at the top of `main.c` and rebuild.

## How it works
A sensor marks each revolution, a timer divides it into angular slices (columns), and the LED column is refreshed in every slice. Because the eye retains each column, the full image appears to float in the air.

## Known issues / roadmap
- 3.3 V data into 5 V-powered LEDs is out of spec (VIH = 3.5 V); add a level shifter or use a diode/resistor/transistor; the use of a resistance on dark colors will become problematic because of a current issue. 
- Motor speed: above ~3000 RPM was a problem; ~1500 RPM works, keeping an eye on the inertia of the mounted components
- [ ] Position sensor (HW-870) test on the STM32
- [ ] Sync the LED refresh with the rotation (POV display)
