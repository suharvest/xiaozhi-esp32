#include "settings_ui.h"

#include "application.h"
#include "audio/audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <algorithm>
#include <cstdio>

#define TAG "SettingsUI"

namespace {

constexpr int kMaxScanResults = 20;
constexpr int kRotationChoices[] = {0, 90, 180, 270};

// Layout constants for the 800x1280 panel.
constexpr int kHeaderHeight = 96;
constexpr int kIconButtonSize = 64;
constexpr int kRowHeight = 96;
constexpr int kButtonHeight = 72;
constexpr int kCardRadius = 16;
constexpr int kSliderHeight = 56;  // fat enough to drag with a finger
constexpr int kKeyboardHeight = 360;  // four rows of ~84 px keys
constexpr int kGap = 16;

// Accent used for primary actions and the selected state. Fixed values so they
// read the same on the light and the dark theme, both of which the device can
// switch to at runtime.
constexpr uint32_t kAccentColor = 0x2F6BFF;
constexpr uint32_t kDangerColor = 0xD64545;
constexpr uint32_t kSuccessColor = 0x2E9E5B;


// --- Phone-style keyboard -------------------------------------------------
//
// LVGL's built-in keyboard handler dispatches on the exact button texts "abc",
// "ABC" and "1#" (lv_keyboard.c, lv_keyboard_def_event_cb), so the mode keys
// have to carry those labels; a "?123" or a shift arrow would be typed into
// the text area instead of switching the layout. The maps are static because
// lv_keyboard_set_map() stores the pointers.
//
// Widths are relative per row: letters 2, so a row of ten letters is 20. The
// second row is nine letters between two hidden half-width spacers, which is
// what gives it the phone-like indent on both ends.
#define KB_W2 (lv_buttonmatrix_ctrl_t)2
#define KB_FN(w) (lv_buttonmatrix_ctrl_t)((w) | LV_BUTTONMATRIX_CTRL_CHECKED)
#define KB_GAP \
    (lv_buttonmatrix_ctrl_t)(1 | LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED)

const char* const kKeyboardLowerMap[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", ",", " ", ".", LV_SYMBOL_OK, ""};

const lv_buttonmatrix_ctrl_t kKeyboardLowerCtrl[] = {
    KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2,
    KB_GAP, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_GAP,
    KB_FN(3), KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_FN(3),
    KB_FN(3), KB_W2, (lv_buttonmatrix_ctrl_t)10, KB_W2, KB_FN(3)};

const char* const kKeyboardUpperMap[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", ",", " ", ".", LV_SYMBOL_OK, ""};

const char* const kKeyboardSpecialMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    "abc", "-", "_", "+", "=", "/", "\\", ":", ";", LV_SYMBOL_BACKSPACE, "\n",
    "~", "<", ">", "[", "]", "{", "}", "\"", "'", "?", "\n",
    ",", " ", ".", LV_SYMBOL_OK, ""};

const lv_buttonmatrix_ctrl_t kKeyboardSpecialCtrl[] = {
    KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2,
    KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2,
    KB_FN(3), KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_FN(3),
    KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2, KB_W2,
    KB_W2, (lv_buttonmatrix_ctrl_t)10, KB_W2, KB_FN(3)};

struct ScanTaskArgs {
    SettingsUi* ui;
    uint32_t generation;
};

const char* SignalIcon(int8_t rssi) {
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    if (rssi >= -85) {
        return MATERIAL_SYMBOLS_WIFI_1_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_OFF;
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

// ---------------------------------------------------------------------------
// Event plumbing
// ---------------------------------------------------------------------------

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

void SettingsUi::Bind(lv_obj_t* obj, Action action, int index, lv_event_code_t code) {
    event_ctx_.push_back(std::unique_ptr<EventCtx>(new EventCtx{this, action, index}));
    lv_obj_add_event_cb(obj, EventThunk, code, event_ctx_.back().get());
}

// The slider writes NVS through AudioCodec::SetOutputVolume(), which persists on
// every call, so dragging only previews the value and the codec is written once
// when the finger leaves the knob.
void SettingsUi::VolumeSliderThunk(lv_event_t* event) {
    auto* ui = static_cast<SettingsUi*>(lv_event_get_user_data(event));
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (ui == nullptr || slider == nullptr) {
        return;
    }
    int value = (int)lv_slider_get_value(slider);
    if (ui->volume_value_ != nullptr) {
        lv_label_set_text_fmt(ui->volume_value_, "%d%%", value);
    }
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    if (value > 0) {
        ui->volume_restore_ = value;
    }
    codec->SetOutputVolume(value);
}

// ---------------------------------------------------------------------------
// Styling helpers
// ---------------------------------------------------------------------------

const lv_font_t* SettingsUi::IconFont(bool large) const {
    if (icon_font_provider_) {
        return icon_font_provider_(large);
    }
    return nullptr;
}

lv_color_t SettingsUi::CardColor() const {
    // Derived from the screen background so cards follow the active theme:
    // lifted on a dark background, darkened on a light one.
    lv_color_t bg = lv_obj_get_style_bg_color(lv_screen_active(), LV_PART_MAIN);
    if (lv_color_brightness(bg) > 128) {
        return lv_color_darken(bg, 24);
    }
    return lv_color_lighten(bg, 40);
}

lv_obj_t* SettingsUi::MakeIconLabel(lv_obj_t* parent, const char* glyph, bool large) {
    lv_obj_t* label = lv_label_create(parent);
    const lv_font_t* font = IconFont(large);
    if (font != nullptr) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_label_set_text(label, glyph);
    icon_labels_.push_back({label, large});
    return label;
}

void SettingsUi::RefreshIconFonts() {
    for (const auto& entry : icon_labels_) {
        const lv_font_t* font = IconFont(entry.second);
        if (font != nullptr) {
            lv_obj_set_style_text_font(entry.first, font, 0);
        }
    }
}

void SettingsUi::ClearPage() {
    if (body_ != nullptr) {
        lv_obj_delete(body_);
        body_ = nullptr;
    }
    if (header_ != nullptr) {
        lv_obj_delete(header_);
        header_ = nullptr;
    }
    textarea_ = nullptr;
    keyboard_ = nullptr;
    volume_slider_ = nullptr;
    volume_value_ = nullptr;
    icon_labels_.clear();
    event_ctx_.clear();
}

lv_obj_t* SettingsUi::BuildPage(const char* title, Action back_action, bool with_refresh) {
    ClearPage();

    header_ = lv_obj_create(root_);
    lv_obj_remove_style_all(header_);
    lv_obj_set_size(header_, LV_PCT(100), kHeaderHeight);
    lv_obj_align(header_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_hor(header_, kGap, 0);
    lv_obj_set_style_pad_column(header_, kGap, 0);
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(header_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* back = lv_button_create(header_);
    lv_obj_set_size(back, kIconButtonSize, kIconButtonSize);
    lv_obj_set_style_radius(back, kIconButtonSize / 2, 0);
    lv_obj_set_style_bg_color(back, CardColor(), 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_center(MakeIconLabel(back, MATERIAL_SYMBOLS_ARROW_BACK, false));
    Bind(back, back_action);

    lv_obj_t* title_label = lv_label_create(header_);
    lv_label_set_text(title_label, title);
    lv_obj_set_flex_grow(title_label, 1);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

    if (with_refresh) {
        lv_obj_t* refresh = lv_button_create(header_);
        lv_obj_set_size(refresh, kIconButtonSize, kIconButtonSize);
        lv_obj_set_style_radius(refresh, kIconButtonSize / 2, 0);
        lv_obj_set_style_bg_color(refresh, CardColor(), 0);
        lv_obj_set_style_shadow_width(refresh, 0, 0);
        lv_obj_center(MakeIconLabel(refresh, MATERIAL_SYMBOLS_REFRESH, false));
        Bind(refresh, Action::WifiRescan);
    }

    body_ = lv_obj_create(root_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_size(body_, LV_PCT(100), LV_VER_RES - kHeaderHeight);
    lv_obj_align(body_, LV_ALIGN_TOP_MID, 0, kHeaderHeight);
    lv_obj_set_style_pad_all(body_, kGap, 0);
    lv_obj_set_style_pad_row(body_, kGap, 0);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_OFF);
    return body_;
}

lv_obj_t* SettingsUi::MakeScrollArea(lv_obj_t* parent) {
    lv_obj_t* area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_set_width(area, LV_PCT(100));
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(area, 12, 0);
    lv_obj_set_scroll_dir(area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_AUTO);
    return area;
}

lv_obj_t* SettingsUi::MakeKeyboard(lv_obj_t* parent, lv_obj_t* textarea, Action ready_action) {
    // Push the keyboard to the bottom of the flex column instead of letting it
    // stretch over the whole remaining height (the default stretched every key
    // to ~200 px with the 14 px default font).
    lv_obj_t* spacer = lv_obj_create(parent);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* kb = lv_keyboard_create(parent);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_height(kb, kKeyboardHeight);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(kb, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(kb, 8, LV_PART_MAIN);

    // Key caps: theme text font (the keyboard otherwise uses the 14 px LVGL
    // default), rounded, card colour. Function keys are marked CHECKED in the
    // ctrl maps and get one shade more contrast instead of the accent, which
    // used to paint a block of blue over the whole left column and the bottom
    // row. Pressing any key tints it towards the accent.
    const lv_font_t* font = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
    if (font != nullptr) {
        lv_obj_set_style_text_font(kb, font, LV_PART_ITEMS);
    }
    const lv_color_t card = CardColor();
    const lv_color_t function_key = lv_color_brightness(card) > 128
                                        ? lv_color_darken(card, 32)
                                        : lv_color_lighten(card, 48);
    const lv_color_t pressed = lv_color_mix(lv_color_hex(kAccentColor), card, 90);
    lv_obj_set_style_radius(kb, 12, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, card, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, function_key,
                              (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, pressed,
                              (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, pressed,
                              (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_PRESSED |
                                  (lv_style_selector_t)LV_STATE_CHECKED);

    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kKeyboardLowerMap, kKeyboardLowerCtrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kKeyboardUpperMap, kKeyboardLowerCtrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kKeyboardSpecialMap, kKeyboardSpecialCtrl);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);

    lv_keyboard_set_textarea(kb, textarea);
    // The OK key runs the page's primary action instead of only dismissing the
    // keyboard, so a password can be submitted without reaching for 连接.
    Bind(kb, ready_action, 0, LV_EVENT_READY);
    return kb;
}

lv_obj_t* SettingsUi::MakeCard(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, kRowHeight);
    lv_obj_set_style_bg_color(card, CardColor(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, kCardRadius, 0);
    lv_obj_set_style_pad_hor(card, 20, 0);
    lv_obj_set_style_pad_column(card, 20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    return card;
}

lv_obj_t* SettingsUi::MakeListItem(lv_obj_t* parent, const char* icon, const char* title,
                                   const char* subtitle, Action action, int index,
                                   const char* trailing_icon) {
    lv_obj_t* card = MakeCard(parent);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    Bind(card, action, index);

    if (icon != nullptr) {
        MakeIconLabel(card, icon, true);
    }

    lv_obj_t* texts = lv_obj_create(card);
    lv_obj_remove_style_all(texts);
    // lv_obj is clickable by default and would swallow the tap meant for the
    // card, so the card's CLICKED callback never fired on the text area.
    lv_obj_remove_flag(texts, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(texts, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* title_label = lv_label_create(texts);
    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

    if (subtitle != nullptr && subtitle[0] != '\0') {
        lv_obj_t* subtitle_label = lv_label_create(texts);
        lv_label_set_text(subtitle_label, subtitle);
        lv_obj_set_width(subtitle_label, LV_PCT(100));
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_opa(subtitle_label, LV_OPA_60, 0);
    }

    if (trailing_icon != nullptr) {
        MakeIconLabel(card, trailing_icon, false);
    }
    return card;
}

lv_obj_t* SettingsUi::MakeTextButton(lv_obj_t* parent, const char* icon, const char* text,
                                     Action action, int index, bool primary) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, kButtonHeight);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, primary ? lv_color_hex(kAccentColor) : CardColor(), 0);
    if (primary) {
        lv_obj_set_style_text_color(button, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(button, 12, 0);
    if (icon != nullptr) {
        MakeIconLabel(button, icon, false);
    }
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    Bind(button, action, index);
    return button;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SettingsUi::Open(SettingsPage page) {
    if (root_ != nullptr) {
        return;
    }
    pending_face_mode_ = -1;
    ESP_LOGI(TAG, "Opening settings overlay on page %d", static_cast<int>(page));

    lv_obj_t* screen = lv_screen_active();
    // Parented to the screen so every label inherits the theme text font. The
    // theme can be reloaded at runtime, which frees the previous font, so no
    // text font pointer is ever cached here.
    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, lv_obj_get_style_bg_color(screen, LV_PART_MAIN), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_foreground(root_);

    wifi_state_ = WifiSettingsState::Idle;
    switch (page) {
        case SettingsPage::Display:
            ShowDisplaySettings();
            break;
        case SettingsPage::Volume:
            ShowVolumeSettings();
            break;
        case SettingsPage::Face:
            ShowFacePage();
            break;
        case SettingsPage::Wifi:
        default:
            ShowWifiPage();
            break;
    }
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
    header_ = nullptr;
    body_ = nullptr;
    textarea_ = nullptr;
    keyboard_ = nullptr;
    volume_slider_ = nullptr;
    volume_value_ = nullptr;
    icon_labels_.clear();
    event_ctx_.clear();
    if (close_cb_) {
        close_cb_();
    }
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

void SettingsUi::ShowWifiPage() {
    page_ = SettingsPage::Wifi;
    wifi_state_ = WifiSettingsState::Scanning;
    lv_obj_t* body = BuildPage("Wi-Fi 网络", Action::Close, true);

    lv_obj_t* card = MakeCard(body);
    MakeIconLabel(card, MATERIAL_SYMBOLS_WIFI, true);
    lv_obj_t* label = lv_label_create(card);
    lv_label_set_text(label, "正在扫描附近的网络 ...");
    lv_obj_set_flex_grow(label, 1);

    StartWifiScan();
}

void SettingsUi::ShowWifiList() {
    wifi_state_ = WifiSettingsState::SelectWifi;
    lv_obj_t* body = BuildPage("Wi-Fi 网络", Action::Close, true);

    lv_obj_t* area = MakeScrollArea(body);
    if (scan_results_.empty()) {
        lv_obj_t* card = MakeCard(area);
        MakeIconLabel(card, MATERIAL_SYMBOLS_WIFI_OFF, true);
        lv_obj_t* label = lv_label_create(card);
        lv_label_set_text(label, "没有扫描到网络，点击右上角重试");
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    } else {
        for (size_t i = 0; i < scan_results_.size(); i++) {
            const auto& item = scan_results_[i];
            char subtitle[32];
            snprintf(subtitle, sizeof(subtitle), "%d dBm", item.rssi);
            MakeListItem(area, SignalIcon(item.rssi), item.ssid.c_str(), subtitle,
                         Action::WifiPickScanned, (int)i,
                         item.encrypted ? MATERIAL_SYMBOLS_LOCK
                                        : MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT);
        }
    }

    lv_obj_t* row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), kButtonHeight);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* manual = MakeTextButton(row, MATERIAL_SYMBOLS_EDIT_SQUARE, "手动输入",
                                      Action::WifiManualSsid, 0, false);
    lv_obj_set_width(manual, LV_PCT(31));
    lv_obj_t* saved =
        MakeTextButton(row, MATERIAL_SYMBOLS_KEY, "已保存", Action::WifiSavedList, 0, false);
    lv_obj_set_width(saved, LV_PCT(31));
    lv_obj_t* static_ip = MakeTextButton(row, MATERIAL_SYMBOLS_EDIT_SQUARE, "静态 IP",
                                         Action::WifiStaticIp, 0, false);
    lv_obj_set_width(static_ip, LV_PCT(31));
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

            // The C6 radio comes up over esp_hosted noticeably later than the
            // UI: a scan issued right after boot fails with
            // ESP_ERR_WIFI_NOT_STARTED (transient toast users reported). Wait
            // for the station to start instead of surfacing the race.
            esp_err_t err = ESP_ERR_WIFI_NOT_STARTED;
            for (int attempt = 0; attempt < 20; ++attempt) {
                err = esp_wifi_scan_start(&scan_config, true);
                if (err != ESP_ERR_WIFI_NOT_STARTED) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
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
        ShowResult(false, esp_err_to_name(error));
        return;
    }
    ShowWifiList();
}

void SettingsUi::ShowManualSsid() {
    wifi_state_ = WifiSettingsState::InputSsid;
    lv_obj_t* body = BuildPage("手动输入 SSID", Action::WifiRescan, false);

    textarea_ = lv_textarea_create(body);
    lv_obj_set_width(textarea_, LV_PCT(100));
    lv_obj_set_height(textarea_, kButtonHeight);
    lv_obj_set_style_radius(textarea_, 12, 0);
    lv_textarea_set_one_line(textarea_, true);
    lv_textarea_set_max_length(textarea_, 32);
    lv_textarea_set_placeholder_text(textarea_, "network name");

    MakeTextButton(body, MATERIAL_SYMBOLS_ARROW_FORWARD, "下一步", Action::WifiManualNext, 0,
                   true);

    keyboard_ = MakeKeyboard(body, textarea_, Action::WifiManualNext);
}

void SettingsUi::ShowPasswordInput(const std::string& ssid, bool encrypted) {
    wifi_state_ = WifiSettingsState::InputPassword;
    pending_ssid_ = ssid;
    pending_encrypted_ = encrypted;

    lv_obj_t* body = BuildPage(ssid.c_str(), Action::WifiPasswordCancel, false);

    textarea_ = lv_textarea_create(body);
    lv_obj_set_width(textarea_, LV_PCT(100));
    lv_obj_set_height(textarea_, kButtonHeight);
    lv_obj_set_style_radius(textarea_, 12, 0);
    lv_textarea_set_one_line(textarea_, true);
    lv_textarea_set_password_mode(textarea_, true);
    lv_textarea_set_max_length(textarea_, 64);
    lv_textarea_set_placeholder_text(textarea_, encrypted ? "password" : "(open network)");

    lv_obj_t* row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), kButtonHeight);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* toggle = MakeTextButton(row, MATERIAL_SYMBOLS_EYEGLASSES, "显示",
                                      Action::WifiTogglePassword, 0, false);
    lv_obj_set_width(toggle, LV_PCT(32));
    lv_obj_t* connect =
        MakeTextButton(row, MATERIAL_SYMBOLS_CHECK, "连接", Action::WifiConnectConfirm, 0, true);
    lv_obj_set_width(connect, LV_PCT(64));

    keyboard_ = MakeKeyboard(body, textarea_, Action::WifiConnectConfirm);
}

void SettingsUi::ShowSavedList() {
    wifi_state_ = WifiSettingsState::SavedList;
    saved_ssids_.clear();
    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        saved_ssids_.push_back(item.ssid);
    }

    lv_obj_t* body = BuildPage("已保存的网络", Action::WifiRescan, false);
    lv_obj_t* area = MakeScrollArea(body);

    if (saved_ssids_.empty()) {
        lv_obj_t* card = MakeCard(area);
        MakeIconLabel(card, MATERIAL_SYMBOLS_INFO, true);
        lv_obj_t* label = lv_label_create(card);
        lv_label_set_text(label, "还没有保存任何网络");
        lv_obj_set_flex_grow(label, 1);
        return;
    }

    for (size_t i = 0; i < saved_ssids_.size(); i++) {
        lv_obj_t* card = MakeCard(area);
        MakeIconLabel(card, MATERIAL_SYMBOLS_KEY, true);

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, saved_ssids_[i].c_str());
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

        lv_obj_t* connect = lv_button_create(card);
        lv_obj_set_size(connect, kIconButtonSize, kIconButtonSize);
        lv_obj_set_style_radius(connect, 12, 0);
        lv_obj_set_style_shadow_width(connect, 0, 0);
        lv_obj_set_style_bg_color(connect, lv_color_hex(kAccentColor), 0);
        lv_obj_set_style_text_color(connect, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(MakeIconLabel(connect, MATERIAL_SYMBOLS_LINK, false));
        Bind(connect, Action::WifiConnectSaved, (int)i);

        lv_obj_t* remove = lv_button_create(card);
        lv_obj_set_size(remove, kIconButtonSize, kIconButtonSize);
        lv_obj_set_style_radius(remove, 12, 0);
        lv_obj_set_style_shadow_width(remove, 0, 0);
        lv_obj_set_style_bg_color(remove, lv_color_hex(kDangerColor), 0);
        lv_obj_set_style_text_color(remove, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(MakeIconLabel(remove, MATERIAL_SYMBOLS_DELETE, false));
        Bind(remove, Action::WifiDeleteSaved, (int)i);
    }
}

void SettingsUi::ShowConnecting(const std::string& ssid) {
    wifi_state_ = WifiSettingsState::Connecting;
    lv_obj_t* body = BuildPage("连接中", Action::WifiResultBack, false);

    lv_obj_t* box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 24, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* spinner = lv_spinner_create(box);
    lv_obj_set_size(spinner, 96, 96);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kAccentColor), LV_PART_INDICATOR);

    lv_obj_t* label = lv_label_create(box);
    lv_label_set_text_fmt(label, "正在连接 %s", ssid.c_str());

    lv_obj_t* hint = lv_label_create(box);
    lv_label_set_text(hint, "最长约 20 秒");
    lv_obj_set_style_opa(hint, LV_OPA_60, 0);
}

void SettingsUi::ShowResult(bool success, const char* message) {
    wifi_state_ = success ? WifiSettingsState::Success : WifiSettingsState::Failed;
    lv_obj_t* body = BuildPage(success ? "连接成功" : "连接失败", Action::WifiResultBack, false);

    lv_obj_t* box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 24, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* icon =
        MakeIconLabel(box, success ? MATERIAL_SYMBOLS_CHECK_CIRCLE : MATERIAL_SYMBOLS_CANCEL,
                      true);
    lv_obj_set_style_text_color(icon, lv_color_hex(success ? kSuccessColor : kDangerColor), 0);

    lv_obj_t* label = lv_label_create(box);
    lv_label_set_text(label, message);
    lv_obj_set_width(label, LV_PCT(90));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    MakeTextButton(body, success ? MATERIAL_SYMBOLS_CHECK : MATERIAL_SYMBOLS_REFRESH,
                   success ? "完成" : "返回列表", Action::WifiResultBack, 0, true);
    MakeTextButton(body, MATERIAL_SYMBOLS_CLOSE, "关闭设置", Action::Close, 0, false);
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

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------

RotationProfile SettingsUi::MakeRotationProfile(int degrees) {
    // The panel and the touch controller always keep the 0-degree flags that
    // were validated on hardware; every other angle is applied with
    // lv_display_set_rotation(), and LVGL 9 rotates the pointer coordinates
    // itself (lv_indev.c: lv_display_rotate_point), so the touch flags must not
    // be changed per angle or the rotation is applied twice.
    if (degrees != 90 && degrees != 180 && degrees != 270) {
        degrees = 0;
    }
    return RotationProfile{degrees, false, false, false, false, true, true};
}

RotationProfile SettingsUi::LoadRotationProfile() {
    Settings settings("reterminal", false);
    int degrees = settings.GetInt("rotation", 0);
    return MakeRotationProfile(degrees);
}

bool SettingsUi::SaveRotation(int degrees) {
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        return false;
    }
    Settings settings("reterminal", true);
    settings.SetInt("rotation", degrees);
    return true;  // committed by the Settings destructor
}

void SettingsUi::ShowStaticIpPage() {
    lv_obj_t* body = BuildPage("静态 IP", Action::WifiRescan, false);

    Settings settings("wifi", false);
    bool enabled = settings.GetInt("static_en", 0) != 0;
    const char* keys[4] = {"static_ip", "static_gw", "static_mask", "static_dns"};
    const char* hints[4] = {"IP 地址 (192.168.1.50)", "网关", "子网掩码 (255.255.255.0)",
                            "DNS (可空, 默认网关)"};

    lv_obj_t* state = lv_label_create(body);
    lv_label_set_text(state, enabled ? "当前: 静态 IP" : "当前: DHCP 自动获取");

    for (int i = 0; i < 4; i++) {
        lv_obj_t* ta = lv_textarea_create(body);
        lv_obj_set_width(ta, LV_PCT(100));
        lv_obj_set_height(ta, kButtonHeight);
        lv_obj_set_style_radius(ta, 12, 0);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_max_length(ta, 15);
        lv_textarea_set_placeholder_text(ta, hints[i]);
        std::string value = settings.GetString(keys[i], "");
        if (!value.empty()) {
            lv_textarea_set_text(ta, value.c_str());
        }
        // Clicking a field points the shared keyboard at it.
        lv_obj_add_event_cb(
            ta,
            [](lv_event_t* event) {
                auto* self = static_cast<SettingsUi*>(lv_event_get_user_data(event));
                if (self->keyboard_ != nullptr) {
                    lv_keyboard_set_textarea(
                        self->keyboard_,
                        static_cast<lv_obj_t*>(lv_event_get_current_target(event)));
                }
            },
            LV_EVENT_CLICKED, this);
        static_ta_[i] = ta;
    }

    lv_obj_t* row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), kButtonHeight);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t* dhcp = MakeTextButton(row, MATERIAL_SYMBOLS_WIFI, "改用 DHCP",
                                    Action::WifiStaticUseDhcp, 0, false);
    lv_obj_set_width(dhcp, LV_PCT(38));
    lv_obj_t* save = MakeTextButton(row, MATERIAL_SYMBOLS_CHECK, "保存并重启",
                                    Action::WifiStaticSave, 0, true);
    lv_obj_set_width(save, LV_PCT(58));

    keyboard_ = MakeKeyboard(body, static_ta_[0], Action::WifiStaticSave);
}

void SettingsUi::ShowFacePage() {
    page_ = SettingsPage::Face;
    int mode = 0;
    std::string endpoint;
    if (face_get_) {
        face_get_(mode, endpoint);
    }
    // Only seed the selection from the saved config on first entry; a tile
    // tap rebuilds the page and must keep the user's pick.
    if (pending_face_mode_ < 0) {
        pending_face_mode_ = mode;
    }

    lv_obj_t* body = BuildPage("人脸识别", Action::Close, false);

    static const char* kModeNames[3] = {"关闭", "检测唤醒", "识别唤醒"};
    static const char* kModeHints[3] = {"不采集", "本地检测到人脸即唤醒", "认识的人才唤醒"};
    lv_obj_t* grid = lv_obj_create(body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, kGap, 0);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < 3; i++) {
        bool selected = i == pending_face_mode_;
        lv_obj_t* tile = lv_button_create(grid);
        lv_obj_set_size(tile, LV_PCT(31), 132);
        lv_obj_set_style_radius(tile, kCardRadius, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_bg_color(tile, selected ? lv_color_hex(kAccentColor) : CardColor(), 0);
        if (selected) {
            lv_obj_set_style_text_color(tile, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, 6, 0);
        lv_obj_t* name = lv_label_create(tile);
        lv_label_set_text(name, kModeNames[i]);
        lv_obj_t* hint = lv_label_create(tile);
        lv_label_set_text(hint, kModeHints[i]);
        lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);
        Bind(tile, Action::FacePick, i);
    }

    {
        Settings settings("face", false);
        int interval = settings.GetInt("interval_s", 5);
        int duration = settings.GetInt("duration_s", 2);
        int cooldown = settings.GetInt("cooldown_s", 8);
        int threshold = settings.GetInt("threshold", 60);
        int known_only = settings.GetInt("known_only", 1);
        // Only the two everyday knobs live on screen; interval, threshold and
        // known_only stay reachable over MCP (self.face.param_set) or HTTP.
        (void)interval;
        (void)threshold;
        (void)known_only;
        char text[24];
        snprintf(text, sizeof(text), "%d 秒", duration);
        MakeListItem(body, MATERIAL_SYMBOLS_CHECK, "持续确认", text, Action::FaceParamCycle, 1,
                     MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT);
        snprintf(text, sizeof(text), "%d 秒", cooldown);
        MakeListItem(body, MATERIAL_SYMBOLS_VOLUME_OFF, "唤醒冷却", text, Action::FaceParamCycle, 2,
                     MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT);
    }

    lv_obj_t* label = lv_label_create(body);
    lv_label_set_text(label, "识别服务地址（识别唤醒模式使用）");

    textarea_ = lv_textarea_create(body);
    lv_obj_set_width(textarea_, LV_PCT(100));
    lv_obj_set_height(textarea_, kButtonHeight);
    lv_obj_set_style_radius(textarea_, 12, 0);
    lv_textarea_set_one_line(textarea_, true);
    lv_textarea_set_max_length(textarea_, 96);
    lv_textarea_set_placeholder_text(textarea_, "http://192.168.x.x:8001/recognize");
    if (!endpoint.empty()) {
        lv_textarea_set_text(textarea_, endpoint.c_str());
    }

    MakeTextButton(body, MATERIAL_SYMBOLS_CHECK, "保存", Action::FaceSave, 0, true);

    keyboard_ = MakeKeyboard(body, textarea_, Action::FaceSave);
}

void SettingsUi::ShowDisplaySettings() {
    page_ = SettingsPage::Display;
    int current = LoadRotationProfile().degrees;
    if (pending_rotation_ != 0 && pending_rotation_ != 90 && pending_rotation_ != 180 &&
        pending_rotation_ != 270) {
        pending_rotation_ = current;
    }

    lv_obj_t* body = BuildPage("屏幕方向", Action::Close, false);

    lv_obj_t* grid = lv_obj_create(body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, kGap, 0);
    lv_obj_set_style_pad_column(grid, kGap, 0);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < 4; i++) {
        int degrees = kRotationChoices[i];
        bool selected = degrees == pending_rotation_;

        lv_obj_t* tile = lv_button_create(grid);
        lv_obj_set_size(tile, LV_PCT(47), 160);
        lv_obj_set_style_radius(tile, kCardRadius, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_bg_color(tile, selected ? lv_color_hex(kAccentColor) : CardColor(), 0);
        if (selected) {
            lv_obj_set_style_text_color(tile, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, 8, 0);

        MakeIconLabel(tile, MATERIAL_SYMBOLS_REPEAT, true);
        lv_obj_t* label = lv_label_create(tile);
        char text[24];
        snprintf(text, sizeof(text), "%d°%s", degrees, degrees == current ? "  (当前)" : "");
        lv_label_set_text(label, text);
        Bind(tile, Action::RotationPick, i);
    }

    {
        Settings settings("power", false);
        int dim_s = settings.GetInt("dim_s", 60);
        int off_s = settings.GetInt("off_s", 0);
        char dim_text[24];
        snprintf(dim_text, sizeof(dim_text), dim_s == 0 ? "关" : "%d 分钟", dim_s / 60);
        char off_text[24];
        snprintf(off_text, sizeof(off_text), off_s == 0 ? "关" : "%d 分钟", off_s / 60);
        MakeListItem(body, MATERIAL_SYMBOLS_REPEAT, "自动息屏", dim_text, Action::SleepDimCycle, 0,
                     MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT);
        MakeListItem(body, MATERIAL_SYMBOLS_POWER_SETTINGS_NEW, "无操作关机", off_text,
                     Action::SleepOffCycle, 0, MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT);
    }

    MakeTextButton(body, MATERIAL_SYMBOLS_POWER_SETTINGS_NEW, "保存并重启", Action::RotationSave,
                   0, true);
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

void SettingsUi::ShowVolumeSettings() {
    page_ = SettingsPage::Volume;
    auto* codec = Board::GetInstance().GetAudioCodec();
    int volume = codec != nullptr ? codec->output_volume() : 0;
    if (volume > 0) {
        volume_restore_ = volume;
    }

    lv_obj_t* body = BuildPage("音量", Action::Close, false);

    lv_obj_t* card = MakeCard(body);
    lv_obj_set_height(card, 120);
    MakeIconLabel(card, volume == 0 ? MATERIAL_SYMBOLS_VOLUME_OFF : MATERIAL_SYMBOLS_VOLUME_UP,
                  true);
    volume_value_ = lv_label_create(card);
    lv_label_set_text_fmt(volume_value_, "%d%%", volume);
    lv_obj_set_flex_grow(volume_value_, 1);
    lv_obj_set_style_text_align(volume_value_, LV_TEXT_ALIGN_RIGHT, 0);

    volume_slider_ = lv_slider_create(body);
    lv_obj_set_width(volume_slider_, LV_PCT(100));
    lv_obj_set_height(volume_slider_, kSliderHeight);
    lv_slider_set_range(volume_slider_, 0, 100);
    lv_slider_set_value(volume_slider_, volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider_, CardColor(), LV_PART_MAIN);
    lv_obj_set_style_radius(volume_slider_, kSliderHeight / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(kAccentColor), LV_PART_INDICATOR);
    lv_obj_set_style_radius(volume_slider_, kSliderHeight / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(kAccentColor), LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_slider_, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(volume_slider_, 16);
    lv_obj_add_event_cb(volume_slider_, VolumeSliderThunk, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(volume_slider_, VolumeSliderThunk, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(volume_slider_, VolumeSliderThunk, LV_EVENT_PRESS_LOST, this);

    MakeTextButton(body, volume == 0 ? MATERIAL_SYMBOLS_VOLUME_UP : MATERIAL_SYMBOLS_VOLUME_OFF,
                   volume == 0 ? "取消静音" : "静音", Action::VolumeMute, 0, false);

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, "松手时才写入设置，拖动过程只做预览。");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_opa(hint, LV_OPA_60, 0);
}

void SettingsUi::ShowRotationConfirm() {
    lv_obj_t* body = BuildPage("确认", Action::ShowDisplay, false);

    lv_obj_t* box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 24, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* icon = MakeIconLabel(box, MATERIAL_SYMBOLS_WARNING, true);
    lv_obj_set_style_text_color(icon, lv_color_hex(kDangerColor), 0);

    lv_obj_t* label = lv_label_create(box);
    lv_label_set_text_fmt(label, "将屏幕方向设为 %d°，设备会立即重启。", pending_rotation_);
    lv_obj_set_width(label, LV_PCT(90));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    MakeTextButton(body, MATERIAL_SYMBOLS_CHECK, "确认并重启", Action::RotationConfirm, 0, true);
    MakeTextButton(body, MATERIAL_SYMBOLS_CLOSE, "取消", Action::ShowDisplay, 0, false);
}

void SettingsUi::CommitRotation() {
    if (!SaveRotation(pending_rotation_)) {
        return;
    }
    ESP_LOGI(TAG, "Saved rotation=%d, rebooting", pending_rotation_);
    operation_active_ = true;

    lv_obj_t* body = BuildPage("屏幕方向", Action::ShowDisplay, false);
    lv_obj_t* box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 24, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* spinner = lv_spinner_create(box);
    lv_obj_set_size(spinner, 96, 96);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kAccentColor), LV_PART_INDICATOR);

    lv_obj_t* label = lv_label_create(box);
    lv_label_set_text_fmt(label, "已保存 %d°，正在重启 ...", pending_rotation_);

    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            Application::GetInstance().Reboot();
            vTaskDelete(nullptr);
        },
        "d1001_reboot", 4096, nullptr, 3, nullptr);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void SettingsUi::HandleAction(Action action, int index) {
    switch (action) {
        case Action::Close:
            if (operation_active_) {
                return;
            }
            Close();
            return;
        case Action::ShowDisplay:
            ShowDisplaySettings();
            return;
        case Action::WifiRescan:
            ShowWifiPage();
            return;
        case Action::WifiSavedList:
            ShowSavedList();
            return;
        case Action::WifiManualSsid:
            ShowManualSsid();
            return;
        case Action::WifiManualNext: {
            if (textarea_ == nullptr) {
                return;
            }
            std::string ssid = lv_textarea_get_text(textarea_);
            if (ssid.empty()) {
                return;
            }
            ShowPasswordInput(ssid, true);
            return;
        }
        case Action::WifiPickScanned:
            if (index >= 0 && index < (int)scan_results_.size()) {
                ShowPasswordInput(scan_results_[index].ssid, scan_results_[index].encrypted);
            }
            return;
        case Action::WifiTogglePassword:
            if (textarea_ != nullptr) {
                lv_textarea_set_password_mode(textarea_,
                                              !lv_textarea_get_password_mode(textarea_));
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
            if (operation_active_) {
                return;
            }
            ShowWifiPage();
            return;
        case Action::RotationPick:
            if (index >= 0 && index < 4) {
                pending_rotation_ = kRotationChoices[index];
                ShowDisplaySettings();
            }
            return;
        case Action::RotationSave:
            if (operation_active_) {
                return;
            }
            ShowRotationConfirm();
            return;
        case Action::RotationConfirm:
            if (operation_active_) {
                return;
            }
            CommitRotation();
            return;
        case Action::SleepDimCycle: {
            static const int kDimChoices[] = {0, 60, 300, 600};
            Settings settings("power", true);
            int current = settings.GetInt("dim_s", 60);
            int next = kDimChoices[0];
            for (int i = 0; i < 4; i++) {
                if (kDimChoices[i] == current) {
                    next = kDimChoices[(i + 1) % 4];
                    break;
                }
            }
            settings.SetInt("dim_s", next);
            if (power_save_changed_) {
                power_save_changed_();
            }
            ShowDisplaySettings();
            break;
        }
        case Action::SleepOffCycle: {
            static const int kOffChoices[] = {0, 600, 1800};
            Settings settings("power", true);
            int current = settings.GetInt("off_s", 0);
            int next = kOffChoices[0];
            for (int i = 0; i < 3; i++) {
                if (kOffChoices[i] == current) {
                    next = kOffChoices[(i + 1) % 3];
                    break;
                }
            }
            settings.SetInt("off_s", next);
            if (power_save_changed_) {
                power_save_changed_();
            }
            ShowDisplaySettings();
            break;
        }
        case Action::FacePick:
            pending_face_mode_ = index;
            ShowFacePage();
            break;
        case Action::FaceSave: {
            std::string endpoint =
                textarea_ != nullptr ? lv_textarea_get_text(textarea_) : "";
            if (face_apply_) {
                face_apply_(pending_face_mode_, endpoint);
            }
            Close();
            break;
        }
        case Action::FaceParamCycle: {
            static const char* kKeys[5] = {"interval_s", "duration_s", "cooldown_s",
                                           "threshold", "known_only"};
            static const int kChoices[5][4] = {{1, 3, 5, 10},
                                               {1, 2, 3, -1},
                                               {3, 8, 15, 30},
                                               {50, 60, 70, 80},
                                               {1, 0, -1, -1}};
            static const int kDefaults[5] = {5, 2, 8, 60, 1};
            Settings settings("face", true);
            int current = settings.GetInt(kKeys[index], kDefaults[index]);
            int next = kChoices[index][0];
            for (int i = 0; i < 4 && kChoices[index][i] != -1; i++) {
                if (kChoices[index][i] == current) {
                    int j = i + 1;
                    if (j >= 4 || kChoices[index][j] == -1) {
                        j = 0;
                    }
                    next = kChoices[index][j];
                    break;
                }
            }
            settings.SetInt(kKeys[index], next);
            if (face_apply_) {
                face_apply_(pending_face_mode_, "");
            }
            ShowFacePage();
            break;
        }
        case Action::WifiStaticIp:
            ShowStaticIpPage();
            break;
        case Action::WifiStaticSave: {
            const char* keys[4] = {"static_ip", "static_gw", "static_mask", "static_dns"};
            std::string values[4];
            for (int i = 0; i < 4; i++) {
                values[i] = static_ta_[i] != nullptr ? lv_textarea_get_text(static_ta_[i]) : "";
            }
            if (values[0].empty() || values[1].empty() || values[2].empty()) {
                return;  // ip/gw/mask are required
            }
            {
                Settings settings("wifi", true);
                settings.SetInt("static_en", 1);
                for (int i = 0; i < 4; i++) {
                    settings.SetString(keys[i], values[i]);
                }
            }
            esp_restart();
            break;
        }
        case Action::WifiStaticUseDhcp: {
            {
                Settings settings("wifi", true);
                settings.SetInt("static_en", 0);
            }
            esp_restart();
            break;
        }
        case Action::VolumeMute: {
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec == nullptr) {
                return;
            }
            int current = codec->output_volume();
            if (current > 0) {
                volume_restore_ = current;
                codec->SetOutputVolume(0);
            } else {
                codec->SetOutputVolume(volume_restore_ > 0 ? volume_restore_ : 60);
            }
            ShowVolumeSettings();
            return;
        }
    }
}
