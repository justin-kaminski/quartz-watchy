# Watchy v3 hardware facts (R1)

Status: research complete for this pass, 2026-10-05. Author: R1. Scope: facts firmware drivers are written from.
Tags: **VERIFIED** (source named), **CONTRADICTED** (source disagrees with the brief), **UNVERIFIABLE** (no
source reachable), **ASSUMED** (best judgement, say how to confirm). No third-party library or driver source was
used; every fact comes from the schematic/KiCad design files, datasheets or SQFMI/Espressif documentation.

## Contents
1. [Documentation found, and its reliability](#1-documentation-found-and-its-reliability)
2. [Pin-table verification (the brief's table)](#2-pin-table-verification)
3. [Power path, charger, LDO, battery](#3-power-path-charger-ldo-battery)
4. [Battery sense and ADC](#4-battery-sense-and-adc)
5. [USB detect and charge status](#5-usb-detect-and-charge-status)
6. [Buttons, EXT1 wake, reset chord, strapping](#6-buttons-ext1-wake-reset-chord-strapping)
7. [BMA423 wiring](#7-bma423-wiring)
8. [E-paper wiring and supply](#8-e-paper-wiring-and-supply)
9. [Vibration motor](#9-vibration-motor)
10. [Clocks, RTC, flash](#10-clocks-rtc-flash)
11. [USB and console](#11-usb-and-console)
12. [ESP32-S3 facts that matter for these pins](#12-esp32-s3-facts-that-matter-for-these-pins)
13. [Expected sleep-current contributors](#13-expected-sleep-current-contributors)
14. [Other quirks, test points, document discrepancies](#14-other-quirks-test-points-document-discrepancies)
15. [Source tags](#15-source-tags)
16. [Unresolved items](#16-unresolved-items)

## 1. Documentation found, and its reliability
- Official hardware repo: `github.com/sqfmi/watchy-hardware`, tag/branch `v3.0` = commit `a31e74ec56` (2026-01-31);
  `main` is `5f147972aa` (adds only a LICENSE update). License: MIT, (c) 2026 SQFMI. [HWREPO]
- Contents of v3.0: `WatchySchematic.pdf` (1 sheet, A4, KiCad 8.0.4, "Rev: 3.0", PDF made 2024-07-24) [SCH]; KiCad
  sources `Watchy.kicad_sch` / `.kicad_pcb` / `.pretty` (first uploaded 2026-01-31) [KSCH][PCB]; `WatchyBOM.csv` [BOM];
  gerbers, STEP models. There is no separate pinout document; SQFMI's docs page points to the *library* `config.h`
  (not read, per rules) [DOCS-HW].
- Method: the PDF has no connectivity in its text layer, so I extracted the netlist from `Watchy.kicad_sch` with my own
  script (357 of 359 symbol pins resolved to wires/labels/no-connect flags; the 2 unresolved pins were not
  investigated and none of the signals discussed here depends on them), and cross-read every net used here against the rendered PDF. The KiCad file values (C16/C19 26 pF, C17/C18 10 pF) match the PDF.
- The PDF/KiCad design is the best source. **Stale or inconsistent SQFMI material (do not trust):**
  - Interactive BOM linked from the docs is dated 2021-12-26 and is for the ESP32 v1.x board (docs label it "v1"). [DOCS-HW]
  - `WatchyBOM.csv` disagrees with the schematic on passives: 32 kHz caps C17/C18 = 18 pF (BOM) vs 10 pF (schematic);
    40 MHz caps C16/C19 = 22 pF vs 26 pF; antenna parts L1/C7/C9 and R12 = "TBD" vs 7.5 nH/3 pF/1 M. Which is
    populated is UNVERIFIABLE.
  - Docs list LDO `ME6211C33M5G-N`; schematic and BOM say `RT908033` (Richtek RT9080-33). UNVERIFIABLE which is fitted.
  - Docs list the 402030 cell datasheet; that datasheet (GM Battery GMB042030) rates **180 mAh typ / 170 mAh min**,
    not 200 mAh. [BATT]
  - Docs "Battery Life" page shows an `ANY_HIGH` EXT1 sample and an RTC-INT ext0 wake: that is v1/v2 (active-high
    buttons, external RTC). It does **not** apply to v3 (section 6). [DOCS-BAT]
- Crowd Supply / shop pages not fetched: marketing only, and the schematic answers every checklist item.

## 2. Pin-table verification
Resolved from the v3.0 KiCad netlist [KSCH] (nets named as in the schematic) and datasheet pin table [ESP-DS Table 2-1].

| Brief row | Brief GPIO | Schematic net / evidence | Verdict |
|---|---|---|---|
| I2C SDA | 12 | net `SDA` = U4.17 GPIO12 <-> BMA423 SDX(2); R18 10K to 3V3; TP7 | VERIFIED |
| I2C SCL | 11 | net `SCL` = U4.16 GPIO11 <-> BMA423 SCX(12); R20 10K to 3V3; TP6 | VERIFIED |
| MENU button | 7 | net `BTN1` = GPIO7 (SW2). Schematic names buttons BTN1..4 only | GPIO VERIFIED; name MENU ASSUMED (see below) |
| BACK button | 6 | net `BTN2` = GPIO6 (SW1); also U1 SR1 input | GPIO VERIFIED; name VERIFIED by [DOCS-START] ("top two buttons: Back and Up") + reset-IC wiring |
| UP button | 0 | net `BTN3` = GPIO0 (SW3); also U1 SR0 input, TP3 "BOOT" | GPIO VERIFIED; name VERIFIED (same evidence) |
| DOWN button | 8 | net `BTN4` = GPIO8 (SW4) | GPIO VERIFIED; name ASSUMED |
| Accel INT1 | 14 | net `ACC_INT_1` = GPIO14 <-> BMA423 INT1(5) | VERIFIED |
| Accel INT2 | 13 | net `ACC_INT_2` = GPIO13 <-> BMA423 INT2(6) | VERIFIED |
| Battery ADC | 9 | net `ADC` = GPIO9 = ADC1_CH8 (divider R8/R9, C11) | VERIFIED (but see section 4: range problem) |
| USB detect | 21 | net `USB_DET` = GPIO21 (divider R14/R15 from VBUS) | VERIFIED |
| Charge status | 10 | net `STAT` = GPIO10 = ADC1_CH9 (R5/R6 around TP4054 CHRG) | Connectivity VERIFIED; polarity/semantics differ from expectation, section 5 |
| Display CS | 33 | net `CS` = GPIO33 -> J3.12 CS# | VERIFIED |
| Display DC | 34 | net `DC` = GPIO34 -> J3.11 D/C# | VERIFIED |
| Display RESET | 35 | net `RES` = GPIO35 -> J3.10 RES#; R19 100K pull-up to 3V3 | VERIFIED |
| Display BUSY | 36 | net `BUSY` = GPIO36 <- J3.9 BUSY | VERIFIED |
| SPI MOSI | 48 | net `MOSI` = GPIO48 (pin 36 SPICLK_N) -> J3.14 SDA | VERIFIED |
| SPI SCK | 47 | net `SCK` = GPIO47 (pin 37 SPICLK_P) -> J3.13 SCL | VERIFIED |
| SPI MISO | 46 | GPIO46 (pin 52) carries a **no-connect flag**; the panel connector has no MISO line | **CONTRADICTED**: no MISO net exists. GPIO46 is an unconnected strapping pin. Use `miso = -1`; never drive GPIO46 |
| Vibration motor | 17 | net `VIB_PWM` = GPIO17 -> R13 1K -> Q2 base | VERIFIED |

Nets present in the schematic but missing from the brief's table:
- `EN` (CHIP_PU) is driven by U1 (smart-reset IC, SR2HARU), R1 10K pull-up and C2 1 uF (section 6).
- USB: `USB_D-` GPIO19, `USB_D+` GPIO20 via ESD device U5 (section 11).
- 32 kHz crystal: GPIO15/GPIO16 (section 10).
- UART0 TX/RX = GPIO43/GPIO44, reachable only at test pads TP1/TP2.
- Everything else is unconnected (no-connect flags): GPIO1-5, GPIO18, GPIO26-32 (in-package flash), GPIO37-42, GPIO45, GPIO46.
  So there are no spare broken-out GPIOs.
- Names MENU/DOWN: [DOCS-START] only states Back and Up are the top pair. MENU=BTN1/GPIO7 and DOWN=BTN4/GPIO8 follow
  by elimination and match the brief. Physical placement from [PCB] (board 33.8 x 46.0 mm): BTN2/BACK and BTN3/UP at
  y=79.9 mm (top pair), BTN1/MENU and BTN4/DOWN at y=106.3 mm (bottom pair); BTN1+BTN2 on the +x edge (x=100.5),
  BTN3+BTN4 on the -x edge (x=70.7). Left/right in the wearer's view is ASSUMED mirrored (components are on the board's
  back side, display on the other; MENU/BACK on the wearer's left, UP/DOWN on the right). Not needed for drivers.

## 3. Power path, charger, LDO, battery
All from [KSCH]/[SCH]; datasheet notes in brackets.
```
USB VBUS ----+--- D1 (MBR0530 Schottky, anode=VBUS) ---+--- VSYS ---> U2 RT9080-33 LDO (EN tied to VIN) ---> +3V3
             |                                          |
             +-- Q1 gate (P-FET CJ2301); R2 100K to GND |  Q1: drain=+BATT, source=VSYS, gate=VBUS
             +-- U3 TP4054 VCC(4), R5, R14, C12 100nF   |
LiPo J1 pin1 = +BATT ----- Q1 drain ---- U3 BAT(3) ---- R8 (battery divider top)
```
- Power-OR: with VBUS absent, R2 pulls Q1's gate low, so Q1 conducts and VSYS = VBAT. With VBUS present, Q1's gate is
  high (off) and VSYS = VBUS minus the Schottky drop. VERIFIED (topology). The charger takes its own input directly
  from VBUS, so USB powers the watch and charges simultaneously.
- Charger U3 `TP4054` (SOT-23-5): VCC=VBUS, BAT=+BATT, PROG=R3 10K to GND, CHRG(STAT)=pin 1. [TP4054 pin table]
  - Charge current: datasheet table lists 10K for 100 mA. Termination at 1/10 of that (10 mA), float 4.2 V, trickle
    below 2.9 V at 1/10 current, VCC range 4.25-6.5 V, thermal foldback at about 120 C. VERIFIED.
  - Battery-pin leakage with VCC absent: charger enters shutdown and BAT current is 2 uA or less (datasheet statement).
  - Cell data [BATT]: charge 0-45 C only; standard charge 0.5C = 90 mA (spec for the 180 mAh cell), so 100 mA is
    about 0.55C. Discharge cutoff 2.75 V; max continuous discharge 1.5C; max continuous charge 1C. The firmware
    cannot measure cell temperature (no NTC on board).
- LDO U2 (`RT908033` = RT9080-33): VIN 1.2-5.5 V, Iq 2 uA typ / 4 uA max at no load, dropout 0.31 V at 600 mA for
  VOUT >= 3 V, current limit 1.1 A typ. [RT9080 DS pages 1, 6]. EN is tied to VIN (always on).
  Derived (ASSUMED): at Wi-Fi TX peaks (about 0.3-0.4 A) the rail loses regulation below roughly VBAT 3.5-3.6 V;
  the ESP32-S3 needs >= 3.0 V (Table 5-2). Gate radio use on battery voltage (SPEC already says so).
- **No battery protection IC on the board** (no DW01/PCM in netlist; J1 goes straight to +BATT). Over-discharge and
  short protection exists only if the pack has its own PCM: UNVERIFIABLE (the cell spec is for a bare cell).
  Firmware must implement its own low-battery cut-off well above 3.0 V.
- No fuel gauge, no NTC, no ship-mode switch. VBAT is only observable through the divider (section 4).

## 4. Battery sense and ADC
- Circuit: +BATT -> R8 100K -> node `ADC` -> R9 360K -> GND, with C11 100 nF on the node, node to GPIO9. VERIFIED [KSCH].
  Ratio Vpin = Vbat x 360/460 = **0.7826 x Vbat**, i.e. Vbat = Vpin x 1.2778. Source impedance 78 kOhm.
  C11 matches the 100 nF external capacitor that the ESP32-S3 ADC specification assumes [ESP-DS section 5.5].
- GPIO9 = pin 14 = RTC_GPIO9 = **ADC1_CH8** [ESP-DS Table 2-6/2-8]. ADC1 is the unit that
  does not conflict with Wi-Fi (ADC2 does; the IDF page states the driver arbitrates, [IDF-ADC]). GPIO9 is not a
  strapping pin.
- Divider drains Vbat/460k continuously (no gating switch): 7.2 uA at 3.3 V, 8.0 uA at 3.7 V, 9.1 uA at 4.2 V.

| Vbat | 3.0 | 3.3 | 3.5 | 3.7 | 3.8 | 4.0 | 4.2 |
|---|---|---|---|---|---|---|---|
| Vpin (V) | 2.348 | 2.583 | 2.739 | 2.896 | 2.974 | 3.130 | 3.287 |

- **Problem (derived from a VERIFIED datasheet figure):** ESP32-S3 ADC calibration table 5-6 gives the effective range
  for the highest attenuation (ATTEN3, `ADC_ATTEN_DB_12`) as **0-2900 mV, total error +-50 mV** [ESP-DS Table 5-6]. With
  this divider the pin exceeds 2.9 V once Vbat > **3.706 V**, and reaches 3.29 V at 4.2 V. The datasheet states no
  accuracy figure and no saturation point above 2.9 V, so the upper ~0.5 V of the cell range (roughly the top 60% of
  capacity) is outside the characterised range. Pin voltage stays below the 3.6 V absolute maximum [Table 5-1].
  Consequences for the driver (decision recommended): (a) read at ATTEN3 with `esp_adc` calibration, average many
  samples (DNL +-4 LSB, Table 5-5); (b) treat >2.9 V as "high, unspecified accuracy" and characterise on a real unit
  with a bench supply before trusting the top end of the percentage curve; (c) a board-level fix is not possible in
  firmware. The SPEC's "calibrated ADC readings" claim holds only for Vbat <= 3.7 V.
- While USB is present the charger holds +BATT at the charge voltage (up to 4.2 V), so a voltage reading is not a
  state-of-charge estimate then. Use `USB_DET` (section 5) to flag charging and skip SoC updates (ASSUMED sound).

## 5. USB detect and charge status
Both are resistor networks from VBUS, no active parts. VERIFIED topology [KSCH]; state tables DERIVED using the
TP4054 datasheet CHRG behaviour (open-drain, pulled low while charging, high-impedance when charge complete or on
under-voltage lock-out; flashes if no battery) [TP4054 pin function + "State of charge indicator"].
- `USB_DET` (GPIO21): VBUS -> R14 51K -> node -> R15 100K -> GND. Pin = 0.662 x VBUS: 3.31 V at 5.0 V,
  logic-high (>= 2.475 V = 0.75 x 3.3 V, Table 5-4) for VBUS >= 3.74 V. **Active HIGH = USB power present.** Draws
  33 uA only while USB is present. GPIO21 is RTC-capable (can wake EXT1/ext0).
- `STAT` (GPIO10): VBUS -> R5 51K -> node `STAT` (to GPIO10) -> R6 100K -> TP4054 CHRG pin. Note the CHRG pin is at
  the *far* end of R6, not ground-referenced.

| State | CHRG pin | GPIO10 level | Reads |
|---|---|---|---|
| No USB (VBUS = 0) | irrelevant | 0 V | LOW |
| USB, charging | strong pull-down | VBUS x 100/151 = 3.31 V at 5 V | HIGH |
| USB, charge complete | high-Z | pulled to VBUS through 51K; clamped by the pad's ESD diode (about 25 uA injected), exceeds the 3.6 V rating | HIGH |
| USB, no battery | flashes about every 0.5-2 s | alternates 3.3 V / VBUS | HIGH |

  **GPIO10 is high whenever USB power is present and low only when it is absent: it cannot tell "charging" from
  "full".** This duplicates USB_DET and is the opposite of what a "charge status" pin implies. Treat GPIO10 as
  UNRELIABLE for charge state (CONTRADICTS the brief's naming "Charge status"; SQFMI's table just says
  "Charge indicator: GPIO10" [DOCS-HW]). Not measured on hardware: confirm with a multimeter on one board.
  Recommended: derive "charging vs full" in software from USB_DET plus a battery-voltage heuristic (for example
  USB present and Vbat above about 4.15 V sustained = topped off; ASSUMED, tune on hardware). Do not rely on a
  GPIO10 edge. Floating-state note: do not enable an internal pull on GPIO10 (it would distort the divider).
- ADC on GPIO10 (ADC1_CH9) cannot separate charging (3.3 V) from full (clamped) either: both exceed the 2.9 V range.

## 6. Buttons, EXT1 wake, reset chord, strapping
- Circuit: each of BTN1-4: switch between the GPIO net and GND (SW1-4), plus an **external 100 kOhm pull-up to +3V3**
  (R4=BTN1, R7=BTN2, R10=BTN3, R11=BTN4). No capacitors, no series resistors, no ESD parts. VERIFIED [KSCH].
  - Polarity: **active LOW** (idle = 1 via external pull-up, pressed = 0). Pressed current = 3.3 V/100 kOhm = 33 uA.
  - **Correct EXT1 mode: `ESP_EXT1_WAKEUP_ANY_LOW`** with mask GPIO0|GPIO6|GPIO7|GPIO8. ESP32-S3 supports
    ANY_LOW and ANY_HIGH, EXT1 usable on RTC GPIOs 0-21 [IDF-SLEEP]; all four button pins are in 0-21 (RTC_GPIO0/6/7/8,
    [ESP-DS Table 2-6]). `ANY_HIGH` (as in SQFMI's v1/v2 doc sample) would wake immediately and continuously.
  - External pull-ups are always powered, so no internal pull-up is needed in deep sleep. The 45 kOhm internal
    pull-ups/downs (Table 5-4) must not be enabled in a way that fights them; keep RTC pulls off.
  - Software debounce required (no RC filtering). Reads after wake: buttons are level-sensed, so a held button
    re-wakes immediately on the next sleep unless released; handle "wait for release" before re-arming.
- **Smart-reset chord (hardware, not firmware):** U1 `SR2HARU` (STMicroelectronics SR2 family): inputs SR0=BTN3(GPIO0,
  UP) and SR1=BTN2(GPIO6, BACK), output RST -> EN (CHIP_PU). Both buttons held simultaneously for tSRC resets the chip.
  - tSRC: ordering letter H = **4.0 s typ** (+-10% at 25 C, +-20% over temperature) per the sibling SR1 datasheet
    ordering table [SR1-DS Table 6, Table 7]; the SR2 datasheet itself (DocID026047) was not retrievable (ST site
    timed out). Corroborated by SQFMI's procedure "hold Back+Up more than 4 seconds" [DOCS-START] and by the board:
    R1 10K pull-up on EN and R7/R10 pull-ups on the SR inputs, which suits an open-drain, no-pull-up part. Verdict:
    VERIFIED (indirect). Part draws 1-1.5 uA (SR1: 1 uA; SR2 product summary: 1.5 uA).
  - **Firmware constraint:** do not give BACK+UP held together any function lasting >= ~3.5 s, and treat BACK+UP as a
    reserved chord. The reset pulse has no fixed minimum width (suffix R: push-button controlled); RST stays asserted
    until a button is released.
  - Recovery/download path [DOCS-START]: hold BACK+UP >4 s, **release BACK first while still holding UP** -> reset
    releases with GPIO0 low -> ROM download mode (USB Serial/JTAG enumerates). Releasing UP first (or both at once
    with UP released first) gives a plain reset. This matches the brief's "UP-button download mode" recovery path.
- Strapping pins [ESP-DS Table 3-1 to 3-5, section 3]:

| Pin | Board connection | Default at reset | Implication |
|---|---|---|---|
| GPIO0 | BTN3 + 100K pull-up, TP3 | weak pull-up = 1 | 1 = SPI boot; 0 with GPIO46=0 = joint download boot (USB-Serial-JTAG or UART). UP held at reset enters download. Never add large capacitance here |
| GPIO3 | no-connect | floating | Only matters if EFUSE_STRAP_JTAG_SEL is burned; default eFuses ignore it. Leave unused |
| GPIO45 | no-connect | weak pull-down = 0 | VDD_SPI = 3.3 V, correct for ESP32-S3FN8 (flash is 3.3 V, Table 1-1). Do not wire or drive |
| GPIO46 | no-connect | weak pull-down = 0 | Boot-mode partner of GPIO0 (0 satisfies download mode); also ROM-log selection. Not MISO |

  Strapping values are latched at reset and the pins are free as normal IO afterwards (hold time 3 ms after CHIP_PU
  high). EN network R1 10K + C2 1 uF equals Espressif's recommended RC [ESP-HWDG].

## 7. BMA423 wiring
- Power: VDD and VDDIO on the always-on +3V3 rail (datasheet: VDD 1.62-3.6 V, VDDIO 1.2-3.6 V, power-up 1 ms max)
  [BMA-DS operating conditions]. No gating: it is powered whenever the watch is, including deep sleep.
- Bus mode: CSB(10) tied to +3V3 -> I2C mode (datasheet recommends hard-wiring CSB to VDDIO for I2C) [BMA-DS 6.3].
  SDX(2)=SDA/GPIO12, SCX(12)=SCL/GPIO11. Auxiliary-interface pins ASDA(4)/ASCL(11): no-connect flagged.
- **I2C address: SDO(1) is tied to GND -> 7-bit address 0x18** (0x19 only if SDO were tied to VDDIO) [BMA-DS 6.5,
  "Primary I2C Interface"]. VERIFIED.
- **I2C pull-ups are present on the board:** R18 10K (SDA) and R20 10K (SCL) to +3V3 [KSCH]. Test pads TP6 (SCL)/TP7
  (SDA). Bus has only one slave (the BMA423), no RTC chip.
- Interrupts: INT1(5)->GPIO14, INT2(6)->GPIO13, direct, no pull-up/pull-down resistor on either line. Both GPIOs are
  RTC-capable (RTC_GPIO13/14), so either can wake from deep sleep. Active level/drive/latching are BMA423 register
  settings (see bma423.md); the board does not fix them. If they are put in the same EXT1 mask as the buttons they
  must be configured active-LOW (EXT1 applies one polarity to the whole mask), and an asserted latched INT must be
  cleared before sleeping or it re-wakes immediately.
- Placement [PCB]: U6 on the component side (F.Cu), position (79.89, 82.32) mm, rotation 0, pin 1 at local (-0.76,
  -0.75) (top-left of the F.Cu view, y down). From this, bma423.md section 7 derives the feature-engine axis remap
  (X=+y, Y=+x, Z=-z, i.e. Bosch's wrist table row 5). BMA423 init/reset detection and the missing official SensorAPI repo: bma423.md sections 1, 4, 12.

## 8. E-paper wiring and supply
J3 (24-pin 0.5 mm FPC) mapped to the Good Display GDEY0154D67 pin table [GD-SPEC section 5]; every row agrees.

| J3 | Panel pin | Board connection |
|---|---|---|
| 1,4 | NC | no-connect |
| 2 | GDR | Q3 AO3400A gate (R16 1M gate pull-down) |
| 3 | RESE | Q3 source, R17 2.2 Ohm sense to GND |
| 5 | VSH2 | C23 1 uF/50 V to GND |
| 6,7 | TSCL, TSDA | **no-connect**: external temperature sensor not fitted |
| 8 | BS1 | GND -> **4-line SPI** (8-bit, separate D/C#) [GD-SPEC Note 5-5] |
| 9 | BUSY | GPIO36 (output of panel, HIGH = busy, Note 5-4) |
| 10 | RES# | GPIO35 + R19 100K pull-up |
| 11 | D/C# | GPIO34 |
| 12 | CS# | GPIO33 (no pull resistor) |
| 13 | SCL | GPIO47 |
| 14 | SDA | GPIO48 |
| 15, 16 | VDDIO, VCI | +3V3 directly |
| 17 | VSS | GND |
| 18 | VDD | C24 1 uF/50 V |
| 19 | VPP | no-connect (test pin) |
| 20 | VSH1 | C25 1 uF/50 V |
| 21 | VGH | net PREVGH (C22 1 uF/50 V, D5) |
| 22 | VSL | C26 1 uF/50 V |
| 23 | VGL | net PREVGL (C29, D3) |
| 24 | VCOM | C27 1 uF/50 V |

- Boost: L2 47 uH from +3V3, Q3 AO3400A, D3/D4/D5 MBR0530, C20/C21 4.7 uF/50 V: the standard SSD1681 external charge-pump
  stage. Nothing for firmware to configure.
- **Supply is NOT switched:** VDDIO/VCI are wired to +3V3 with no load switch. The panel is powered whenever the
  watch is. It must be put into the controller's deep-sleep mode after each update, and RES# is held high by R19 while
  the MCU sleeps (so the panel is not reset). CS#/D/C#/SCK/SDA have no pulls and float in deep sleep; harmless once
  the controller is in deep sleep, but hold CS high (digital pad hold, see section 12) to be safe (ASSUMED).
- **Temperature sensor pins TSCL/TSDA are unused**, so the driver must use the SSD1681 internal sensor or write a
  temperature value; details in ssd1681.md.
- GPIO33-36 and GPIO47/48 are not RTC GPIOs (no RTC hold); GPIO33-37 sit in the VDD_SPI power domain (3.3 V for FN8).

## 9. Vibration motor
- Drive: GPIO17 -> R13 1K -> Q2 `S8050` NPN base; emitter GND; collector -> motor negative (M1.2). Motor positive
  and flyback diode D2 `1N4001` cathode on **+3V3** (not VBAT); C14 100 nF across the motor. **Active HIGH = motor on**
  (low-side switch). VERIFIED [KSCH]. Base current about (3.3-0.7)/1k = 2.6 mA, adequate for the motor.
- **No base pull-down on Q2:** a floating GPIO17 (reset, boot, deep sleep) leaves the base undefined. Drive GPIO17
  low and latch it with RTC hold (GPIO17 is RTC_GPIO17) before deep sleep; also keep it low at boot (ASSUMED good
  practice; verify no motor "tick" at boot).
- Motor `1020-15-003-001` flat coreless [MOTOR]: rated 3.0 V (usable 2.5-3.5 V), starting voltage <= 2.3 V, rated
  current <= 80 mA, >= 10,000 rpm, about 0.8 G. Surge current is higher than rated; the LDO limit (1.1 A) covers it.
  Keep pulses short (tens to hundreds of ms); no PWM requirement (net is named VIB_PWM but drive is plain on/off or PWM).

## 10. Clocks, RTC, flash
- **No external RTC chip:** the complete component list is U1 SR2HARU (reset), U2 LDO, U3 TP4054, U4 ESP32-S3, U5
  USBLC6-2P6, U6 BMA423 [KSCH]. VERIFIED. SQFMI's revision table: v3 "RTC: EXT 32KHz crystal" [DOCS-HW].
- **32.768 kHz crystal Y1** (2.0 x 1.2 mm, part number not given) on `32K_P` = U4 pin 21 = **GPIO15 (XTAL_32K_P)** and
  `32K_N` = pin 22 = **GPIO16 (XTAL_32K_N)**; load capacitors C17 on 32K_P and C18 on 32K_N to GND. VERIFIED
  [KSCH][ESP-DS Table 2-1]. Capacitor value: **10 pF each (schematic/PDF) vs 18 pF each (BOM CSV)**: CONFLICT,
  UNVERIFIABLE which is populated. Effective load per Espressif's formula CL = C1*C4/(C1+C4) + Cstray [ESP-HWDG]:
  with 10 pF and 2-4 pF stray about 7-9 pF, with 18 pF about 11-13 pF; crystal CL unknown. No parallel bias resistor
  is shown (Espressif says it is normally not populated). Crystal must have ESR <= 70 kOhm [ESP-HWDG]. Firmware
  must verify the oscillator actually runs (calibration cycles) and keep a "time valid" flag (as the SPEC says).
- 40 MHz main crystal Y2, 26 pF (schematic) load caps; R12 shown as 1M in series with XTAL_P in the schematic but
  "TBD" in the BOM: UNVERIFIABLE, irrelevant to firmware (production boards run).
- Flash: **ESP32-S3FN8 = 8 MB quad-SPI in-package flash, no PSRAM**, 3.3 V VDD_SPI, ambient -40..85 C [ESP-DS Table
  1-1]. VERIFIED. GPIO26-32 belong to the in-package flash and are no-connect on the board; flash endurance
  100,000 cycles, 20-year retention (Table 5-11); 80 MHz max flash clock.
- RTC slow clock options include the external 32 kHz crystal (datasheet "RTC Clock" list) [ESP-DS section 4.1.3.3 Clock; internal slow RC is typically about 136 kHz]. RTC
  SLOW/FAST memory stay powered in deep sleep [IDF-SLEEP].

## 11. USB and console
- Micro-USB J2 (shield and ID pins no-connect). D+ -> U5 `USBLC6-2P6` -> **GPIO20 (USB_D+)**, D- -> U5 -> **GPIO19
  (USB_D-)**: native full-speed USB of the ESP32-S3, no UART bridge chip, no auto-reset circuit (no DTR/RTS).
  SQFMI: "USB-Serial: built-in CDC/JTAG; bootload/reset: buttons" [DOCS-HW]. VERIFIED. VBUS is not wired to any USB
  pad of the MCU (only to the GPIO21 divider).
- Console reach: the USB Serial/JTAG controller appears as `/dev/ttyACM*` on Linux; `idf.py flash` can put the chip into
  download mode automatically while the USB device is present [IDF-USB]. UART0 (GPIO43 TX, GPIO44 RX) is exposed only on
  test pads TP1/TP2 (3.3 V logic) for a spare console. 
- **USB disappears in deep sleep:** the controller and PHY power off and the device "appears disconnected", then
  re-enumerates on wake [IDF-USB]. The host tooling in the SPEC (handle CDC vanishing) is required; to flash a sleeping
  watch use the BACK+UP chord (section 6) or press any button first.
- GPIO19/GPIO20 are dedicated to the USB Serial/JTAG function on this board; do not reuse them. Default pad state at
  reset has GPIO20 with USB_PU (D+ pull-up) enabled [ESP-DS Table 2-1 and its legend].
- If the console is used with no host attached the TX buffer fills and a 50 ms one-time wait occurs [IDF-USB]; the SPEC rule
  "console never keeps the watch awake on battery" must be implemented using GPIO21 (USB present), not by relying on CDC state.

## 12. ESP32-S3 facts that matter for these pins
All [ESP-DS v2.2] unless noted. VERIFIED.
- RTC-capable GPIOs: **GPIO0-21** (RTC_GPIO0..21) = usable for EXT0/EXT1 deep-sleep wake [Table 2-6; IDF-SLEEP]. Pins used
  here that are RTC-capable: buttons 0/6/7/8, INT 13/14, ADC 9, STAT 10, USB_DET 21, motor 17, I2C 11/12.
  **Not RTC-capable:** display CS/DC/RES/BUSY 33-36 and SPI 47/48 (use `gpio_hold_en`/`gpio_deep_sleep_hold_en`-style
  digital hold, not RTC hold; IDF API names to be confirmed by the firmware researcher).
- ADC1 = GPIO1-10 -> CH0-CH9; ADC2 = GPIO11-20 -> CH0-CH9; GPIO9 = ADC1_CH8, GPIO10 = ADC1_CH9 [Tables 2-6, 2-8]. Effective
  ranges and errors: ATTEN0 0-850 mV +-5 mV, ATTEN1 0-1100 +-6, ATTEN2 0-1600 +-10, ATTEN3 0-2900 +-50 [Table 5-6].
  Sampling max 100 kSPS, INL +-8 LSB, DNL +-4 LSB [Table 5-5].
- Absolute max on power pins -0.3..3.6 V; VIH = 0.75 VDD (max VDD+0.3), VIL = 0.25 VDD; internal pull-up/down 45 kOhm;
  pad drive 40 mA source/28 mA sink at PAD_DRIVER=3 [Tables 5-1, 5-4].
- Strapping: see section 6 table. Reset-state pulls: GPIO0 weak pull-up; GPIO45/46 weak pull-down (Table 3-1).
- GPIO15/16 are the 32 kHz crystal pins; using them as GPIO is impossible while the crystal is fitted.
- Deep-sleep current (datasheet typ): 7 uA (RTC memory only), 8 uA (RTC memory + RTC peripherals); light-sleep 240 uA
  [Table 5-10]. RTC-GPIO wake (EXT0/EXT1) is expected to keep the RTC peripherals domain powered (ESP-IDF decides; ASSUMED), so budget 8 uA, not 7 uA.
- ESP-IDF documentation tree is v6.1 for "stable" as of this date [IDF-SLEEP][IDF-ADC]; EXT1 modes available on S3:
  `ANY_LOW`, `ANY_HIGH` only (no ALL_LOW).

## 13. Expected sleep-current contributors
SQFMI publishes no sleep-current figure; the battery-life page only claims 5-7 days for timekeeping (Arduino firmware)
and 2-3 days with Wi-Fi [DOCS-BAT]. Derived floor for Quartz deep sleep (all typ, 25 C, ASSUMED until measured):

| Contributor | Current | Source |
|---|---|---|
| ESP32-S3 deep sleep (RTC mem + RTC periph) | 8 uA | [ESP-DS Table 5-10] |
| Battery divider R8+R9 (always on) | 7.2-9.1 uA (Vbat/460k) | computed |
| BMA423 with step counter running (low-power mode, 50 Hz minimum for features: 13-27 uA depending on averaging; suspend 3.5 uA but then nothing counts; Bosch marketing says < 30 uA) | **13-27 uA** | [bma423.md section 8; Bosch rev 2.0 datasheet p.19] |
| LDO RT9080 quiescent | 2 uA (4 max); ME6211 if fitted is higher (not checked) | [RT9080 DS] |
| SR2HARU smart reset | 1-1.5 uA | [SR1-DS], ST SR2 summary |
| TP4054 BAT-pin leakage with no USB | <= 2 uA | [TP4054] |
| SSD1681 + panel in deep sleep | see ssd1681.md | [GD-SPEC] |
| Button pull-ups | 0 (33 uA per pressed button) | computed |
| Total estimate | **about 35-55 uA with the step counter running** (about 25 uA if the BMA423 were suspended) | derived |

At 170-180 mAh that is roughly 130-210 days of pure sleep (the accelerometer is the largest single contributor); actual life is dominated by wake energy (display refresh,
radio). Measure on hardware (SPEC power baseline).

## 14. Other quirks, test points, document discrepancies
- Test points: TP1 TX (GPIO43), TP2 RX (GPIO44), TP3 BOOT (GPIO0/BTN3), TP4 3V3, TP5 GND, TP6 SCL, TP7 SDA.
- ESP32-S3 symbol in the schematic carries ESP32-original labels ("DAC_1/DAC_2" on GPIO18/17): ESP32-S3 has no DAC; ignore.
- GPIO18 and GPIO1-5, 37-42 are unconnected (no pads); leave as default inputs.
- USB cable: any charge-only cable works for charging, but data needs a data cable (docs FAQ) [DOCS-START].
- Docs mention the board revision is shown by the library's About screen; there is no hardware revision strap or ID
  pin on the board (UNVERIFIABLE how firmware detects a v3 board; assume v3 only).
- Case/screen warnings from docs: screen adhesive unaffected above 60 C is damaged [docs FAQ]; irrelevant to firmware.

## 15. Source tags
Retrieval date for all web sources: 2026-10-05. Full list with notes: `sources.md`.
| Tag | Source |
|---|---|
| SCH | https://github.com/sqfmi/watchy-hardware/blob/v3.0/WatchySchematic.pdf (single sheet) |
| KSCH | https://raw.githubusercontent.com/sqfmi/watchy-hardware/v3.0/Watchy.kicad_sch (netlist extracted by script) |
| PCB | https://raw.githubusercontent.com/sqfmi/watchy-hardware/v3.0/Watchy.kicad_pcb |
| BOM | https://raw.githubusercontent.com/sqfmi/watchy-hardware/v3.0/WatchyBOM.csv |
| HWREPO | https://github.com/sqfmi/watchy-hardware (tags v1.0, v1.5, v2.0, branch v3.0, main) |
| DOCS-HW | https://github.com/sqfmi/watchy-docs/blob/main/docs/hardware.md (published at https://watchy.sqfmi.com/docs/hardware) |
| DOCS-START | https://github.com/sqfmi/watchy-docs/blob/main/docs/getting-started.md (bootloader/reset procedure) |
| DOCS-BAT | https://github.com/sqfmi/watchy-docs/blob/main/docs/battery-life.md |
| ESP-DS | https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf (v2.2): Table 1-1 (p.13), 2-1/2-6/2-8 (pp.16-28), 3-1..3-5 (pp.32-35), 5-1..5-6 (pp.64-66), 5-10 (p.67), 5-11 (p.68) |
| IDF-SLEEP | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/sleep_modes.html (v6.1) |
| IDF-USB | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/usb-serial-jtag-console.html (v6.1) |
| IDF-ADC | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/adc/adc_oneshot.html (v6.1) |
| ESP-HWDG | https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html (RTC clock, CHIP_PU, strapping) |
| TP4054 | https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/6128/TP4054-5272.pdf (pp.4-7) |
| RT9080 | https://www.richtek.com/assets/product_file/RT9080/DS9080-09.pdf (features p.1, electrical p.6) |
| SR1-DS | ST SR1 datasheet DocID026048 rev 2 (mirror https://cdn.components.ru/media/PDF/2180717.pdf), Tables 6 and 7 (ordering) |
| BATT | https://www.powerstream.com/lip/GMB042030.pdf (GM Battery GMB042030, p.2 Table 1) |
| MOTOR | https://github.com/SeeedDocument/Bazaar_doc/raw/master/316040001/1020_datasheet.doc (spec 1020-15-003-001) |
| BMA-DS | https://watchy.sqfmi.com/pdf/BST-BMA423-DS000-1509600.pdf (Bosch BST-BMA423-DS000-01 v1.1, May 2019, preliminary) and the newer rev 2.0 https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/876/BMA423_Rev2.0_Aug2019.pdf (BST-BMA423-DS004-00, Aug 2019); sections 6.2, 6.3, 6.5 are the same in both; details in bma423.md |
| GD-SPEC | https://v4.cecdn.yun300.cn/100001_1909185148/GDEY0154D67-new.pdf (Good Display GDEY0154D67 specification, section 5) |

## 16. Unresolved items
1. Which populated values apply to C17/C18 (10 vs 18 pF), C16/C19, R12, and the LDO part (RT9080 vs ME6211): needs a board
   inspection or the assembly files. Mitigation: verify the 32 kHz oscillator at runtime (calibration), keep "time valid".
2. GPIO10 semantics measured on hardware (expected: HIGH with USB present in charging and full states). Measure once.
3. ADC behaviour above 2.9 V at the pin (Vbat > 3.7 V): characterise saturation on a real unit; decide percent curve.
4. SR2HARU parameters rest on the SR1 sibling datasheet plus SQFMI's 4 s procedure; the SR2 datasheet could not be
   retrieved. Low risk. (Hold time for download/reset chord documented as 4 s.)
5. Whether the supplied LiPo contains a protection circuit (cell datasheet is for a bare cell) and its real capacity
   (180 mAh typ per cell spec vs "200 mAh" marketing).
6. Left/right physical button positions in the wearer's view (only top/bottom pairing is documented).
7. MENU/DOWN naming rests on elimination plus the brief; no official document labels BTN1/BTN4.
