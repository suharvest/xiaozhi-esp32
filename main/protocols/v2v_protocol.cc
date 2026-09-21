#include "v2v_protocol.h"

#include "application.h"
#include "usb_log_sink.h"
#include "usb_tx_queue.h"

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>

#define TAG "V2vProtocol"

namespace {

constexpr uint32_t kRxPollMs = 50;         // also drives the candidate timeout
constexpr uint32_t kConfigRetryMs = 1000;  // spec §3.1
constexpr uint32_t kPingIntervalMs = 2000; // spec §5.3
constexpr int kMaxMissedPongs = 3;         // 6 s without a reply means gone
constexpr size_t kRxChunk = 512;

uint32_t NowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Reads a string member, empty when absent.
std::string JsonString(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return (item != nullptr && cJSON_IsString(item)) ? item->valuestring : std::string();
}

}  // namespace

V2vProtocol::V2vProtocol() {
    // The link carries raw PCM at the codec rate in both directions; the base
    // class default of 24 kHz / 60 ms belongs to the Opus transports.
    server_sample_rate_ = 16000;
    server_frame_duration_ = 20;
}

V2vProtocol::~V2vProtocol() {
    running_ = false;
    if (rx_task_ != nullptr) {
        vTaskDelete(rx_task_);
        rx_task_ = nullptr;
    }
}

bool V2vProtocol::Start() {
    if (!usb_transport::UsbTxQueue::GetInstance().Start()) {
        ESP_LOGE(TAG, "Failed to start the USB send queue");
        return false;
    }
    // From here on the port carries frames, so logging has to be framed too.
    usb_transport::InstallUsbLogSink();

    running_ = true;
    if (xTaskCreate(RxTaskEntry, "v2v_rx", 4096, this, 5, &rx_task_) != pdPASS) {
        rx_task_ = nullptr;
        running_ = false;
        return false;
    }
    return true;
}

void V2vProtocol::RxTaskEntry(void* arg) {
    static_cast<V2vProtocol*>(arg)->RxTask();
}

void V2vProtocol::RxTask() {
    std::vector<uint8_t> chunk(kRxChunk);
    std::vector<usb_frame::Frame> frames;

    while (running_) {
        const int n = usb_serial_jtag_read_bytes(chunk.data(), chunk.size(),
                                                 pdMS_TO_TICKS(kRxPollMs));
        const uint32_t now = NowMs();

        // Always feed, even with zero bytes: a silent link may be holding half
        // a candidate frame whose timeout nothing else would ever evaluate
        // (spec §7.6 item 4).
        frames.clear();
        decoder_.Feed(n > 0 ? chunk.data() : nullptr, n > 0 ? static_cast<size_t>(n) : 0, now,
                      frames);
        for (const auto& frame : frames) {
            HandleFrame(frame);
        }

        if (!handshaked_) {
            if (last_config_ms_ == 0 || now - last_config_ms_ >= kConfigRetryMs) {
                SendConfig();
                last_config_ms_ = now;
            }
            continue;
        }

        if (now - last_ping_ms_ >= kPingIntervalMs) {
            if (missed_pongs_ >= kMaxMissedPongs) {
                ESP_LOGW(TAG, "No PONG for %d intervals, dropping the session",
                         missed_pongs_);
                handshaked_ = false;
                missed_pongs_ = 0;
                last_config_ms_ = 0;
                CloseAudioChannel(false);
                // A fresh handshake restarts the numbering (spec §6).
                usb_transport::UsbTxQueue::GetInstance().ResetSeq();
                if (on_disconnected_) {
                    on_disconnected_();
                }
                continue;
            }
            SendPing(now);
            missed_pongs_++;
            last_ping_ms_ = now;
        }
    }
    vTaskDelete(nullptr);
}

void V2vProtocol::HandleFrame(const usb_frame::Frame& frame) {
    switch (frame.type) {
        case usb_frame::kTypePing: {
            // Echo the payload back untouched.
            usb_transport::UsbTxQueue::GetInstance().Post(
                usb_frame::kTypePong, frame.payload.data(), frame.payload.size());
            break;
        }
        case usb_frame::kTypePong:
            missed_pongs_ = 0;
            break;
        case usb_frame::kTypeAudioDown: {
            if (!audio_channel_opened_ || !on_incoming_audio_) {
                break;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = server_sample_rate_;
            packet->frame_duration = server_frame_duration_;
            packet->payload = frame.payload;
            on_incoming_audio_(std::move(packet));
            break;
        }
        case usb_frame::kTypeCtrlDown:
            HandleCtrlDown(std::string(frame.payload.begin(), frame.payload.end()));
            break;
        default:
            // LOG, AUDIO_UP and CTRL_UP are device to host; ignore them here.
            break;
    }
}

void V2vProtocol::HandleCtrlDown(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Unparsable control message");
        return;
    }
    const std::string type = JsonString(root, "type");

    // Any control frame ends the handshake retry loop (spec §3.1).
    if (!handshaked_) {
        handshaked_ = true;
        last_ping_ms_ = NowMs();
        missed_pongs_ = 0;
        if (on_connected_) {
            on_connected_();
        }
    }

    if (type == "session.updated") {
        const cJSON* x_usb = cJSON_GetObjectItem(root, "x_usb");
        if (x_usb != nullptr) {
            const cJSON* mode = cJSON_GetObjectItem(x_usb, "mode");
            if (mode != nullptr) {
                ApplyMode(JsonString(mode, "wake"), JsonString(mode, "vad"));
            }
        }
        if (!audio_channel_opened_) {
            audio_channel_opened_ = true;
            if (on_audio_channel_opened_) {
                on_audio_channel_opened_();
            }
        }
        SendModeActual();
        cJSON_Delete(root);
        return;
    }

    // The rest is translated into the vocabulary Application already handles.
    std::string translated;
    if (type == "tts_started") {
        translated = "{\"type\":\"tts\",\"state\":\"start\"}";
    } else if (type == "tts_done" || type == "abort") {
        translated = "{\"type\":\"tts\",\"state\":\"stop\"}";
    } else if (type == "asr_partial" || type == "asr_final") {
        const std::string text = JsonString(root, "text");
        translated = "{\"type\":\"stt\",\"text\":\"" + text + "\"}";
    }
    cJSON_Delete(root);

    if (!translated.empty() && on_incoming_json_) {
        cJSON* out = cJSON_Parse(translated.c_str());
        if (out != nullptr) {
            on_incoming_json_(out);
            cJSON_Delete(out);
        }
    }
}

void V2vProtocol::ApplyMode(const std::string& wake, const std::string& vad) {
    auto& app = Application::GetInstance();

    // caps advertised in config: wake and vad_barge_in. always_stream is not
    // advertised, so a request for wake=host degrades back to device and the
    // host is told through mode_actual (spec §3.1).
    if (wake == "host") {
        ESP_LOGW(TAG, "wake=host requested but always_stream is not advertised, staying on device");
        mode_wake_ = "device";
    } else if (!wake.empty()) {
        mode_wake_ = "device";
    }
    app.SetContinuousUplinkEnabled(mode_wake_ == "host");

    if (vad == "host") {
        mode_vad_ = "host";
    } else if (!vad.empty()) {
        mode_vad_ = "device";
    }
    app.SetVadBargeInEnabled(mode_vad_ == "device");
    ESP_LOGI(TAG, "mode wake=%s vad=%s", mode_wake_.c_str(), mode_vad_.c_str());
}

void V2vProtocol::SendConfig() {
    std::string json =
        "{\"type\":\"config\",\"sample_rate\":16000,\"channels\":1,\"x_usb\":{\"fw\":\"";
    json += BOARD_NAME;
    json += "\",\"frame_ms\":20,\"wake_word\":\"";
#ifdef CONFIG_CUSTOM_WAKE_WORD
    json += CONFIG_CUSTOM_WAKE_WORD;
#endif
    json += "\",\"caps\":[\"wake\",\"vad_barge_in\"],\"preroll_ms_max\":0}}";
    SendCtrl(json, false);
}

void V2vProtocol::SendModeActual() {
    std::string json = "{\"type\":\"x_usb.stats\",\"mode_actual\":{\"wake\":\"";
    json += mode_wake_;
    json += "\",\"vad\":\"";
    json += mode_vad_;
    json += "\"}}";
    SendCtrl(json, false);
}

void V2vProtocol::SendPing(uint32_t now_ms) {
    uint8_t payload[8];
    const uint64_t us = static_cast<uint64_t>(now_ms) * 1000ULL;
    for (int i = 0; i < 8; i++) {
        payload[i] = static_cast<uint8_t>((us >> (8 * i)) & 0xFF);
    }
    usb_transport::UsbTxQueue::GetInstance().Post(usb_frame::kTypePing, payload, sizeof(payload));
}

bool V2vProtocol::SendCtrl(const std::string& json, bool order_barrier) {
    return usb_transport::UsbTxQueue::GetInstance().Post(usb_frame::kTypeCtrlUp, json.data(),
                                                         json.size(), order_barrier);
}

bool V2vProtocol::SendText(const std::string& text) {
    return SendCtrl(text, false);
}

bool V2vProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!audio_channel_opened_ || packet == nullptr) {
        return false;
    }
    return usb_transport::UsbTxQueue::GetInstance().Post(
        usb_frame::kTypeAudioUp, packet->payload.data(), packet->payload.size());
}

bool V2vProtocol::OpenAudioChannel() {
    // There is no per-session channel on this link: the handshake is the
    // channel. Until the host has answered there is nowhere to send audio.
    if (!handshaked_) {
        return false;
    }
    if (!audio_channel_opened_.exchange(true) && on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void V2vProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // nothing to say: the link stays up between turns
    if (audio_channel_opened_.exchange(false) && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool V2vProtocol::IsAudioChannelOpened() const {
    return audio_channel_opened_;
}

void V2vProtocol::SendWakeWordDetected(const std::string& wake_word) {
    if (mode_wake_ != "device") {
        return;
    }
    std::string json = "{\"type\":\"x_usb.wake\",\"source\":\"multinet\",\"text\":\"";
    json += wake_word;
    json += "\"}";
    SendCtrl(json, false);
}

void V2vProtocol::SendStartListening(ListeningMode mode) {
    // Opening the channel already says the turn started; the v2v vocabulary
    // has no separate listen.start.
    (void)mode;
}

void V2vProtocol::SendStopListening() {
    if (mode_vad_ != "device") {
        return;
    }
    SendCtrl("{\"type\":\"asr_eos\"}", true);
}

void V2vProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    if (mode_vad_ != "device") {
        return;
    }
    SendCtrl("{\"type\":\"abort\"}", true);
}

void V2vProtocol::SendMcpMessage(const std::string& message) {
    // MCP does not cross this link (spec §3).
    (void)message;
}
