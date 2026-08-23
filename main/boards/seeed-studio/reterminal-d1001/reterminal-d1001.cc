#include "wifi_board.h"

#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp_lcd_touch_gsl3670.h"
#include "lcd_init_cmds.h"
#include "reterminal_d1001_audio_codec.h"
#include "reterminal_d1001_expander.h"
#include "settings_ui.h"

#include <driver/i2c_master.h>
#include <esp_lcd_jd9365.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <memory>
#include <string>

#define TAG "ReTerminalD1001"

// Display subclass that keeps the stock layout and only adds the settings
// entry button on top of it.
class ReTerminalD1001Display final : public MipiLcdDisplay {
public:
    using OpenSettingsCallback = std::function<void()>;

    ReTerminalD1001Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x,
                           bool mirror_y, bool swap_xy)
        : MipiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                         swap_xy) {}

    void SetOpenSettingsCallback(OpenSettingsCallback callback) {
        open_settings_ = std::move(callback);
    }

    void SetOnThemeChanged(std::function<void()> callback) {
        on_theme_changed_ = std::move(callback);
    }

    // The theme owns the fonts and frees them when it is reloaded, so they are
    // always read back from the theme and never cached.
    const lv_font_t* GetIconFont(bool large) const {
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        if (theme == nullptr) {
            return nullptr;
        }
        auto font = large ? theme->large_icon_font() : theme->icon_font();
        return font == nullptr ? nullptr : font->font();
    }

    void SetupUI() override {
        MipiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        // The status bar owns the top strip, so the entry sits just below it in
        // the top-right corner. It is parented to the active screen (not the
        // top layer) so the theme's text color and font reach it.
        settings_button_ = lv_button_create(lv_screen_active());
        lv_obj_set_size(settings_button_, kSettingsButtonSize, kSettingsButtonSize);
        lv_obj_align(settings_button_, LV_ALIGN_TOP_RIGHT, -12, 56);
        lv_obj_set_style_radius(settings_button_, kSettingsButtonSize / 2, 0);
        lv_obj_set_style_bg_opa(settings_button_, LV_OPA_40, 0);
        lv_obj_set_style_bg_opa(settings_button_, LV_OPA_80, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(settings_button_, 0, 0);
        lv_obj_set_style_pad_all(settings_button_, 0, 0);
        lv_obj_add_event_cb(settings_button_, OnSettingsClicked, LV_EVENT_CLICKED, this);

        settings_icon_ = lv_label_create(settings_button_);
        const lv_font_t* icon_font = GetIconFont(false);
        if (icon_font != nullptr) {
            lv_obj_set_style_text_font(settings_icon_, icon_font, 0);
        }
        lv_label_set_text(settings_icon_, MATERIAL_SYMBOLS_SETTINGS);
        lv_obj_center(settings_icon_);
    }

    void SetTheme(Theme* theme) override {
        MipiLcdDisplay::SetTheme(theme);

        DisplayLockGuard lock(this);
        if (settings_icon_ != nullptr) {
            const lv_font_t* icon_font = GetIconFont(false);
            if (icon_font != nullptr) {
                lv_obj_set_style_text_font(settings_icon_, icon_font, 0);
            }
        }
        if (on_theme_changed_) {
            on_theme_changed_();
        }
    }

    // Software rotation for 90/270; 180 is done with the panel mirror flags so
    // no per-frame rotation cost is paid for it.
    void ApplyRotation(int degrees) {
        lv_display_t* disp = lv_display_get_default();
        if (disp == nullptr) {
            return;
        }
        lv_display_rotation_t rotation = LV_DISPLAY_ROTATION_0;
        if (degrees == 90) {
            rotation = LV_DISPLAY_ROTATION_90;
        } else if (degrees == 270) {
            rotation = LV_DISPLAY_ROTATION_270;
        }
        if (rotation != LV_DISPLAY_ROTATION_0) {
            DisplayLockGuard lock(this);
            lv_display_set_rotation(disp, rotation);
        }
    }

    void SetSettingsButtonHidden(bool hidden) {
        if (settings_button_ == nullptr) {
            return;
        }
        if (hidden) {
            lv_obj_add_flag(settings_button_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(settings_button_, LV_OBJ_FLAG_HIDDEN);
        }
    }

private:
    static constexpr int kSettingsButtonSize = 56;

    static void OnSettingsClicked(lv_event_t* event) {
        auto* self = static_cast<ReTerminalD1001Display*>(lv_event_get_user_data(event));
        if (self != nullptr && self->open_settings_) {
            self->open_settings_();
        }
    }

    OpenSettingsCallback open_settings_;
    std::function<void()> on_theme_changed_;
    lv_obj_t* settings_button_ = nullptr;
    lv_obj_t* settings_icon_ = nullptr;
};

class ReTerminalD1001Board : public WifiBoard {
private:
    ReTerminalD1001Expander expander_;
    ReTerminalD1001AudioCodec* audio_codec_ = nullptr;
    Button boot_button_;
    ReTerminalD1001Display* display_ = nullptr;
    std::unique_ptr<SettingsUi> settings_ui_;
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

    void InitializeMipiDisplay(const RotationProfile& rotation) {
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
        // Field order differs between IDF 5.5 and 6.0, so assign instead of
        // using designated initializers (-Wpedantic rejects out-of-order ones).
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;  // reset is behind the PCA9535
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;

        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_jd9365(panel_io, &panel_config, &panel));

        // The panel reset line hangs off the expander, so the driver cannot
        // pulse it itself: do it here before the init sequence is sent.
        expander_.ResetLcdPanel();

        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new ReTerminalD1001Display(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                              DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                              rotation.lcd_mirror_x, rotation.lcd_mirror_y,
                                              rotation.lcd_swap_xy);
        display_->SetOpenSettingsCallback([this]() { OpenSettings(); });
        display_->ApplyRotation(rotation.degrees);
        ESP_LOGI(TAG, "Display initialized, rotation=%d", rotation.degrees);
    }

    void InitializeTouch(const RotationProfile& rotation) {
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
                    // x_max/y_max stay at the native panel size: the driver
                    // mirrors raw coordinates before the optional swap.
                    .swap_xy = rotation.touch_swap_xy,
                    .mirror_x = rotation.touch_mirror_x,
                    .mirror_y = rotation.touch_mirror_y,
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

    void OpenSettings() {
        if (settings_ui_ == nullptr) {
            settings_ui_.reset(new SettingsUi(
                display_,
                [this](const std::string& ssid, const std::string& password) {
                    ConnectFromSettings(ssid, password);
                },
                [this]() { display_->SetSettingsButtonHidden(false); }));
            settings_ui_->SetIconFontProvider(
                [this](bool large) { return display_->GetIconFont(large); });
            display_->SetOnThemeChanged([this]() {
                if (settings_ui_ != nullptr) {
                    settings_ui_->RefreshIconFonts();
                }
            });
        }
        if (settings_ui_->IsOpen()) {
            return;
        }
        display_->SetSettingsButtonHidden(true);
        settings_ui_->Open();
    }

    // Runs on a worker task: swap the credentials in, restart the station and
    // wait for the link. On failure the previous credentials are restored.
    void ConnectFromSettings(const std::string& ssid, const std::string& password) {
        auto* request = new ConnectRequest{this, ssid, password};
        BaseType_t ok = xTaskCreate(
            [](void* arg) {
                std::unique_ptr<ConnectRequest> request(static_cast<ConnectRequest*>(arg));
                request->board->RunConnect(*request);
                vTaskDelete(nullptr);
            },
            "d1001_wifi_set", 4096, request, 3, nullptr);
        if (ok != pdPASS) {
            delete request;
            settings_ui_->OnConnectResult(false, "无法启动连接任务");
        }
    }

    struct ConnectRequest {
        ReTerminalD1001Board* board;
        std::string ssid;
        std::string password;
    };

    void RunConnect(const ConnectRequest& request) {
        auto& ssid_manager = SsidManager::GetInstance();

        // Snapshot the previous entry so a failed attempt can be rolled back.
        bool existed = false;
        std::string old_password;
        for (const auto& item : ssid_manager.GetSsidList()) {
            if (item.ssid == request.ssid) {
                existed = true;
                old_password = item.password;
                break;
            }
        }
        ssid_manager.AddSsid(request.ssid, request.password);

        auto& wifi_manager = WifiManager::GetInstance();
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle || state == kDeviceStateListening ||
            state == kDeviceStateSpeaking) {
            // Tear the protocol down before the link is taken away.
            EnterWifiConfigMode();
            for (int i = 0; i < 60 && !IsInWifiConfigMode(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        ESP_LOGI(TAG, "Connecting to %s from the settings UI", request.ssid.c_str());
        wifi_manager.StartStation();

        bool success = false;
        for (int i = 0; i < 200; i++) {
            if (wifi_manager.IsConnected() && wifi_manager.GetSsid() == request.ssid) {
                success = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        std::string message;
        if (success) {
            message = "已连接到 " + request.ssid + "\nIP: " + wifi_manager.GetIpAddress();
        } else {
            message = "无法连接到 " + request.ssid + "，请检查密码后重试";
            // Roll the credentials back so a wrong password is not kept.
            const auto& list = ssid_manager.GetSsidList();
            for (size_t i = 0; i < list.size(); i++) {
                if (list[i].ssid == request.ssid) {
                    ssid_manager.RemoveSsid((int)i);
                    break;
                }
            }
            if (existed) {
                ssid_manager.AddSsid(request.ssid, old_password);
            }
        }
        settings_ui_->OnConnectResult(success, message);
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

        RotationProfile rotation = SettingsUi::LoadRotationProfile();
        ESP_LOGI(TAG, "Screen rotation=%d", rotation.degrees);

        InitializeMipiDisplay(rotation);
        InitializeTouch(rotation);
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
