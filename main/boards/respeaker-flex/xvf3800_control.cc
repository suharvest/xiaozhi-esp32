#include "xvf3800_control.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

#define TAG "XVF3800"

namespace {

constexpr uint8_t kStatusSuccess = 0;
constexpr uint8_t kStatusRetry = 64;
// xvf_host.py retries a status-64 read up to 100 times with a 10 ms gap
// (python_control/xvf_host.py, the SERVICER_COMMAND_RETRY loop). The first
// attempt here used 8 tries at 5 ms and gave up on two parameters that the
// device simply had not finished serving.
constexpr int kMaxRetries = 100;
constexpr uint32_t kRetryDelayMs = 10;
constexpr uint32_t kTimeoutMs = 200;

// Resource / command ids, from the parameter table in reSpeaker's
// python_control/xvf_host.py. `count` is the number of elements, the byte
// length follows from the type.
struct Param {
    const char* name;
    uint8_t resid;
    uint8_t cmd;
    uint8_t bytes;
};

constexpr Param kVersion         = {"VERSION",            48,  0, 3};
constexpr Param kOpL             = {"AUDIO_MGR_OP_L",     35, 15, 2};
constexpr Param kOpR             = {"AUDIO_MGR_OP_R",     35, 19, 2};
constexpr Param kMicGain         = {"AUDIO_MGR_MIC_GAIN", 35,  0, 4};
constexpr Param kAgcOnOff        = {"PP_AGCONOFF",        17, 10, 4};
constexpr Param kAgcMaxGain      = {"PP_AGCMAXGAIN",      17, 11, 4};
constexpr Param kAgcDesiredLevel = {"PP_AGCDESIREDLEVEL", 17, 12, 4};
constexpr Param kAgcGain         = {"PP_AGCGAIN",         17, 13, 4};
constexpr Param kAsrOutOnOff     = {"AEC_ASROUTONOFF",    33, 35, 4};
constexpr Param kAsrOutGain      = {"AEC_ASROUTGAIN",     33, 36, 4};
// Decides which beams category 8 ("user chosen channels") actually carries,
// i.e. what our first slot really is.
constexpr Param kSelectedChannels = {"AUDIO_MGR_SELECTED_CHANNELS", 35, 12, 2};
// A limiter on the output caps the power the AGC is allowed to reach, so it
// can explain an AGC that never meets its own target level.
constexpr Param kLimitOnOff      = {"PP_LIMITONOFF",      17, 19, 4};
constexpr Param kLimitPLimit     = {"PP_LIMITPLIMIT",     17, 20, 4};
// Noise suppression gain floors: set too low they eat quiet speech, which
// shows up as a wake word that only occasionally passes threshold.
constexpr Param kMinNs           = {"PP_MIN_NS",          17, 21, 4};
constexpr Param kMinNn           = {"PP_MIN_NN",          17, 22, 4};
constexpr Param kEchoOnOff       = {"PP_ECHOONOFF",       17, 23, 4};

// Table 26 "Audio manager mux options" of the XVF3800 user guide.
const char* CategoryName(uint8_t category) {
    switch (category) {
        case 0:  return "silence";
        case 1:  return "raw mic, pre-amplification";
        case 2:  return "unpacked mic";
        case 3:  return "amplified mic + system delay (fed to SHF)";
        case 4:  return "far end reference";
        case 5:  return "far end reference + system delay";
        case 6:  return "processed: beamformed output";
        case 7:  return "AEC residual / ASR output";
        case 8:  return "user chosen (copies auto-select beam 6/3)";
        case 9:  return "post-SHF user DSP";
        case 10: return "far end at native rate";
        case 11: return "amplified mic, before system delay";
        case 12: return "amplified far end + system delay";
        default: return "unknown";
    }
}

// The one source value that carries its own meaning.
const char* SourceNote(uint8_t category, uint8_t source) {
    if (category == 6) {
        switch (source) {
            case 0:
            case 1:  return "slow-moving beam";
            case 2:  return "fast-moving beam";
            case 3:  return "auto-select beam (recommended for ASR)";
            default: return "";
        }
    }
    return "";
}

float DecodeFloat(const uint8_t* b) {
    float f;
    memcpy(&f, b, sizeof(f));  // little endian, as packed by xvf_host.py
    return f;
}

int32_t DecodeInt32(const uint8_t* b) {
    int32_t v;
    memcpy(&v, b, sizeof(v));
    return v;
}

}  // namespace

bool Xvf3800Control::Initialize(i2c_master_bus_handle_t bus, uint8_t address) {
    if (bus == nullptr) {
        ESP_LOGW(TAG, "No I2C bus, control interface unavailable");
        return false;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address;
    dev_cfg.scl_speed_hz = 100000;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        dev_ = nullptr;
        return false;
    }
    return true;
}

bool Xvf3800Control::Read(uint8_t resid, uint8_t cmd, uint8_t* out, uint8_t len) {
    if (dev_ == nullptr) {
        return false;
    }
    const uint8_t request[3] = {resid, static_cast<uint8_t>(cmd | 0x80),
                                static_cast<uint8_t>(len + 1)};
    uint8_t response[64];
    if (len + 1 > (int)sizeof(response)) {
        ESP_LOGE(TAG, "read length %u too large", (unsigned)len);
        return false;
    }

    for (int attempt = 0; attempt < kMaxRetries; attempt++) {
        esp_err_t err = i2c_master_transmit(dev_, request, sizeof(request), kTimeoutMs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "resid=%u cmd=%u transmit failed: %s", (unsigned)resid,
                     (unsigned)cmd, esp_err_to_name(err));
            return false;
        }
        err = i2c_master_receive(dev_, response, len + 1, kTimeoutMs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "resid=%u cmd=%u receive failed: %s", (unsigned)resid,
                     (unsigned)cmd, esp_err_to_name(err));
            return false;
        }
        if (response[0] == kStatusSuccess) {
            memcpy(out, response + 1, len);
            return true;
        }
        if (response[0] != kStatusRetry) {
            char hex[3 * 16 + 1] = {0};
            int off = 0;
            for (int i = 0; i < len + 1 && off + 3 < (int)sizeof(hex); i++) {
                off += snprintf(hex + off, sizeof(hex) - off, "%02x ", response[i]);
            }
            ESP_LOGE(TAG, "resid=%u cmd=%u bad status %u, raw: %s", (unsigned)resid,
                     (unsigned)cmd, (unsigned)response[0], hex);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
    }
    char hex[3 * 16 + 1] = {0};
    int off = 0;
    for (int i = 0; i < len + 1 && off + 3 < (int)sizeof(hex); i++) {
        off += snprintf(hex + off, sizeof(hex) - off, "%02x ", response[i]);
    }
    ESP_LOGE(TAG, "resid=%u cmd=%u still status 64 after %d attempts, last raw: %s",
             (unsigned)resid, (unsigned)cmd, kMaxRetries, hex);
    return false;
}

bool Xvf3800Control::Write(uint8_t resid, uint8_t cmd, const uint8_t* payload, uint8_t len) {
    if (dev_ == nullptr) {
        return false;
    }
    uint8_t frame[3 + 16];
    if (len > (int)sizeof(frame) - 3) {
        ESP_LOGE(TAG, "write length %u too large", (unsigned)len);
        return false;
    }
    frame[0] = resid;
    frame[1] = cmd;  // no 0x80: bit 7 set would make it a read
    frame[2] = len;
    memcpy(frame + 3, payload, len);
    esp_err_t err = i2c_master_transmit(dev_, frame, len + 3, kTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resid=%u cmd=%u write failed: %s", (unsigned)resid, (unsigned)cmd,
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

void Xvf3800Control::ApplyTuning() {
#if CONFIG_XVF3800_APPLY_TUNING
    if (dev_ == nullptr) {
        return;
    }
    // One parameter per boot. SAVE_CONFIGURATION (48, 9) is deliberately not
    // issued, so this never reaches the device's flash and a power cycle puts
    // the old value back.
    const float target = CONFIG_XVF3800_AGC_DESIRED_LEVEL_PPM / 1000000.0f;
    uint8_t buf[8];

    if (!Read(kAgcDesiredLevel.resid, kAgcDesiredLevel.cmd, buf, kAgcDesiredLevel.bytes)) {
        ESP_LOGE(TAG, "tuning aborted: cannot read AGC_DESIRED_LEVEL first");
        return;
    }
    const float before = DecodeFloat(buf);
    ESP_LOGW(TAG, "tuning AGC_DESIRED_LEVEL: before = %.8f", before);

    uint8_t payload[4];
    memcpy(payload, &target, sizeof(target));
    if (!Write(kAgcDesiredLevel.resid, kAgcDesiredLevel.cmd, payload, sizeof(payload))) {
        return;
    }
    ESP_LOGW(TAG, "tuning AGC_DESIRED_LEVEL: wrote  = %.8f", target);

    vTaskDelay(pdMS_TO_TICKS(20));
    if (!Read(kAgcDesiredLevel.resid, kAgcDesiredLevel.cmd, buf, kAgcDesiredLevel.bytes)) {
        ESP_LOGE(TAG, "cannot read AGC_DESIRED_LEVEL back");
        return;
    }
    const float after = DecodeFloat(buf);
    ESP_LOGW(TAG, "tuning AGC_DESIRED_LEVEL: after  = %.8f %s", after,
             after == target ? "(applied)" : "(NOT applied)");
    ESP_LOGW(TAG, "not saved to flash, a power cycle restores %.8f", before);
#endif
}

void Xvf3800Control::DumpConfiguration() {
    if (dev_ == nullptr) {
        return;
    }
    uint8_t buf[8];

    if (Read(kVersion.resid, kVersion.cmd, buf, kVersion.bytes)) {
        ESP_LOGW(TAG, "firmware version %u.%u.%u", buf[0], buf[1], buf[2]);
    }

    // The two I2S slots. Category/source come from Table 26 of the user guide.
    if (Read(kOpL.resid, kOpL.cmd, buf, kOpL.bytes)) {
        ESP_LOGW(TAG, "OP_L  = (%u, %u)  %s %s", buf[0], buf[1], CategoryName(buf[0]),
                 SourceNote(buf[0], buf[1]));
    }
    if (Read(kOpR.resid, kOpR.cmd, buf, kOpR.bytes)) {
        ESP_LOGW(TAG, "OP_R  = (%u, %u)  %s %s", buf[0], buf[1], CategoryName(buf[0]),
                 SourceNote(buf[0], buf[1]));
    }

    if (Read(kMicGain.resid, kMicGain.cmd, buf, kMicGain.bytes)) {
        ESP_LOGW(TAG, "MIC_GAIN = %.4f", DecodeFloat(buf));
    }
    if (Read(kAgcOnOff.resid, kAgcOnOff.cmd, buf, kAgcOnOff.bytes)) {
        ESP_LOGW(TAG, "AGC = %s", DecodeInt32(buf) ? "on" : "off");
    }
    if (Read(kAgcGain.resid, kAgcGain.cmd, buf, kAgcGain.bytes)) {
        ESP_LOGW(TAG, "AGC_GAIN = %.4f", DecodeFloat(buf));
    }
    if (Read(kAgcMaxGain.resid, kAgcMaxGain.cmd, buf, kAgcMaxGain.bytes)) {
        ESP_LOGW(TAG, "AGC_MAX_GAIN = %.4f", DecodeFloat(buf));
    }
    if (Read(kAgcDesiredLevel.resid, kAgcDesiredLevel.cmd, buf, kAgcDesiredLevel.bytes)) {
        ESP_LOGW(TAG, "AGC_DESIRED_LEVEL = %.8f", DecodeFloat(buf));
    }
    if (Read(kAsrOutOnOff.resid, kAsrOutOnOff.cmd, buf, kAsrOutOnOff.bytes)) {
        ESP_LOGW(TAG, "ASR_OUT = %s (0: AEC residual per mic, 1: ASR output per beam)",
                 DecodeInt32(buf) ? "on" : "off");
    }
    if (Read(kAsrOutGain.resid, kAsrOutGain.cmd, buf, kAsrOutGain.bytes)) {
        ESP_LOGW(TAG, "ASR_OUT_GAIN = %.4f", DecodeFloat(buf));
    }
    if (Read(kSelectedChannels.resid, kSelectedChannels.cmd, buf, kSelectedChannels.bytes)) {
        ESP_LOGW(TAG, "SELECTED_CHANNELS = (%u, %u)  (beams feeding category 8)", buf[0], buf[1]);
    }
    if (Read(kLimitOnOff.resid, kLimitOnOff.cmd, buf, kLimitOnOff.bytes)) {
        ESP_LOGW(TAG, "LIMIT = %s", DecodeInt32(buf) ? "on" : "off");
    }
    if (Read(kLimitPLimit.resid, kLimitPLimit.cmd, buf, kLimitPLimit.bytes)) {
        ESP_LOGW(TAG, "LIMIT_PLIMIT = %.8f (power)", DecodeFloat(buf));
    }
    if (Read(kMinNs.resid, kMinNs.cmd, buf, kMinNs.bytes)) {
        ESP_LOGW(TAG, "MIN_NS = %.4f (stationary NS gain floor)", DecodeFloat(buf));
    }
    if (Read(kMinNn.resid, kMinNn.cmd, buf, kMinNn.bytes)) {
        ESP_LOGW(TAG, "MIN_NN = %.4f (non-stationary NS gain floor)", DecodeFloat(buf));
    }
    if (Read(kEchoOnOff.resid, kEchoOnOff.cmd, buf, kEchoOnOff.bytes)) {
        ESP_LOGW(TAG, "ECHO_SUPPRESSION = %s", DecodeInt32(buf) ? "on" : "off");
    }
    ESP_LOGW(TAG, "---- end of XVF3800 baseline ----");
}
