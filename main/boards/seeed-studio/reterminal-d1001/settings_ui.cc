#include "settings_ui.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>

#include <algorithm>
#include <cstdio>

#define TAG "SettingsUI"

namespace {

constexpr int kMaxScanResults = 20;
constexpr int kHeaderHeight = 72;
constexpr int kButtonHeight = 64;

// Argument block handed to the detached scan task.
struct ScanTaskArgs {
    SettingsUi* ui;
    uint32_t generation;
};

const char* SignalBars(int8_t rssi) {
    if (rssi >= -55) {
        return "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88";  // ████
    }
    if (rssi >= -65) {
        return "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88";
    }
    if (rssi >= -75) {
        return "\xe2\x96\x88\xe2\x96\x88";
    }
    return "\xe2\x96\x88";
}

}  // namespace

SettingsUi::SettingsUi(LcdDisplay* display, ConnectCallback connect_cb, CloseCallback close_cb)
    : display_(display), connect_cb_(std::move(connect_cb)), close_cb_(std::move(close_cb)) {}

SettingsUi::~SettingsUi() {
    lv_async_call_cancel(AsyncThunk, this);
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
}

void SettingsUi::EventThunk(lv_event_t* event) {
    auto* ctx = static_cast<EventCtx*>(lv_event_get_user_data(event));
    if (ctx == nullptr || ctx->ui == nullptr) {
        return;
    }
    SettingsUi* ui = ctx->ui;
    // Page transitions delete the object that is currently dispatching this
    // event, so run them from the async queue instead of inside the callback.
    ui->pending_actions_.push_back({ctx->action, ctx->index});
    if (!ui->async_scheduled_) {
        ui->async_scheduled_ = true;
        lv_async_call(AsyncThunk, ui);
    }
}

void SettingsUi::AsyncThunk(void* arg) {
    static_cast<SettingsUi*>(arg)->DrainPendingActions();
}

void SettingsUi::DrainPendingActions() {
    async_scheduled_ = false;
    std::vector<std::pair<Action, int>> actions;
    actions.swap(pending_actions_);
    for (const auto& action : actions) {
        HandleAction(action.first, action.second);
        if (root_ == nullptr) {
            break;
        }
    }
}

void SettingsUi::Bind(lv_obj_t* obj, Action action, int index) {
    event_ctx_.push_back(std::unique_ptr<EventCtx>(new EventCtx{this, action, index}));
    lv_obj_add_event_cb(obj, EventThunk, LV_EVENT_CLICKED, event_ctx_.back().get());
}

void SettingsUi::Open() {
    if (root_ != nullptr) {
        return;
    }
    ESP_LOGI(TAG, "Opening settings overlay");

    lv_obj_t* screen = lv_screen_active();
    lv_color_t bg = lv_obj_get_style_bg_color(screen, LV_PART_MAIN);

    // Parented to the screen so every label inherits the theme font. The theme
    // can be reloaded at runtime, which frees the previous font, so no font
    // pointer is ever cached here.
    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, bg, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(root_);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* header = lv_obj_create(root_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_hor(header, 12, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_set_size(back, 120, 56);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "返回");
    lv_obj_center(back_label);
    Bind(back, Action::Close);

    title_label_ = lv_label_create(header);
    lv_obj_set_style_pad_left(title_label_, 16, 0);
    lv_label_set_text(title_label_, "设置");

    page_ = SettingsPage::Home;
    ShowHome();
}

void SettingsUi::Close() {
    if (root_ == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "Closing settings overlay");
    scan_generation_++;
    pending_actions_.clear();
    lv_obj_delete(root_);
    root_ = nullptr;
    title_label_ = nullptr;
    body_ = nullptr;
    textarea_ = nullptr;
    keyboard_ = nullptr;
    event_ctx_.clear();
    if (close_cb_) {
        close_cb_();
    }
}

void SettingsUi::SetTitle(const char* title) {
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, title);
    }
}

void SettingsUi::ClearBody() {
    if (body_ != nullptr) {
        lv_obj_delete(body_);
        body_ = nullptr;
    }
    textarea_ = nullptr;
    keyboard_ = nullptr;
    // The event contexts belong to the objects that were just deleted; the back
    // button in the header keeps the first entry alive.
    if (event_ctx_.size() > 1) {
        event_ctx_.erase(event_ctx_.begin() + 1, event_ctx_.end());
    }
}

lv_obj_t* SettingsUi::MakeBody() {
    ClearBody();
    body_ = lv_obj_create(root_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_size(body_, LV_PCT(100), LV_VER_RES - kHeaderHeight);
    lv_obj_align(body_, LV_ALIGN_TOP_MID, 0, kHeaderHeight);
    lv_obj_set_style_pad_all(body_, 16, 0);
    lv_obj_set_style_pad_row(body_, 12, 0);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_OFF);
    return body_;
}

lv_obj_t* SettingsUi::MakeButton(lv_obj_t* parent, const char* text, Action action, int index) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), kButtonHeight);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    Bind(button, action, index);
    return button;
}

void SettingsUi::ShowHome() {
    page_ = SettingsPage::Home;
    lv_obj_t* body = MakeBody();
    SetTitle("设置");
    MakeButton(body, "Wi-Fi 网络", Action::HomeWifi);
    MakeButton(body, "返回主界面", Action::Close);
}

void SettingsUi::ShowWifiPage() {
    page_ = SettingsPage::Wifi;
    wifi_state_ = WifiSettingsState::Scanning;
    lv_obj_t* body = MakeBody();
    SetTitle("Wi-Fi 网络");

    lv_obj_t* label = lv_label_create(body);
    lv_label_set_text(label, "正在扫描附近的 Wi-Fi ...");

    MakeButton(body, "返回", Action::BackHome);
    StartWifiScan();
}

void SettingsUi::StartWifiScan() {
    scan_generation_++;
    auto* args = new ScanTaskArgs{this, scan_generation_};
    ESP_LOGI(TAG, "Scanning (generation %u)", (unsigned)args->generation);

    BaseType_t ok = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<ScanTaskArgs> args(static_cast<ScanTaskArgs*>(arg));
            std::vector<WifiScanItem> results;

            wifi_scan_config_t scan_config = {};
            scan_config.show_hidden = false;
            scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_config.scan_time.active.min = 100;
            scan_config.scan_time.active.max = 300;

            esp_err_t err = esp_wifi_scan_start(&scan_config, true);
            if (err == ESP_OK) {
                uint16_t ap_count = 0;
                err = esp_wifi_scan_get_ap_num(&ap_count);
                if (err == ESP_OK && ap_count > 0) {
                    uint16_t wanted = std::min<uint16_t>(ap_count, kMaxScanResults);
                    auto* records = new wifi_ap_record_t[wanted];
                    err = esp_wifi_scan_get_ap_records(&wanted, records);
                    if (err == ESP_OK) {
                        for (uint16_t i = 0; i < wanted; i++) {
                            WifiScanItem item;
                            item.ssid = std::string(reinterpret_cast<char*>(records[i].ssid));
                            item.rssi = records[i].rssi;
                            item.encrypted = records[i].authmode != WIFI_AUTH_OPEN;
                            if (!item.ssid.empty()) {
                                results.push_back(item);
                            }
                        }
                    }
                    delete[] records;
                }
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Found %d networks", (int)results.size());
            }
            args->ui->OnScanComplete(std::move(results), err, args->generation);
            vTaskDelete(nullptr);
        },
        "d1001_scan", 4096, args, 3, nullptr);

    if (ok != pdPASS) {
        delete args;
        ESP_LOGE(TAG, "Failed to create the scan task");
    }
}

void SettingsUi::OnScanComplete(std::vector<WifiScanItem> results, esp_err_t error,
                                uint32_t generation) {
    DisplayLockGuard lock(display_);
    if (root_ == nullptr || generation != scan_generation_ ||
        wifi_state_ != WifiSettingsState::Scanning) {
        return;
    }
    scan_results_ = std::move(results);
    if (error != ESP_OK) {
        lv_obj_t* body = MakeBody();
        lv_obj_t* label = lv_label_create(body);
        lv_label_set_text_fmt(label, "扫描失败: %s", esp_err_to_name(error));
        MakeButton(body, "重新扫描", Action::WifiRescan);
        MakeButton(body, "已保存的网络", Action::WifiSavedList);
        MakeButton(body, "返回", Action::BackHome);
        return;
    }
    ShowWifiList();
}

void SettingsUi::ShowWifiList() {
    wifi_state_ = WifiSettingsState::SelectWifi;
    lv_obj_t* body = MakeBody();
    SetTitle("选择 Wi-Fi");

    if (scan_results_.empty()) {
        lv_obj_t* label = lv_label_create(body);
        lv_label_set_text(label, "没有扫描到 Wi-Fi 网络");
    } else {
        lv_obj_t* list = lv_list_create(body);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);
        for (size_t i = 0; i < scan_results_.size(); i++) {
            const auto& item = scan_results_[i];
            char text[96];
            snprintf(text, sizeof(text), "%s  %s %s", item.ssid.c_str(), SignalBars(item.rssi),
                     item.encrypted ? "\xe2\x97\x8f" : "");
            lv_obj_t* button = lv_list_add_button(list, nullptr, text);
            lv_obj_set_style_min_height(button, kButtonHeight, 0);
            Bind(button, Action::WifiPickScanned, (int)i);
        }
    }

    MakeButton(body, "重新扫描", Action::WifiRescan);
    MakeButton(body, "已保存的网络", Action::WifiSavedList);
    MakeButton(body, "返回", Action::BackHome);
}

void SettingsUi::ShowPasswordInput(const std::string& ssid, bool encrypted) {
    wifi_state_ = WifiSettingsState::InputPassword;
    pending_ssid_ = ssid;
    pending_encrypted_ = encrypted;

    lv_obj_t* body = MakeBody();
    SetTitle("输入密码");

    lv_obj_t* label = lv_label_create(body);
    lv_label_set_text_fmt(label, "连接到: %s", ssid.c_str());

    textarea_ = lv_textarea_create(body);
    lv_obj_set_width(textarea_, LV_PCT(100));
    lv_textarea_set_one_line(textarea_, true);
    lv_textarea_set_password_mode(textarea_, true);
    lv_textarea_set_max_length(textarea_, 64);
    lv_textarea_set_placeholder_text(textarea_, encrypted ? "Wi-Fi password" : "(open network)");

    lv_obj_t* row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), kButtonHeight);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t* show = lv_button_create(row);
    lv_obj_set_flex_grow(show, 1);
    lv_obj_set_height(show, kButtonHeight);
    lv_obj_t* show_label = lv_label_create(show);
    lv_label_set_text(show_label, "显示密码");
    lv_obj_center(show_label);
    Bind(show, Action::WifiTogglePassword);

    lv_obj_t* connect = lv_button_create(row);
    lv_obj_set_flex_grow(connect, 1);
    lv_obj_set_height(connect, kButtonHeight);
    lv_obj_t* connect_label = lv_label_create(connect);
    lv_label_set_text(connect_label, "连接");
    lv_obj_center(connect_label);
    Bind(connect, Action::WifiConnectConfirm);

    lv_obj_t* cancel = lv_button_create(row);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, kButtonHeight);
    lv_obj_t* cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);
    Bind(cancel, Action::WifiPasswordCancel);

    keyboard_ = lv_keyboard_create(body);
    lv_obj_set_width(keyboard_, LV_PCT(100));
    lv_obj_set_flex_grow(keyboard_, 1);
    lv_keyboard_set_textarea(keyboard_, textarea_);
}

void SettingsUi::ShowSavedList() {
    wifi_state_ = WifiSettingsState::SavedList;
    saved_ssids_.clear();
    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        saved_ssids_.push_back(item.ssid);
    }

    lv_obj_t* body = MakeBody();
    SetTitle("已保存的网络");

    if (saved_ssids_.empty()) {
        lv_obj_t* label = lv_label_create(body);
        lv_label_set_text(label, "还没有保存任何 Wi-Fi");
    } else {
        lv_obj_t* list = lv_list_create(body);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);
        for (size_t i = 0; i < saved_ssids_.size(); i++) {
            lv_obj_t* row = lv_obj_create(list);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), kButtonHeight);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row, 8, 0);

            lv_obj_t* connect = lv_button_create(row);
            lv_obj_set_flex_grow(connect, 3);
            lv_obj_set_height(connect, kButtonHeight - 8);
            lv_obj_t* connect_label = lv_label_create(connect);
            lv_label_set_text(connect_label, saved_ssids_[i].c_str());
            lv_obj_center(connect_label);
            Bind(connect, Action::WifiConnectSaved, (int)i);

            lv_obj_t* remove = lv_button_create(row);
            lv_obj_set_flex_grow(remove, 1);
            lv_obj_set_height(remove, kButtonHeight - 8);
            lv_obj_t* remove_label = lv_label_create(remove);
            lv_label_set_text(remove_label, "删除");
            lv_obj_center(remove_label);
            Bind(remove, Action::WifiDeleteSaved, (int)i);
        }
    }

    MakeButton(body, "返回", Action::WifiRescan);
}

void SettingsUi::ShowConnecting(const std::string& ssid) {
    wifi_state_ = WifiSettingsState::Connecting;
    lv_obj_t* body = MakeBody();
    SetTitle("连接中");
    lv_obj_t* label = lv_label_create(body);
    lv_label_set_text_fmt(label, "正在连接 %s ...", ssid.c_str());
    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, "请稍候，最长约 20 秒");
}

void SettingsUi::ShowResult(bool success, const char* message) {
    wifi_state_ = success ? WifiSettingsState::Success : WifiSettingsState::Failed;
    lv_obj_t* body = MakeBody();
    SetTitle(success ? "连接成功" : "连接失败");
    lv_obj_t* label = lv_label_create(body);
    lv_label_set_text(label, message);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    MakeButton(body, success ? "完成" : "返回列表", Action::WifiResultBack);
    MakeButton(body, "关闭设置", Action::Close);
}

void SettingsUi::OnConnectResult(bool success, const std::string& message) {
    DisplayLockGuard lock(display_);
    operation_active_ = false;
    if (root_ == nullptr || wifi_state_ != WifiSettingsState::Connecting) {
        return;
    }
    ESP_LOGI(TAG, "%s: %s", success ? "Success" : "Failed", message.c_str());
    ShowResult(success, message.c_str());
}

void SettingsUi::HandleAction(Action action, int index) {
    switch (action) {
        case Action::Close:
            if (operation_active_) {
                return;
            }
            Close();
            return;
        case Action::HomeWifi:
            ShowWifiPage();
            return;
        case Action::BackHome:
            scan_generation_++;
            ShowHome();
            return;
        case Action::WifiRescan:
            ShowWifiPage();
            return;
        case Action::WifiSavedList:
            ShowSavedList();
            return;
        case Action::WifiPickScanned:
            if (index >= 0 && index < (int)scan_results_.size()) {
                ShowPasswordInput(scan_results_[index].ssid, scan_results_[index].encrypted);
            }
            return;
        case Action::WifiTogglePassword:
            if (textarea_ != nullptr) {
                bool on = lv_textarea_get_password_mode(textarea_);
                lv_textarea_set_password_mode(textarea_, !on);
            }
            return;
        case Action::WifiPasswordCancel:
            ShowWifiList();
            return;
        case Action::WifiConnectConfirm: {
            if (operation_active_ || textarea_ == nullptr) {
                return;
            }
            std::string password = lv_textarea_get_text(textarea_);
            if (pending_encrypted_ && password.empty()) {
                return;
            }
            std::string ssid = pending_ssid_;
            operation_active_ = true;
            ShowConnecting(ssid);
            if (connect_cb_) {
                connect_cb_(ssid, password);
            }
            return;
        }
        case Action::WifiConnectSaved: {
            if (operation_active_ || index < 0) {
                return;
            }
            const auto& list = SsidManager::GetInstance().GetSsidList();
            if (index >= (int)list.size()) {
                return;
            }
            std::string ssid = list[index].ssid;
            std::string password = list[index].password;
            pending_ssid_ = ssid;
            pending_encrypted_ = !password.empty();
            operation_active_ = true;
            ShowConnecting(ssid);
            if (connect_cb_) {
                connect_cb_(ssid, password);
            }
            return;
        }
        case Action::WifiDeleteSaved:
            if (index >= 0 && index < (int)SsidManager::GetInstance().GetSsidList().size()) {
                ESP_LOGI(TAG, "Removing saved network at index %d", index);
                SsidManager::GetInstance().RemoveSsid(index);
            }
            ShowSavedList();
            return;
        case Action::WifiResultBack:
            ShowWifiPage();
            return;
    }
}
