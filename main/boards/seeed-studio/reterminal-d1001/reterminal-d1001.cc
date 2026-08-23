#include "wifi_board.h"

#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "lcd_init_cmds.h"
#include "reterminal_d1001_audio_codec.h"
#include "reterminal_d1001_expander.h"

#include <esp_lcd_jd9365.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>

#define TAG "ReTerminalD1001"

class ReTerminalD1001Board : public WifiBoard {
private:
    ReTerminalD1001Expander expander_;
    ReTerminalD1001AudioCodec* audio_codec_ = nullptr;
    Button boot_button_;
    LcdDisplay* display_ = nullptr;

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
