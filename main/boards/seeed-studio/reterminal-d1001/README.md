# Seeed Studio reTerminal D1001

XiaoZhi port for the Seeed Studio reTerminal D1001 (ESP32-P4NRW32 +
ESP32-C6 Wi-Fi coprocessor, ES8311 speaker, ES7210 dual-mic, 800x1280 MIPI-DSI
panel, GSL3670 touch).

## Status

Bring-up in progress. Validated on hardware:

- PCA9535 power sequence (power hold, panel rails, PA gating).
- ES8311 playback and ES7210 capture codecs on the separate TX/RX I2S buses.
- ESP-Hosted SDIO link to the factory ESP32-C6 firmware (Wi-Fi scan and
  config AP work; the co-proc runs esp-hosted 2.3.0 while this image ships
  host 2.12.x, so upgrade the C6 slave firmware separately before
  long-running use).
- XiaoZhi boots into Wi-Fi config mode.

Not yet implemented: display, touch, GPIO3 button, device AEC validation.

The hardware facts come from the
[reTerminal D10xx product documentation](https://wiki.seeedstudio.com/reterminal_d10xx_main_page/)
and the
[official reTerminal-D1001 repository](https://github.com/Seeed-Studio/reTerminal-D1001).
See `docs/reterminal-d1001-port-design.md` for the port design and milestones.

## Build

Use ESP-IDF 6.0.2 and build the single variant with:

```sh
python3 scripts/build.py seeed-studio/reterminal-d1001 --name reterminal-d1001
```

Verified on a unit with 32 MB Winbond flash (ID ef/4019); the board config
assumes 32 MB and uses `partitions/v2/32m.csv`. The factory images are
published in the Seeed BSP repository under `firmware/`.

The ESP32-P4 application and the ESP32-C6 ESP-Hosted slave are separate
firmware images with separate version lifecycles. Building or updating this
P4 image does not build or silently update the C6 firmware; record and
manage both versions independently.

## Pin facts (from the Seeed BSP)

| Function        | Pins / address                              |
|-----------------|---------------------------------------------|
| Audio I2C (I2C1)| SDA GPIO20, SCL GPIO21                      |
| PCA9535         | 0x20 (power hold bit8, PA bit11, panel bits 0/2/7/12) |
| ES8311 (DAC)    | 0x18, I2S0 TX: MCLK 33, BCLK 32, WS 31, DOUT 30 |
| ES7210 (ADC)    | 0x40, I2S1 RX TDM: MCLK 29, BCLK 28, LRCK 27, DIN 26 |
| C6 SDIO         | CMD 6, D0 7, D1 8, D2 9, D3 10, CLK 11, CHIP_PU 13 |
| Panel           | 800x1280 JD9365DA-H3, MIPI-DSI 2 lane, LDO3 2.5 V |
| Touch           | GSL3670 on I2C0 (SDA 37, SCL 38), INT GPIO16 |
| Button          | GPIO3                                       |
