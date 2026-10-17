# Bill of materials (rev 2)

Read from `cadProd_compact.kicad_sch` (saved 2026-10-15 09:16). Every resistor, capacitor and LED uses the **0805** hand-solder footprint.

## Capacitors (25)

| Value      | Qty    | References                                    | Notes                                              |
| ---------- | ------ | --------------------------------------------- | -------------------------------------------------- |
| **100 nF** | **10** | C2, C4, C6, C10, C13, C16, C17, C19, C22, C25 | C17 is written as "0.1uF", which is the same value |
| **10 µF**  | **6**  | C1, C3, C5, C8, C20, C21                      |                                                    |
| **1 µF**   | **6**  | C7, C9, C14, C15, C18, C24                    |                                                    |
| **2.2 µF** | **2**  | C11, C12                                      | PCM5102 charge pump / LDOO                         |
| **47 nF**  | **1**  | C23                                           | Q2 soft start                                      |

Ceramic X5R/X7R, 6.3 V or more (10 V gives more margin for the 10–22 µF parts).

## Resistors (27)

| Value      | Qty    | References                                   | What for                                                             |
| ---------- | ------ | -------------------------------------------- | -------------------------------------------------------------------- |
| **10 kΩ**  | **10** | R6, R7, R8, R9, R10, R11, R12, R18, R22, R26 | SD pull-ups, BQ24074 TS, PGOOD/CHG, EN/BOOT, amp SHDN, XSMT, Q2 gate |
| **1 kΩ**   | **5**  | R13, R14, R15, R17, R21                      | LEDs (2.2–4.7 kΩ also works and saves power)                         |
| **1 MΩ**   | **2**  | R5, R27                                      | Battery divider                                                      |
| **100 kΩ** | **2**  | R16, R25                                     | EN_ANAVOL pull-down, Q2 gate pull-up                                 |
| **20 kΩ**  | **2**  | R19, R20                                     | Amp input                                                            |
| **5.1 kΩ** | **2**  | R1, R2                                       | USB-C CC1/CC2                                                        |
| **3.9 kΩ** | **2**  | R23, R24                                     | Amp feedback                                                         |
| **2 kΩ**   | **1**  | R3                                           | BQ24074 ILIM                                                         |
| **1.2 kΩ** | **1**  | R4                                           | BQ24074 ISET (charge current)                                        |

## ICs and transistors

| Part                     | Qty | References                         | Package        |
| ------------------------ | --- | ---------------------------------- | -------------- |
| ESP32-S3-WROOM-2 (N16R8) | 1   | ESP1                               | Module         |
| BQ24074RGT               | 1   | BATPROT1                           | VQFN-16 3×3 mm |
| TPS7A2033                | 2   | Digital_Vol, AnaVol1               | SOT-23-5       |
| PCM5102                  | 1   | DAC1                               | TSSOP-20       |
| MAX9722AEUE              | 1   | AMP1                               | TSSOP-16       |
| AO3401A (P-MOSFET)       | 1   | Q2                                 | SOT-23         |
| PESD5V0L1BA1 (ESD)       | 6   | ESD1, ESD2, ESD3, ESD4, ESD7, ESD8 | SOD-323        |

## LEDs (0805)

| Colour | Qty | Reference |
| ------ | --- | --------- |
| Red    | 1   | D1        |
| Orange | 1   | D2        |
| Green  | 1   | D3        |
| Purple | 1   | D4        |
| Blue   | 1   | D5        |

## Buttons and connectors

| Part                                   | Qty | References                               | Footprint                          |
| -------------------------------------- | --- | ---------------------------------------- | ---------------------------------- |
| Push button with stem (Alps SKQG)      | 6   | HOME1, NEXT1, PLAY1, PREV1, VOL+1, VOL-1 | SW_SPST_SKQG_WithStem              |
| Small push button (Alps SKRK)          | 2   | BOOT1, EN1                               | SW_Push_SPST_NO_Alps_SKRK          |
| USB-C receptacle (GCT USB4105)         | 1   | USBC1                                    | USB4105 16P                        |
| Micro-USB (Würth 629105150521)         | 1   | J1                                       |                                    |
| 1×4 pin header, 2.54 mm (USB)          | 1   | J2                                       |                                    |
| microSD socket (Hirose DM3AT-SF-PEJM5) | 1   | MicroSD1                                 |                                    |
| 3.5 mm jack (PJ-320D)                  | 1   | AUX1                                     |                                    |
| Battery connector (JST)                | 1   | Battery1                                 | **Check the footprint, see below** |

## Totals

| Type       | Unique values | Parts |
| ---------- | ------------- | ----- |
| Capacitors | 6             | 25    |
| Resistors  | 9             | 27    |
| Everything |               | 84    |

## Check before ordering

- **Battery1 footprint is `Crystal:Resonator-2Pin_W6.0mm_H3.0mm`.** That's a crystal footprint, not a JST battery connector. Change it to the matching JST footprint, e.g. `Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal` for JST-PH.
- **C3/C5 "10-22yF":** set one real value (the "y" is how KiCad shows µ).
- **Both TPS7A2033s use the `AP2112K-3.3` symbol.** Its pinout (1 VIN, 2 GND, 3 EN, 4 NC, 5 VOUT) matches the TPS7A2033 in SOT-23-5, so it's fine. Only the symbol name is different.
- **AUX1 has no value.** Fill it in (PJ-320D) so it shows in the BOM export.
- **Spares:** 0805 parts are easy to lose when hand soldering. Order a 10–20 % extra of the common values (100 nF, 10 kΩ, 1 µF, 10 µF).
