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
- Idle stability: two 30 min runs with the display on, wake word off
  (free SRAM flat at 248371) and wake word on (free SRAM flat at 250531),
  no resets.

Not yet validated: device AEC, 30 min continuous conversation, touch
coordinate orientation on the panel (the BSP values swap_xy=0, mirror_x=1,
mirror_y=1 are used as-is).

### Wake word and internal RAM

The AFE wake word (`wn9_nihaoxiaozhi_tts`) runs on the ESP-IDF 6.0 build, but
only with `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`. Without it the
esp_hosted SDIO mempools (40 + 47 KB, allocated from an ESP_SYSTEM_INIT_FN)
sit in RETENT_RAM, the only internal heap available before the scheduler
starts, and two things go wrong on this board:

- once `.data/.bss` grow (e.g. the touch driver) the main task stack can no
  longer be allocated: `Mem alloc fail. size 0x2a00 caps 0x804` /
  `assert failed: esp_startup_start_app app_startup.c:83 (res == pdTRUE)`;
- with less static data the firmware boots, but the esp-sr prebuilt
  libraries (`esp32p4_less_v3_idf6`) hang the CPU right after the WakeNet
  pipeline is created (HP watchdog reset loop, `rst:0x7`), apparently
  because an internal-RAM allocation inside the closed library fails and the
  library busy-waits instead of reporting it.

Both symptoms disappear when the mempools live in PSRAM (GDMA reaches PSRAM
on the P4). The same sources built with ESP-IDF 5.5.4 need the same option
(there the mempool allocation itself asserts at boot) and, because 5.5
cannot address this flash chip above 16 MB, would also need
`partitions/v2/16m.csv`; only the IDF >= 6.0 entry is kept in `config.json`.

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

Verified with ESP-IDF 6.0.2 (`espressif/idf:v6.0.2`). The factory images are published in the Seeed BSP repository
under `firmware/`.

The ESP32-P4 application and the ESP32-C6 ESP-Hosted slave are separate
firmware images with separate version lifecycles. Building or updating this
P4 image does not build or silently update the C6 firmware; record and
manage both versions independently.

## On-screen settings

A round 56x56 gear button sits in the top-right corner of the main screen,
just below the status bar. Tapping it opens a full-screen overlay
(`settings_ui.h/.cc`); the main UI keeps running underneath and is restored
when the overlay closes. The GPIO3 button behaviour is unchanged.

Every page uses the same shell: an app bar (back arrow, title, optional
refresh) over a card list — 16 px radius, 16 px gaps, 96 px rows, 72 px
buttons, one accent colour (#2F6BFF) for primary actions. Icons come from the
board's existing Material Symbols font (`font_material_symbols_30_4`) through
`lvgl_theme`'s `icon_font()` / `large_icon_font()`; no font file is added. Card
colours are derived from the theme background, so light and dark both work, and
both the entry icon and every overlay icon are re-pointed at the new font when
the theme is reloaded (the old font is freed at that moment).

- **Wi-Fi 网络**: signal-strength icon per row, lock icon on encrypted
  networks, `手动输入` and `已保存` at the bottom; scans in a background task (`esp_wifi_scan_start`, up to 20
  results), lists SSID/signal/encryption, opens an on-screen keyboard for the
  password and connects. Saved networks can be reconnected or deleted. The new
  credentials are written through `SsidManager` before the attempt and rolled
  back if the connection does not come up within 20 s. Switching networks while
  the device is idle goes through `EnterWifiConfigMode()` first, so the protocol
  is torn down cleanly before the station restarts.
- **屏幕方向**: four large tiles with the current one highlighted, then
  `保存并重启` with a confirmation step. 0/90/180/270, stored in NVS (namespace `reterminal`, key
  `rotation`) and applied at boot; the device reboots 2 s after the choice is
  confirmed. 90/270 use `lv_display_set_rotation()` (the MIPI port runs with
  `sw_rotate`), 180 uses the panel mirror flags. Touch swap/mirror flags come
  from the same table in `SettingsUi::MakeRotationProfile()`.

Only the 0° row of that table is hardware-validated. The 90/180/270 rows are
candidates and still need a four-corner touch calibration pass on the device;
frame rate under software rotation at 800x1280 RGB565 has not been measured
either. Both are edited in one place: `MakeRotationProfile()`.

The board build appends `CONFIG_LV_USE_KEYBOARD=y`, `CONFIG_LV_USE_LIST=y`,
`CONFIG_LV_USE_TEXTAREA=y` and `CONFIG_LV_USE_SPINNER=y`, which the project
defaults leave off.

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
