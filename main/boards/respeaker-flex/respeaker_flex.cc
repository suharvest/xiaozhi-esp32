#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "ReSpeakerFlex"

// XVF3800 把处理后的 PCM 以 I2S 主从方式交给 ESP32-S3，音量由 XVF3800 自己
// 管理，因此这里屏蔽软件音量写入，避免上层再做一次数字衰减。
class ReSpeakerAudioCodec : public NoAudioCodecDuplex {
public:
    ReSpeakerAudioCodec()
        : NoAudioCodecDuplex(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_BCK_IO,
                             AUDIO_I2S_WS_IO, AUDIO_I2S_DO_IO, AUDIO_I2S_DI_IO) {
        ESP_LOGI(TAG, "ReSpeaker I2S codec ready (%d Hz, 32-bit slot)", AUDIO_INPUT_SAMPLE_RATE);
    }
};

class ReSpeakerFlexBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;

    // XVF3800 和可选的 SSD1306 挂在同一条 I2C 总线上，这里建一次，两边共用。
    void InitializeI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_PIN,
            .scl_io_num = I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            i2c_bus_ = nullptr;
        }
    }

    void ProbeXvf3800() {
        if (i2c_bus_ == nullptr) {
            return;
        }
        esp_err_t err = i2c_master_probe(i2c_bus_, XVF3800_I2C_ADDR, pdMS_TO_TICKS(200));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "XVF3800 detected at 0x%02X", XVF3800_I2C_ADDR);
        } else {
            ESP_LOGW(TAG, "XVF3800 not responding at 0x%02X: %s", XVF3800_I2C_ADDR,
                     esp_err_to_name(err));
        }
    }

    // ReSpeaker Flex 出厂不带 OLED；探测不到就退到 NoDisplay，不影响语音链路。
    void InitializeDisplay() {
        if (i2c_bus_ == nullptr ||
            i2c_master_probe(i2c_bus_, 0x3C, pdMS_TO_TICKS(200)) != ESP_OK) {
            ESP_LOGI(TAG, "No SSD1306 on the bus, running headless");
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.on_color_trans_done = nullptr;
        io_config.user_ctx = nullptr;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.scl_speed_hz = 400 * 1000;

        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &panel_io_) != ESP_OK) {
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        if (esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_) != ESP_OK) {
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SSD1306");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    ReSpeakerFlexBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        ProbeXvf3800();
        InitializeDisplay();
        InitializeButtons();
    }

#if CONFIG_SKIP_WIFI_PROVISIONING
    // This board talks to its host over USB, so there is nothing to provision.
    // Not calling the base StartNetwork() leaves WifiManager uninitialised, so
    // no station scan, no config AP, no DNS or web server, and no looping setup
    // prompt. All WifiManager getters guard on their internal handles, so the
    // status bar simply shows "no network".
    virtual void StartNetwork() override {
        ESP_LOGW(TAG, "Wi-Fi provisioning skipped, going straight to idle");
        auto& app = Application::GetInstance();
        // device_state_machine.cc has no Starting -> Idle edge; Activating is a
        // legal step out of Starting and has no handler of its own, so this
        // lands in Idle with a single state-changed event.
        app.SetDeviceState(kDeviceStateActivating);
        app.SetDeviceState(kDeviceStateIdle);
    }
#endif

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static ReSpeakerAudioCodec audio_codec;
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(ReSpeakerFlexBoard);
