#ifndef XIAOZHI_BOARDS_SEEED_STUDIO_RETERMINAL_D1001_CONFIG_H_
#define XIAOZHI_BOARDS_SEEED_STUDIO_RETERMINAL_D1001_CONFIG_H_

#include <driver/gpio.h>
#include <driver/i2c_types.h>

// Audio control bus (I2C1) and devices.
#define AUDIO_I2C_PORT I2C_NUM_1
#define AUDIO_I2C_SDA_PIN GPIO_NUM_20
#define AUDIO_I2C_SCL_PIN GPIO_NUM_21
#define PCA9535_I2C_ADDRESS 0x20
#define ES8311_I2C_ADDRESS 0x18
#define ES7210_I2C_ADDRESS 0x40
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// ES8311 playback bus (I2S TX).
#define AUDIO_TX_MCLK_PIN GPIO_NUM_33
#define AUDIO_TX_BCLK_PIN GPIO_NUM_32
#define AUDIO_TX_WS_PIN GPIO_NUM_31
#define AUDIO_TX_DOUT_PIN GPIO_NUM_30

// ES7210 capture bus (I2S RX/TDM).
#define AUDIO_RX_MCLK_PIN GPIO_NUM_29
#define AUDIO_RX_BCLK_PIN GPIO_NUM_28
#define AUDIO_RX_LRCK_PIN GPIO_NUM_27
#define AUDIO_RX_DIN_PIN GPIO_NUM_26

// PCA9535 (address 0x20) output bits, from the Seeed D1001 BSP pin table.
// The expander drives power and panel control signals; see
// reterminal_d1001_expander.{h,cc} for the wrappers that own these bits.
#define EXPANDER_BIT_LCD_PWR_EN 0        // EXP_PIN_NUM_0: display power enable
#define EXPANDER_BIT_LCD_BACKLIGHT_EN 7  // EXP_PIN_NUM_7: backlight power enable
#define EXPANDER_BIT_PWR_HOLD 8          // EXP_PIN_NUM_8: system power hold (vdd_3v3)
#define EXPANDER_BIT_LCD_RST 2           // EXP_PIN_NUM_2: LCD reset
#define EXPANDER_BIT_PA_ENABLE 11        // EXP_PIN_NUM_11: NS4150B power amp enable
#define EXPANDER_BIT_TOUCH_RST 12        // EXP_PIN_NUM_12: GSL3670 touch reset

// GSL3670 touch controller bus (I2C0) and interrupt.
#define TOUCH_I2C_PORT I2C_NUM_0
#define TOUCH_I2C_SDA_PIN GPIO_NUM_37
#define TOUCH_I2C_SCL_PIN GPIO_NUM_38
#define TOUCH_INTERRUPT_PIN GPIO_NUM_16

#define BOOT_BUTTON_GPIO GPIO_NUM_3

// JD9365DA-H3 panel and MIPI-DSI PHY facts.
#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 1280
#define DISPLAY_MIPI_DSI_LANE_NUM 2
#define DISPLAY_MIPI_DSI_PHY_LDO_CHANNEL 3
#define DISPLAY_MIPI_DSI_PHY_LDO_VOLTAGE_MV 2500

// ESP32-C6 ESP-Hosted SDIO connection.
#define ESP32C6_SDIO_CMD_PIN GPIO_NUM_6
#define ESP32C6_SDIO_CLK_PIN GPIO_NUM_11
#define ESP32C6_SDIO_D0_PIN GPIO_NUM_7
#define ESP32C6_SDIO_D1_PIN GPIO_NUM_8
#define ESP32C6_SDIO_D2_PIN GPIO_NUM_9
#define ESP32C6_SDIO_D3_PIN GPIO_NUM_10
#define ESP32C6_RESET_PIN GPIO_NUM_13

#endif  // XIAOZHI_BOARDS_SEEED_STUDIO_RETERMINAL_D1001_CONFIG_H_