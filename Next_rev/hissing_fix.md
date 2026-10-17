# Hiss under music: diagnosis, firmware fix, and next-rev checklist

## Context
Custom ESP32-S3 board: PCM5102A DAC → MAX9722 headphone amp, analog rail from a TPS7A2033 LDO (`config.h`). The user hears a smooth hiss on sensitive IEMs. It is **worse at low volume** and **gone when paused**.

What that points to:
- **Gone when paused:** pause drives `XSMT` low (`Engine::updateMute`, `engine.h:711`) while the amp stays on. So the noise does not come from the MAX9722 itself. It comes from the DAC output (or its supply), or it is carried in the digital stream.
- **Worse at low volume:** volume is purely digital (`GaplessOut::scale`, `engine.h:128`). The analog chain always runs at full gain, so the DAC's noise floor × amp gain stays the same at every volume. At high volume the music masks it. At low volume it doesn't.
- There is also a firmware effect on top: at low volume, music is attenuated and then re-quantized to **16 bits** without dither. And the "never send zero" trick (`engine.h:96`, `0 → 1`, applied only when both channels are 0) is non-linear. Quiet passages that sit around ±1–2 LSB come out as gritty, hiss-like noise.

**Answer to "software or hardware?":** both, but mostly hardware. Firmware can remove the low-volume re-quantization noise and the non-linearity, which is a real and audible win on IEMs. The steady analog floor can only be lowered by reducing analog gain before the amp. A resistor swap on the current board can do that.

## Step 1: Quick diagnosis (no code change needed except a temp flag)
1. Temporary test: keep `XSMT` HIGH while paused (add a `#define HISS_TEST 1` that makes `updateMute()` always enable the DAC). Listen while paused, which sends digital silence plus the 1 LSB offset.
   - Hiss present → analog floor (DAC + gain/layout). Hardware fix is the main lever.
   - Silent → the noise is in the digital stream, and the firmware fix below should solve it.
2. Same song, max volume vs low volume, with the IEM volume matched by ear. If the low-volume version is grainier, that confirms the re-quantization part.

## Step 2: Firmware fix: 32-bit output, linear anti-mute offset
File: `flac_player_4/flac_player/engine.h` (`GaplessOut` only; the rest of the engine is untouched).

- Replace the `AudioOutputI2S *out` inside `GaplessOut` with a small 32-bit I2S output built on Arduino-ESP32's `ESP_I2S.h` `I2SClass`. This is the same new-style driver ESP8266Audio 2.x uses, so there is no legacy/new driver conflict. Settings: `I2S_MODE_STD`, `I2S_DATA_BIT_WIDTH_32BIT`, stereo, same pins (`PIN_I2S_BCK/LRCK/DIN`) and DMA sizes from `config.h`. The PCM5102A accepts 32-bit frames natively (FMT=I2S, SCK grounded → PLL from BCK).
  - Keep the existing interface (`begin`, `SetRate`, `SetChannels`, `ConsumeSample` returning false when the DMA is full) so `fillSilence`, `tone`, `measure()` and the voice/duck mixing keep working unchanged. Use a non-blocking write (timeout 0 / check free space) to keep the "return false when full" behaviour.
- `scale()` → produce `int32_t`: `y = lrintf(x * gain * 65536.0f)`, clamped to the int32 range. Volume attenuation then keeps 16+ bits of resolution down to the bottom of the 80-step range.
- Replace the conditional `0 → 1` with a **constant** tiny DC offset added to every sample (e.g. `+256` in 32-bit units, about −143 dBFS). It is linear, inaudible, and still keeps the PCM5102A zero-detect from auto-muting (the reason for the original trick, comment at `engine.h:24-27`).
- `begin()` in `Engine` (`engine.h:157`) builds the new output instead of `AudioOutputI2S`. Remove `SetGain(1.0f)` and `SetBuffers`, which belong to the old class.
- Dither is not needed at 32-bit (the DAC's own noise is far above the 32-bit LSB).

## Step 3: Hardware on the current board (rework)
- **Lower the MAX9722 gain** by swapping its input/feedback resistors. With the PCM5102A at 2.1 Vrms full scale, IEMs need only ~0.1–0.3 Vrms. Aim for roughly −10 to −15 dB total (e.g. Rf/Rin ≈ 0.25). Then raise `MAX_GAIN` / `VOLUME_DEFAULT` in `config.h` if max volume ends up too quiet. This cuts the DAC noise floor at your ears by the same number of dB, and is the single biggest improvement.

## Step 4: Next PCB revision checklist
- **Gain staging:** design the amp gain for IEMs (attenuation, not gain), or add a passive divider between the DAC and amp. A jumper or two resistor options lets you pick IEM vs headphone gain.
- **PCM5102A supplies:** AVDD and CPVDD from the TPS7A2033 (clean). DVDD separate, from the digital 3.3 V through a ferrite bead. Use 10 µF + 100 nF at each pin, placed tight. Put 1 µF on LDOO and 2.2 µF on CAPP/CAPM, close to the pins. Tie SCK to GND, and strap FMT, FLT, DEMP solidly (no floating pins).
- **DAC output filter:** 470 Ω + 2.2 nF RC on OUTL/OUTR (from the datasheet) before the amp input. Use C0G caps for the audio path and amp input coupling, never Y5V/X5R in the signal path.
- **MAX9722:** charge-pump caps (C1 flying, C2 hold) right at the pins with short loops. Put the input coupling cap and gain resistors next to the amp and away from digital traces. Keep the PVSS loop small.
- **Grounding:** one solid ground plane. Keep the analog section (DAC output → amp → jack) physically separate from the ESP32, SD card and charger, with no digital traces under it. Return the headphone jack sleeve directly to the amp ground, not through the digital area.
- **Noisy sources:** keep the charger and any buck/boost inductor away from the analog corner. Give the I2S lines a series resistor (22–33 Ω) at the ESP32 to soften the edges. Route SD SPI and I2S away from the analog outputs.
- **Test points:** add pads on DAC OUTL/OUTR and the amp output, so the next noise hunt can be done with a scope/sound card.

## Verification
- Build in Arduino IDE (ESP32S3 Dev Module, settings from `README.md`). Check that the serial `[rate]` line still reports ≈1.000x at 44.1 kHz and 22.05 kHz (voice clips, chime).
- Listen on IEMs: quiet passages at low volume should be clean, not grainy. Pause, voice clips, ducking, fades, chime, ticks and gapless transitions should all behave as before, with no pops at song ends (the DAC must still never auto-mute).
- Repeat Step 1's `HISS_TEST` after the firmware change. Any hiss that remains is the analog floor, which Step 3/4 address.
