#include "wifi_board.h"

#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp_lcd_touch_gsl3670.h"
#include "lcd_init_cmds.h"
#include "reterminal_d1001_audio_codec.h"
#include "reterminal_d1001_expander.h"

#include <driver/i2c_master.h>
#include <esp_lcd_jd9365.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#define TAG "ReTerminalD1001"

class ReTerminalD1001Board : public WifiBoard {
private:
    ReTerminalD1001Expander expander_;
    ReTerminalD1001AudioCodec* audio_codec_ = nullptr;
    Button boot_button_;
    LcdDisplay* display_ = nullptr;
    i2c_master_bus_handle_t touch_i2c_bus_ = nullptr;
    gsl3670_driver_config_t touch_driver_config_ = {};

    void EnableDsiPhyPower() {
        // The MIPI DSI PHY is fed by LDO3; without it the PHY stays in the
        // "no power" state and the DSI bus cannot be created.
        static esp_ldo_channel_handle_t phy_pwr_chan = nullptr;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = DISPLAY_MIPI_DSI_PHY_LDO_CHANNEL,
            .voltage_mv = DISPLAY_MIPI_DSI_PHY_LDO_VOLTAGE_MV,
        };
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan));
        ESP_LOGI(TAG, "MIPI DSI PHY powered on");
    }

    void InitializeMipiDisplay() {
        EnableDsiPhyPower();
        expander_.PowerUpDisplayRails();

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = nullptr;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = DISPLAY_MIPI_DSI_LANE_NUM,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = DISPLAY_MIPI_DSI_LANE_BITRATE_MBPS,
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &panel_io));

        esp_lcd_dpi_panel_config_t dpi_config = {
            .virtual_channel = 0,
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
            .dpi_clock_freq_mhz = DISPLAY_DPI_CLOCK_MHZ,
            .in_color_format = LCD_COLOR_FMT_RGB565,
            .out_color_format = LCD_COLOR_FMT_RGB565,
            .num_fbs = 1,
            .video_timing =
                {
                    .h_size = DISPLAY_WIDTH,
                    .v_size = DISPLAY_HEIGHT,
                    .hsync_pulse_width = DISPLAY_HSYNC_PULSE_WIDTH,
                    .hsync_back_porch = DISPLAY_HSYNC_BACK_PORCH,
                    .hsync_front_porch = DISPLAY_HSYNC_FRONT_PORCH,
                    .vsync_pulse_width = DISPLAY_VSYNC_PULSE_WIDTH,
                    .vsync_back_porch = DISPLAY_VSYNC_BACK_PORCH,
                    .vsync_front_porch = DISPLAY_VSYNC_FRONT_PORCH,
                },
        };

        jd9365_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
            .mipi_config =
                {
                    .dsi_bus = mipi_dsi_bus,
                    .dpi_config = &dpi_config,
                    .lane_num = DISPLAY_MIPI_DSI_LANE_NUM,
                },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .reset_gpio_num = GPIO_NUM_NC,  // reset is behind the PCA9535
            .vendor_config = &vendor_config,
        };

        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_jd9365(panel_io, &panel_config, &panel));

        // The panel reset line hangs off the expander, so the driver cannot
        // pulse it itself: do it here before the init sequence is sent.
        expander_.ResetLcdPanel();

        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new MipiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                      DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "Display initialized");
    }

    void InitializeTouch() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = TOUCH_I2C_PORT,
            .sda_io_num = TOUCH_I2C_SDA_PIN,
            .scl_io_num = TOUCH_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        esp_err_t ret = i2c_new_master_bus(&bus_config, &touch_i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create the touch I2C bus: %s", esp_err_to_name(ret));
            return;
        }

        esp_lcd_panel_io_handle_t touch_io = nullptr;
        esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_GSL3670_CONFIG();
        touch_io_config.scl_speed_hz = TOUCH_I2C_FREQ_HZ;
        ret = esp_lcd_new_panel_io_i2c(touch_i2c_bus_, &touch_io_config, &touch_io);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create the GSL3670 panel IO: %s", esp_err_to_name(ret));
            return;
        }

        // The touch reset line is on the PCA9535, so hand the driver a hook
        // instead of a GPIO number.
        touch_driver_config_.ctx = &expander_;
        touch_driver_config_.set_reset = [](void* ctx, bool asserted) {
            static_cast<ReTerminalD1001Expander*>(ctx)->SetTouchReset(asserted);
        };

        esp_lcd_touch_config_t touch_config = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,  // the BSP polls the controller
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = TOUCH_SWAP_XY,
                    .mirror_x = TOUCH_MIRROR_X,
                    .mirror_y = TOUCH_MIRROR_Y,
                },
            .driver_data = &touch_driver_config_,
        };

        esp_lcd_touch_handle_t touch = nullptr;
        ret = esp_lcd_touch_new_i2c_gsl3670(touch_io, &touch_config, &touch);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize the GSL3670: %s", esp_err_to_name(ret));
            return;
        }

        const lvgl_port_touch_cfg_t lvgl_touch_config = {
            .disp = lv_display_get_default(),
            .handle = touch,
        };
        if (lvgl_port_add_touch(&lvgl_touch_config) == nullptr) {
            ESP_LOGE(TAG, "Failed to register the touch panel with LVGL");
            return;
        }
        ESP_LOGI(TAG, "Touch panel initialized");
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
    ReTerminalD1001Board() : boot_button_(BOOT_BUTTON_GPIO) {
        ESP_LOGI(TAG, "initializing reTerminal D1001");

        // Minimal power bring-up: I2C1 + PCA9535 and the system power hold.
        // The power amplifier stays off until the audio path is ready
        // (pop-free order) and the panel rails come up with the display.
        expander_.Initialize();
        expander_.ApplyMinimalPowerSequence();

        audio_codec_ = new ReTerminalD1001AudioCodec(expander_.GetI2cBus(), AUDIO_INPUT_SAMPLE_RATE,
                                                     AUDIO_OUTPUT_SAMPLE_RATE);
        audio_codec_->SetPowerAmpCallback([this](bool on) { expander_.SetPowerAmp(on); });

        InitializeMipiDisplay();
        InitializeTouch();
        InitializeButtons();

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override { return audio_codec_; }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(ReTerminalD1001Board);
