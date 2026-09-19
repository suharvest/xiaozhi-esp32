#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// =============================================================
// Seeed Studio ReSpeaker Flex (XIAO ESP32S3 + XVF3800 板)
// =============================================================
// 音频链路：XVF3800 DSP 做降噪/AEC/波束成形，处理后的 PCM 通过 I2S 送给
// ESP32-S3；ESP32-S3 的播放数据同样经 I2S 回给 XVF3800。所以输入输出都是
// I2S，不是 PDM。

// --- 板载指示灯与按键 ---
#define BUILTIN_LED_GPIO          GPIO_NUM_21
#define BOOT_BUTTON_GPIO          GPIO_NUM_0
#define USER_BUTTON_GPIO          GPIO_NUM_NC
#define TOUCH_BUTTON_GPIO         GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO     GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO   GPIO_NUM_NC
#define RESET_NVS_BUTTON_GPIO     GPIO_NUM_NC
#define RESET_FACTORY_BUTTON_GPIO GPIO_NUM_NC

// --- 音频 ---
#define AUDIO_INPUT_I2S
#define AUDIO_OUTPUT_I2S

// XVF3800 固件工作在 16kHz；输入输出保持同一采样率，便于它做 AEC。
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// I2S 引脚（XIAO 排针位号见注释）
#define AUDIO_I2S_BCK_IO GPIO_NUM_8   // BCLK,  XIAO D9
#define AUDIO_I2S_WS_IO  GPIO_NUM_7   // LRCK,  XIAO D8
#define AUDIO_I2S_DO_IO  GPIO_NUM_44  // ESP32 -> XVF3800，占用 UART0 RX
#define AUDIO_I2S_DI_IO  GPIO_NUM_43  // XVF3800 -> ESP32，占用 UART0 TX

// XVF3800 输出 32 位 slot。NoAudioCodecDuplex 已固定按 32bit slot 配置
// （main/audio/codecs/no_audio_codec.cc），这里只作记录，代码不读取该宏。
#define AUDIO_I2S_BITS_PER_SAMPLE 32

// --- I2C（XVF3800 控制口，和可选的 SSD1306 共用一条总线）---
#define I2C_SDA_PIN      GPIO_NUM_5   // XIAO D4
#define I2C_SCL_PIN      GPIO_NUM_6   // XIAO D5
#define XVF3800_I2C_ADDR 0x2C

// --- 可选 OLED（ReSpeaker Flex 默认不贴，探测不到时降级为 NoDisplay）---
#define DISPLAY_SDA_PIN  I2C_SDA_PIN
#define DISPLAY_SCL_PIN  I2C_SCL_PIN
#define DISPLAY_WIDTH    128
#define DISPLAY_HEIGHT   64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// --- 摄像头未接线 ---
#define CAMERA_PIN_PWDN  -1
#define CAMERA_PIN_RESET -1
#define CAMERA_PIN_XCLK  -1
#define CAMERA_PIN_SIOD  -1
#define CAMERA_PIN_SIOC  -1
#define CAMERA_PIN_D7    -1
#define CAMERA_PIN_D6    -1
#define CAMERA_PIN_D5    -1
#define CAMERA_PIN_D4    -1
#define CAMERA_PIN_D3    -1
#define CAMERA_PIN_D2    -1
#define CAMERA_PIN_D1    -1
#define CAMERA_PIN_D0    -1
#define CAMERA_PIN_VSYNC -1
#define CAMERA_PIN_HREF  -1
#define CAMERA_PIN_PCLK  -1

#endif  // _BOARD_CONFIG_H_
