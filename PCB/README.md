# Music player PCB: next revision (rev 2)

Everything to change, check and test for rev 2. The main goal is power saving while the player is **off**:

|                  | Off current   | Shelf life on 1000 mAh                                                         |
| ---------------- | ------------- | ------------------------------------------------------------------------------ |
| Rev 1, estimated | ~0.6–1 mA     | ~6 weeks                                                                       |
| Rev 2, target    | **~25–30 µA** | well over a year (in practice the battery's own self-discharge sets the limit) |

The estimates come from datasheet typical values. Measure the real numbers at bring-up (section 6).

---

## 1. Power rails (the new design)

```
BAT ── BQ24074 ── +REG ──┬── TPS7A2033 "Digital_Vol" (EN = VIN, always on) ── AwO_D3v3 ──┬── ESP32-S3
                         │                                                                ├── BQ24074 EN2, pull-ups R9 R10 R11 R12
                         │                                                                └── Q2 AO3401A ── D3v3 (switched) ──┬── SD card + R6 R7
                         │                                                                                                    └── PCM5102 DVDD + CPVDD
                         └── TPS7A2033 "AnaVol" (EN = EN_ANAVOL) ── A3v3 (switched) ──┬── PCM5102 AVDD
                                                                                        └── MAX9722 amp
```

| Rail         | What's on it                                                              | On while the player is off?         |
| ------------ | ------------------------------------------------------------------------- | ----------------------------------- |
| **AwO_D3v3** | ESP32-S3, BQ24074 EN2, R9 (PGOOD), R10 (CHG), R11 (EN), R12 (BOOT)        | Yes                                 |
| **D3v3**     | SD card VDD, R6 (CMD), R7 (DAT0), PCM5102 DVDD (pin 20) and CPVDD (pin 1) | **No**, Q2 switches it off          |
| **A3v3**     | PCM5102 AVDD, MAX9722                                                     | **No**, `EN_ANAVOL` switches it off |

---

## 2. Changes already in the schematic

- [x] Digital regulator changed from MCP1825 to **TPS7A2033** (Iq ~6.5 µA instead of ~120 µA), with EN tied to VIN
- [x] **C24 1 µF** at the digital regulator's VIN
- [x] **C21/C22** (10 µF + 100 nF) at its output
- [x] Always-on rail **AwO_D3v3** split from the switched **D3v3**
- [x] **Q2 AO3401A** high-side switch, with R25 100k gate pull-up (off at boot and in sleep) and R26 10k to the GPIO
- [x] **R5** (SD Det pull-up, ~330 µA with a card in) **removed**. The firmware uses the internal pull-up while awake.
- [x] R6 and R7 (SD pull-ups) moved to switched D3v3, so they can't back-power the card
- [x] PCM5102 DVDD/CPVDD moved to switched D3v3
- [x] R9, R10, R11, R12 and BQ24074 EN2 on AwO_D3v3. EN on the switched rail would hold the ESP32 in reset forever.

---

## 3. Still to do in the schematic

### Must fix

- [x] **C23 (47 nF) from Q2 gate to Q2 source**, side by side with R25. This gives a ~0.5 ms turn-on (10k × 47 nF), so the inrush is about 75 mA instead of a dip that could reset the ESP32. (Gate-to-GND would switch D3v3 on briefly at every power-up.)
- [ ] **Connect the `GPIO` label at R26 to a real pin: IO21 (module pad 23).** IO11 (pad 19), IO12 (pad 20) and IO18 (pad 11) are also free. Avoid the strapping pins IO0, IO3, IO45 and IO46.
- [ ] **Give the two `GPIO` labels separate names.** Same-named labels are one net in KiCad. Use `BAT_SENSE` → IO8 (pad 12) and `D3V3_EN` → IO21 (pad 23).
- [x] **Battery divider on IO8 (module pad 12)** (R5/R27 1 MΩ, C25 100 nF). It's needed for battery %, LED4 (above 60 %) and the low-battery warnings:
    ```
    BAT+ ──[1 MΩ]──┬── IO8
                   ├──[1 MΩ]── GND
                   └──[100 nF]── GND
    ```
    It draws about 2 µA. IO8 is on ADC1.
- [ ] **Make thermal refes for the `GND` on `LEDs` and Battery**
- [ ] **Battery connector** Make the battery connector fit a JST connection

### Check against datasheets

- [ ] **BQ24074 TMR (pin 14) and ITERM (pin 15)** are unconnected. Check what that means for the safety timer and the termination current, and tie them as intended.
- [ ] **BQ24074 TS:** R8 is a fixed 10k to GND, so temperature protection is off. For a LiPo in a closed case, consider a pack with a 10k NTC wired to TS.
- [ ] **TPS7A2033 output capacitance:** confirm that 10 µF + 100 nF on each output is within the stable range. It should be.
- [ ] **AO3401A pinout in the footprint:** 1 = Gate, 2 = Source, 3 = Drain (SOT-23).

### LEDs

- [ ] **Make the schematic and firmware agree on the colours.** Right now they don't:

    | GPIO | Net  | Schematic | Firmware `config.h` |
    | ---- | ---- | --------- | ------------------- |
    | 38   | LED1 | D1 red    | green               |
    | 39   | LED2 | D2 orange | purple              |
    | 40   | LED3 | D3 green  | orange              |
    | 41   | LED4 | D4 purple | blue                |
    | 42   | LED5 | D5 blue   | red                 |

    Decide which colour goes where, then make the schematic, the silkscreen and `config.h` match.

- [ ] Optional: change R13–R21 from 1 kΩ to **2.2–4.7 kΩ**. That's about 1 mA per LED now; higher values are usually still bright enough behind a case.

### Optional

- [ ] **External 1 MΩ pull-up on PLAY** to AwO_D3v3. The firmware could then power down the RTC peripherals in deep sleep, saving a few µA.
- [ ] **MAX17048 fuel gauge** on I2C (e.g. IO11/IO12), if the divider's ~±10 % isn't good enough. It stays accurate while charging and under load, and draws ~4 µA in hibernate. It needs a firmware driver.
- [ ] Remove any leftover symbols that have no footprint (the old AP2112K symbol).

---

## 4. Footprints and BOM

- [ ] **Digital_Vol:** SOT-23-5 for the TPS7A2033 (it was the MCP1825's SOT-223/DDPAK). Pins: 1 VIN, 2 GND, 3 EN, 4 NC, 5 VOUT.
- [ ] **Q2:** SOT-23. Add **C23** and **C24** footprints.
- [ ] **The BOM says µF**, not "yF". KiCad shows µ as y on some values.
- [ ] TPS7A2033 ×2 (digital + analog). The MCP1825 is no longer used.
- [ ] Battery divider: 2 × 1 MΩ, 1 × 100 nF (0402/0603).

---

## 5. Layout

- [ ] **C24** right next to the digital TPS7A2033's VIN pin. **C21/C22** right next to its VOUT.
- [ ] **C23** close to Q2, between its gate and source pads.
- [ ] Keep the ESP32's decoupling (C6, C7, C8) right at the module's 3V3 pin.
- [ ] **C1/C2** (SD decoupling) close to the SD socket, on D3v3.
- [ ] Battery divider close to IO8, away from the I2S and SD clock traces. The 1 MΩ node is high-impedance and picks up noise.
- [ ] Short, wide traces from Q2 to the SD card VDD.

---

## 6. Bring-up tests

1. **Rails:** power from the battery only. AwO_D3v3 should be 3.3 V. **D3v3 and A3v3 should be 0 V** until the firmware turns them on.
2. **Boot:** the ESP32 starts every time (if it doesn't, EN/R11 is on the wrong rail). Hold Play for 3 s to turn on.
3. **Inrush:** with a scope on AwO_D3v3, the firmware switches D3v3 on. The dip must stay above about 3.0 V, and the ESP32 must not reset.
4. **SD and audio:** cards mount, music plays, and there's no pop at power-on.
5. **Off current:** power off, then put a multimeter (µA range) in series with the battery. Target **~25–30 µA**. If it's much higher:
    - Is D3v3 really 0 V?
    - Is any ESP32 pin driving high into the SD card or DAC?
    - Are the LEDs off?
6. **Charging:** plug in USB. The orange LED lights while charging, also while the player is off. Unplugged, there's no wake loop.
7. **Battery %:** compare Settings > Battery with a multimeter on BAT+ (around 4.2 V full, 3.7 V about 50 %).
8. **LEDs:** check each colour against the table in section 3.

---

## 7. Firmware changes needed for rev 2

These go into `flac_player_4/flac_player/` once the board exists. They're not done yet; ask when the GPIO is final.

- [ ] `config.h`: new `PIN_D3V3_EN 21` (or whichever pin you choose). LOW = D3v3 on.
- [ ] Boot: drive `PIN_D3V3_EN` LOW, wait ~5 ms, then start the SD card.
- [ ] Before deep sleep: drive **all** SD pins (CS, CLK, MOSI, MISO) and I2S pins **LOW**, then set `PIN_D3V3_EN` HIGH and hold it.
    - Rev 1 holds SD CS _high_ in sleep. On rev 2 that would push current into the unpowered card through its protection diodes.
- [ ] `PIN_BAT_ADC 8`. This turns on Settings > Battery, LED4 above 60 %, the red low-battery LED and the "Battery low" / "Battery critical" warnings. That code is already written.
- [ ] LED pin colours in `config.h` to match section 3.
