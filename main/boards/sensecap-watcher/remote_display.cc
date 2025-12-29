#include "remote_display.h"
#include "display/lvgl_display/lvgl_display.h"
#include "board.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <web_socket.h>
#include <cstring>
#include <arpa/inet.h>

#define TAG "RemoteDisplay"

// 消息头结构 (网络字节序)
struct __attribute__((packed)) MessageHeader {
    uint8_t type;
    uint8_t flags;
    uint16_t payload_size;
};

struct __attribute__((packed)) ScreenFrameHeader {
    MessageHeader base;
    uint16_t width;
    uint16_t height;
    uint32_t timestamp;
};

struct __attribute__((packed)) AudioFrameHeader {
    MessageHeader base;
    uint16_t sample_rate;
    uint8_t frame_duration;
    uint8_t reserved;
};

RemoteDisplay::RemoteDisplay(LvglDisplay* display)
    : display_(display) {
}

RemoteDisplay::~RemoteDisplay() {
    Stop();
}

bool RemoteDisplay::Start(const std::string& server_url, int fps, int quality, int timeout_ms) {
    if (running_) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    fps_ = fps;
    quality_ = quality;

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

    // 启动屏幕捕获任务 (使用较小的栈)
    xTaskCreate([](void* arg) {
        static_cast<RemoteDisplay*>(arg)->ScreenCaptureTask();
        vTaskDelete(nullptr);
    }, "remote_disp", 4096, this, 1, &task_handle_);  // 降低优先级和栈大小

    ESP_LOGI(TAG, "Remote display started (fps=%d, quality=%d)", fps_, quality_);
    return true;
}

void RemoteDisplay::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // 等待任务结束
    if (task_handle_) {
        vTaskDelay(pdMS_TO_TICKS(500));
        task_handle_ = nullptr;
    }

    if (websocket_) {
        websocket_->Close();
        websocket_.reset();
    }

    connected_ = false;
    ESP_LOGI(TAG, "Remote display stopped");
}

void RemoteDisplay::ScreenCaptureTask() {
    ESP_LOGI(TAG, "Screen capture task started");

    TickType_t frame_interval = pdMS_TO_TICKS(1000 / fps_);
    uint32_t frame_count = 0;
    uint32_t fail_count = 0;

    while (running_ && connected_) {
        TickType_t start_tick = xTaskGetTickCount();
        std::string jpeg_data;

        // 截图
        if (display_->SnapshotToJpeg(jpeg_data, quality_)) {
            if (SendScreenFrame(jpeg_data)) {
                frame_count++;
                fail_count = 0;
                if (frame_count % 50 == 0) {
                    ESP_LOGI(TAG, "Sent %lu frames, jpeg size: %zu", frame_count, jpeg_data.size());
                }
            } else {
                fail_count++;
                ESP_LOGW(TAG, "Failed to send screen frame (fail_count=%lu)", fail_count);
                // 连续失败多次，可能连接已断开
                if (fail_count >= 5) {
                    ESP_LOGE(TAG, "Too many send failures, stopping");
                    connected_ = false;
                    break;
                }
            }
        } else {
            ESP_LOGW(TAG, "Failed to capture screen");
        }

        // 计算剩余等待时间，避免累积延迟
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed < frame_interval) {
            vTaskDelay(frame_interval - elapsed);
        }
    }

    ESP_LOGI(TAG, "Screen capture task ended (total frames: %lu)", frame_count);
}

bool RemoteDisplay::SendScreenFrame(const std::string& jpeg_data) {
    if (!connected_ || !websocket_) {
        return false;
    }

    // 尝试获取锁，如果被音频占用则跳过本帧
    std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return true;  // 返回 true 避免计入失败，只是跳过
    }

    // 构造消息
    size_t total_size = sizeof(ScreenFrameHeader) + jpeg_data.size();
    std::vector<uint8_t> buffer(total_size);

    auto* header = reinterpret_cast<ScreenFrameHeader*>(buffer.data());
    header->base.type = MSG_TYPE_SCREEN_FRAME;
    header->base.flags = 0;
    header->base.payload_size = htons(sizeof(ScreenFrameHeader) - sizeof(MessageHeader) + jpeg_data.size());
    header->width = htons(display_->width());
    header->height = htons(display_->height());
    header->timestamp = htonl(esp_timer_get_time() / 1000);

    memcpy(buffer.data() + sizeof(ScreenFrameHeader), jpeg_data.data(), jpeg_data.size());

    return websocket_->Send(buffer.data(), buffer.size(), true);
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

    // 尝试获取锁，如果被屏幕帧占用则跳过
    std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return true;  // 跳过本包，但不计入失败
    }

    // 构造消息
    size_t total_size = sizeof(AudioFrameHeader) + opus_data.size();
    std::vector<uint8_t> buffer(total_size);

    auto* header = reinterpret_cast<AudioFrameHeader*>(buffer.data());
    header->base.type = MSG_TYPE_AUDIO_FRAME;
    header->base.flags = 0;
    header->base.payload_size = htons(sizeof(AudioFrameHeader) - sizeof(MessageHeader) + opus_data.size());
    header->sample_rate = htons(sample_rate);
    header->frame_duration = frame_duration;
    header->reserved = 0;

    memcpy(buffer.data() + sizeof(AudioFrameHeader), opus_data.data(), opus_data.size());

    return websocket_->Send(buffer.data(), buffer.size(), true);
}

void RemoteDisplay::SendHello() {
    // 发送简单的 JSON hello 消息
    char hello[256];
    snprintf(hello, sizeof(hello),
        "{\"type\":\"hello\",\"client\":\"sensecap-watcher\","
        "\"screen\":{\"width\":%d,\"height\":%d,\"fps\":%d},"
        "\"audio\":{\"format\":\"opus\"}}",
        display_->width(), display_->height(), fps_);

    std::lock_guard<std::mutex> lock(send_mutex_);
    websocket_->Send(hello);
    ESP_LOGI(TAG, "Sent hello: %s", hello);
}
