#include "remote_display.h"
#include "board.h"

#include <esp_log.h>
#include <web_socket.h>
#include <cstring>
#include <arpa/inet.h>
#include <cJSON.h>
#include <mbedtls/base64.h>

#define TAG "RemoteDisplay"

// 音频帧头结构 (网络字节序)
struct __attribute__((packed)) AudioFrameHeader {
    uint8_t type;
    uint8_t flags;
    uint16_t payload_size;
    uint16_t sample_rate;
    uint8_t frame_duration;
    uint8_t reserved;
};

RemoteDisplay* RemoteDisplay::GetInstance() {
    static RemoteDisplay instance;
    return &instance;
}

RemoteDisplay::~RemoteDisplay() {
    Stop();
}

bool RemoteDisplay::Start(const std::string& server_url, int timeout_ms) {
    if (running_) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    // 获取网络接口并创建 WebSocket
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    websocket_ = network->CreateWebSocket(2);  // 使用 connect_id 2
    if (!websocket_) {
        ESP_LOGE(TAG, "Failed to create WebSocket");
        return false;
    }

    // 设置回调
    websocket_->OnConnected([this]() {
        ESP_LOGI(TAG, "Connected to remote display server");
        connected_ = true;
        SendHello();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Disconnected from remote display server");
        connected_ = false;
    });

    websocket_->OnError([](int error) {
        ESP_LOGE(TAG, "WebSocket error: %d", error);
    });

    // 尝试连接
    ESP_LOGI(TAG, "Connecting to %s (timeout %dms)...", server_url.c_str(), timeout_ms);

    if (!websocket_->Connect(server_url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to remote display server");
        websocket_.reset();
        return false;
    }

    // 等待连接完成
    int wait_time = 0;
    while (!connected_ && wait_time < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_time += 100;
    }

    if (!connected_) {
        ESP_LOGE(TAG, "Connection timeout");
        websocket_->Close();
        websocket_.reset();
        return false;
    }

    running_ = true;
    ESP_LOGI(TAG, "Remote display started (UI state sync mode)");
    return true;
}

void RemoteDisplay::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (websocket_) {
        websocket_->Close();
        websocket_.reset();
    }

    connected_ = false;
    ESP_LOGI(TAG, "Remote display stopped");
}

void RemoteDisplay::SendEmotion(const char* emotion) {
    if (!running_ || !connected_) return;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_emotion_ == emotion) return;  // 无变化
        current_emotion_ = emotion;
    }

    SendUIState();
}

void RemoteDisplay::SendStatus(const char* status) {
    if (!running_ || !connected_) return;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_status_ == status) return;  // 无变化
        current_status_ = status;
    }

    SendUIState();
}

void RemoteDisplay::SendChatMessage(const char* role, const char* content) {
    if (!running_ || !connected_) return;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_chat_role_ = role ? role : "";
        current_chat_text_ = content ? content : "";
    }

    SendUIState();
}

void RemoteDisplay::SendTheme(const char* theme_name) {
    if (!running_ || !connected_) return;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_theme_ = theme_name ? theme_name : "";
    }

    SendUIState();
}

void RemoteDisplay::SendVolume(int volume, bool muted) {
    if (!running_ || !connected_) return;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_volume_ == volume && current_muted_ == muted) return;
        current_volume_ = volume;
        current_muted_ = muted;
    }

    SendUIState();
}

void RemoteDisplay::SendUIState() {
    if (!connected_ || !websocket_) return;

    // 构造 JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ui_state");

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cJSON_AddStringToObject(root, "emotion", current_emotion_.c_str());
        cJSON_AddStringToObject(root, "status", current_status_.c_str());

        cJSON* chat = cJSON_CreateObject();
        cJSON_AddStringToObject(chat, "role", current_chat_role_.c_str());
        cJSON_AddStringToObject(chat, "text", current_chat_text_.c_str());
        cJSON_AddItemToObject(root, "chat", chat);

        cJSON_AddStringToObject(root, "theme", current_theme_.c_str());
        cJSON_AddNumberToObject(root, "volume", current_volume_);
        cJSON_AddBoolToObject(root, "muted", current_muted_);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        websocket_->Send(json_str);
        free(json_str);
    }
}

void RemoteDisplay::ForwardAudioPacket(const std::vector<uint8_t>& opus_data,
                                        int sample_rate, int frame_duration) {
    if (!running_ || !connected_) {
        return;
    }

    SendAudioFrame(opus_data, sample_rate, frame_duration);
}

bool RemoteDisplay::SendAudioFrame(const std::vector<uint8_t>& opus_data,
                                    int sample_rate, int frame_duration) {
    if (!connected_ || !websocket_) {
        return false;
    }

    // 尝试获取锁，如果被占用则跳过
    std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return true;  // 跳过本包，但不计入失败
    }

    // 构造消息
    size_t total_size = sizeof(AudioFrameHeader) + opus_data.size();
    std::vector<uint8_t> buffer(total_size);

    auto* header = reinterpret_cast<AudioFrameHeader*>(buffer.data());
    header->type = MSG_TYPE_AUDIO_FRAME;
    header->flags = 0;
    header->payload_size = htons(sizeof(AudioFrameHeader) - 4 + opus_data.size());
    header->sample_rate = htons(sample_rate);
    header->frame_duration = frame_duration;
    header->reserved = 0;

    memcpy(buffer.data() + sizeof(AudioFrameHeader), opus_data.data(), opus_data.size());

    return websocket_->Send(buffer.data(), buffer.size(), true);
}

void RemoteDisplay::SendHello() {
    // 发送 hello 消息
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "client", "sensecap-watcher");
    cJSON_AddStringToObject(root, "mode", "ui_state");

    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "opus");
    cJSON_AddItemToObject(root, "audio", audio);

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        websocket_->Send(json_str);
        ESP_LOGI(TAG, "Sent hello: %s", json_str);
        free(json_str);
    }
}

void RemoteDisplay::SendPreviewImage(const uint8_t* jpeg_data, size_t size) {
    if (!running_ || !connected_ || !websocket_) return;
    if (jpeg_data == nullptr || size == 0) return;

    // 计算 Base64 编码后的大小
    size_t base64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &base64_len, jpeg_data, size);

    // 分配 Base64 缓冲区
    char* base64_buf = (char*)malloc(base64_len + 1);
    if (base64_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for base64 encoding");
        return;
    }

    // 执行 Base64 编码
    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char*)base64_buf, base64_len + 1, &written, jpeg_data, size);
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed: %d", ret);
        free(base64_buf);
        return;
    }
    base64_buf[written] = '\0';

    // 构造 JSON 消息
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "preview_image");
    cJSON_AddStringToObject(root, "format", "jpeg");
    cJSON_AddNumberToObject(root, "size", size);
    cJSON_AddStringToObject(root, "data", base64_buf);

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(base64_buf);

    if (json_str) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        websocket_->Send(json_str);
        ESP_LOGI(TAG, "Sent preview image: %zu bytes (base64: %zu)", size, written);
        free(json_str);
    }
}
