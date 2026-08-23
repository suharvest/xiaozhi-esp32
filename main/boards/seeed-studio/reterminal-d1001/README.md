# Seeed Studio reTerminal D1001

XiaoZhi port for the Seeed Studio reTerminal D1001 (ESP32-P4NRW32 +
ESP32-C6 Wi-Fi coprocessor, ES8311 speaker, ES7210 dual-mic, 800x1280 MIPI-DSI
panel, GSL3670 touch).

## Status

Validated on hardware (P4 v1.3, 32 MB Winbond flash ef/4019, 32 MB PSRAM):

- PCA9535 power sequence (power hold, panel rails, PA gating, LCD/touch
  reset). The expander macros are pin *masks*; the BSP bit numbers are
  0/2/7/8/11/12.
- ES8311 playback and ES7210 capture codecs on the separate TX/RX I2S buses
  (16 kHz capture baseline, matching the Seeed factory BSP).
- ESP-Hosted SDIO link to the factory ESP32-C6 firmware (Wi-Fi, config AP and
  the XiaoZhi cloud connection all work; the co-proc runs esp-hosted 2.3.0
  while this image ships host 2.12.x, see "C6 firmware" below).
- GPIO3 button: toggles the chat state, or enters Wi-Fi config mode while the
  device is starting.
- 800x1280 JD9365DA-H3 MIPI-DSI panel (2 lanes at 1000 Mbps, LDO3 at 2.5 V,
  60 MHz DPI clock, RGB565 with one framebuffer) using the Seeed init
  sequence in `lcd_init_cmds.h`, plus the GPIO14 PWM backlight
  (`jd9365: LCD ID: 93 65 04`, `Display initialized`).
- GSL3670 capacitive touch on I2C0, polled (no interrupt), registered as an
  LVGL input device (`gsl3670: load fw success`, `Touch panel initialized`).
  The driver is vendored from the Seeed BSP because no registry component
  exists; `gsl_point_id.c` keeps its original Silead GPL-2.0 header.
- Idle stability: 30 min with the display on, free SRAM flat at ~248 KB.

Not yet validated: device AEC, 30 min continuous conversation, touch
coordinate orientation on the panel (the BSP values swap_xy=0, mirror_x=1,
mirror_y=1 are used as-is).

### Wake word and the ESP-IDF version

The wake word is **disabled on the ESP-IDF 6.0 build** of this board. With
esp-sr 2.4.7 on IDF 6.0.2 (prebuilt library set `esp32p4_less_v3_idf6`) the
CPU hangs inside wakenet9/multinet7 inference right after the AFE pipeline is
created, and the HP watchdog resets the chip in a loop. This is not a board
problem: the same source tree and sdkconfig built with ESP-IDF 5.5.4
(library set `esp32p4_less_v3`) runs the WakeNet pipeline stably. Ruled out
on hardware: reference channel/AEC, 16/24 kHz, every wn9 model, 16 MB vs
32 MB flash layout and 32-bit flash cache, esp-sr 2.5.1 (fails earlier for an
unrelated reason). See `d1001-dev/HANDOFF.md` (ignored dir) for the logs and
the esp-sr issue draft.

Two build entries therefore exist under the same name:

| ESP-IDF | Flash layout | Wake word | Notes |
|---------|--------------|-----------|-------|
| >= 6.0 (upstream CI) | `partitions/v2/32m.csv`, 32-bit flash cache | off (VAD + button) | default path |
| < 6.0 (5.5.4 tested) | `partitions/v2/16m.csv` | on | IDF 5.5 cannot address this flash chip above 16 MB, so the assets partition must stay below 16 MB |

Both entries set `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`: before the
scheduler starts the only internal heap is RETENT_RAM (~99 KB) and the
esp_hosted SDIO mempools (88 KB) otherwise leave no room for the main task
stack once `.data/.bss` grow (symptom: `Mem alloc fail. size 0x2a00 caps
0x804` / `esp_startup_start_app app_startup.c:83`).

Do not mix the two layouts on one device without re-flashing the partition
table; the preset-Wi-Fi NVS lives at 0x3b000 (32m) or 0x9000 (16m).

### C6 firmware

The factory C6 image is esp-hosted 2.3.0. Host-driven slave OTA
(`esp_hosted_slave_ota_*`) requires slave >= 2.6.0 and espressif/esp-hosted-mcu
publishes no prebuilt C6 SDIO binaries, so the upgrade needs a self-built
slave image and a physical flashing path; it is tracked separately from the
P4 image and is not part of this port.

The hardware facts come from the
[reTerminal D10xx product documentation](https://wiki.seeedstudio.com/reterminal_d10xx_main_page/)
and the
[official reTerminal-D1001 repository](https://github.com/Seeed-Studio/reTerminal-D1001).
See `docs/reterminal-d1001-port-design.md` for the port design and milestones.

## Build

```sh
python3 scripts/build.py seeed-studio/reterminal-d1001 --name reterminal-d1001
```

The build entry is selected by the ESP-IDF version in the environment (see
the table above). The factory images are published in the Seeed BSP repository
under `firmware/`.

The ESP32-P4 application and the ESP32-C6 ESP-Hosted slave are separate
firmware images with separate version lifecycles. Building or updating this
P4 image does not build or silently update the C6 firmware; record and
manage both versions independently.

## Pin facts (from the Seeed BSP)

| Function        | Pins / address                              |
|-----------------|---------------------------------------------|
| Audio I2C (I2C1)| SDA GPIO20, SCL GPIO21                      |
| PCA9535         | 0x20 (power hold bit8, PA bit11, LCD pwr bit0, LCD rst bit2, backlight bit7, touch rst bit12) |
| ES8311 (DAC)    | 0x18, I2S0 TX: MCLK 33, BCLK 32, WS 31, DOUT 30 |
| ES7210 (ADC)    | 0x40, I2S1 RX TDM: MCLK 29, BCLK 28, LRCK 27, DIN 26 |
| C6 SDIO         | CMD 6, D0 7, D1 8, D2 9, D3 10, CLK 11, CHIP_PU 13 |
| Panel           | 800x1280 JD9365DA-H3, MIPI-DSI 2 lane, LDO3 2.5 V, backlight PWM GPIO14 |
| Touch           | GSL3670 on I2C0 (SDA 37, SCL 38), INT GPIO16 (unused, polled) |
| Button          | GPIO3                                       |
