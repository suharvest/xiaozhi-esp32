#ifndef RETERMINAL_D1001_SETTINGS_UI_H
#define RETERMINAL_D1001_SETTINGS_UI_H

#include "display/lcd_display.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class SettingsPage {
    Home,
    Wifi,
    Display,
};

enum class WifiSettingsState {
    Scanning,
    SelectWifi,
    InputPassword,
    SavedList,
    Connecting,
    Success,
    Failed,
};

// One entry per supported orientation. The LCD flags go to the panel / LVGL
// port, the touch flags to esp_lcd_touch. The table is kept in a single place
// so per-angle calibration only ever edits one function.
struct RotationProfile {
    int degrees;
    bool lcd_mirror_x;
    bool lcd_mirror_y;
    bool lcd_swap_xy;
    bool touch_swap_xy;
    bool touch_mirror_x;
    bool touch_mirror_y;
};

struct WifiScanItem {
    std::string ssid;
    int8_t rssi;
    bool encrypted;
};

class SettingsUi {
public:
    using ConnectCallback = std::function<void(const std::string& ssid,
                                               const std::string& password)>;
    using CloseCallback = std::function<void()>;

    SettingsUi(LcdDisplay* display, ConnectCallback connect_cb, CloseCallback close_cb);
    ~SettingsUi();

    // Must run inside the LVGL context (event callback) or while the display
    // lock is held.
    void Open();
    void Close();
    bool IsOpen() const { return root_ != nullptr; }

    // Called from worker tasks; both take the display lock themselves.
    void OnScanComplete(std::vector<WifiScanItem> results, esp_err_t error, uint32_t generation);
    void OnConnectResult(bool success, const std::string& message);

    // Screen rotation persisted in NVS (namespace "reterminal", key "rotation").
    static RotationProfile LoadRotationProfile();
    static RotationProfile MakeRotationProfile(int degrees);
    static bool SaveRotation(int degrees);

private:
    enum class Action {
        Close,
        HomeWifi,
        HomeDisplay,
        BackHome,
        WifiRescan,
        WifiSavedList,
        WifiPickScanned,
        WifiConnectConfirm,
        WifiPasswordCancel,
        WifiTogglePassword,
        WifiConnectSaved,
        WifiDeleteSaved,
        WifiResultBack,
        RotationSelect,
    };

    struct EventCtx {
        SettingsUi* ui;
        Action action;
        int index;
    };

    static void EventThunk(lv_event_t* event);
    static void AsyncThunk(void* arg);
    void DrainPendingActions();
    void HandleAction(Action action, int index);
    void Bind(lv_obj_t* obj, Action action, int index = 0);

    lv_obj_t* MakeBody();
    lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, Action action, int index = 0);
    void SetTitle(const char* title);
    void ClearBody();

    void ShowHome();
    void ShowWifiPage();
    void StartWifiScan();
    void ShowWifiList();
    void ShowPasswordInput(const std::string& ssid, bool encrypted);
    void ShowSavedList();
    void ShowConnecting(const std::string& ssid);
    void ShowResult(bool success, const char* message);
    void ShowDisplaySettings();
    void SelectRotation(int degrees);

    LcdDisplay* display_;
    ConnectCallback connect_cb_;
    CloseCallback close_cb_;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* textarea_ = nullptr;
    lv_obj_t* keyboard_ = nullptr;

    SettingsPage page_ = SettingsPage::Home;
    WifiSettingsState wifi_state_ = WifiSettingsState::Scanning;
    std::vector<WifiScanItem> scan_results_;
    std::vector<std::string> saved_ssids_;
    std::string pending_ssid_;
    bool pending_encrypted_ = true;
    bool operation_active_ = false;
    uint32_t scan_generation_ = 0;
    std::vector<std::unique_ptr<EventCtx>> event_ctx_;
    std::vector<std::pair<Action, int>> pending_actions_;
    bool async_scheduled_ = false;
};

#endif  // RETERMINAL_D1001_SETTINGS_UI_H
