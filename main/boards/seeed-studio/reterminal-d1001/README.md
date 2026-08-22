# Seeed Studio reTerminal D1001

This board definition is currently a skeleton only. It registers an independent
ESP32-P4 board and records the known hardware wiring, but does not yet initialize
power, audio, display, touch, or buttons.

The hardware facts come from the
[reTerminal D10xx product documentation](https://wiki.seeedstudio.com/reterminal_d10xx_main_page/)
and the
[official reTerminal-D1001 repository](https://github.com/Seeed-Studio/reTerminal-D1001).

Use ESP-IDF 6.0.2 and build the single variant with:

```sh
python3 scripts/build.py seeed-studio/reterminal-d1001 --name reterminal-d1001
```

The board configuration currently follows the product documentation and assumes
32 MB flash. Verify the physical flash ID and capacity before the first write;
stop if the detected capacity does not match. Back up the factory firmware and
partition table before flashing.

The ESP32-P4 application and the ESP32-C6 ESP-Hosted slave are separate firmware
images with separate version lifecycles. Building or updating this P4 image does
not build or silently update the C6 firmware; record and manage both versions
independently.