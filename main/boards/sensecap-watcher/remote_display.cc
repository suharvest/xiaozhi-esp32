#include "remote_display.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <web_socket.h>
#include <cstring>
#include <arpa/inet.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <mdns.h>
#include <esp_netif.h>
#include <sys/socket.h>
#include <fcntl.h>

#define TAG "RemoteDisplay"

// Parse ws://host:port/path → host, port
static bool ParseWsUrl(const std::string& url, std::string& host, uint16_t& port) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return false;
    std::string rest = url.substr(pos + 3);
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = (uint16_t)atoi(rest.substr(colon + 1).c_str());
    } else {
        host = rest;
        port = 80;
    }
    return !host.empty();
}

// Quick non-blocking TCP connect to check reachability (avoids 18s LWIP SYN timeout)
static bool IsTcpReachable(const char* host, uint16_t port, int timeout_ms) {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return false;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == 0) { close(sock); return true; }
    if (errno != EINPROGRESS) { close(sock); return false; }

    // Wait with select()
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    ret = select(sock + 1, nullptr, &wfds, nullptr, &tv);

    bool ok = false;
    if (ret > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
        ok = (err == 0);
    }
    close(sock);
    return ok;
}

// NVS 存储 key
#define NVS_NAMESPACE "remote_disp"
#define NVS_KEY_ENABLED "enabled"
#define NVS_KEY_URL "url"
#define NVS_KEY_TIMEOUT "timeout"

// Opus 音频帧头结构 (little-endian)
struct __attribute__((packed)) OpusAudioHeader {
    uint8_t type;            // MSG_TYPE_AUDIO_FRAME (0x02)
    uint16_t sample_rate;    // 采样率
    uint8_t frame_duration;  // 帧时长 (ms)
    // 后跟 Opus 编码数据
};

RemoteDisplay* RemoteDisplay::GetInstance() {
    static RemoteDisplay instance;
    return &instance;
}

RemoteDisplayConfig RemoteDisplay::LoadConfig() {
    RemoteDisplayConfig config;
    Settings settings(NVS_NAMESPACE, false);
    config.enabled = settings.GetBool(NVS_KEY_ENABLED, false);
    config.server_url = settings.GetString(NVS_KEY_URL, "");
    config.timeout_ms = settings.GetInt(NVS_KEY_TIMEOUT, 4000);
    return config;
}

void RemoteDisplay::SaveConfig(const RemoteDisplayConfig& config) {
    Settings settings(NVS_NAMESPACE, true);
    settings.SetBool(NVS_KEY_ENABLED, config.enabled);
    settings.SetString(NVS_KEY_URL, config.server_url);
    settings.SetInt(NVS_KEY_TIMEOUT, config.timeout_ms);
    ESP_LOGI(TAG, "Config saved: enabled=%d, url=%s, timeout=%d",
             config.enabled, config.server_url.c_str(), config.timeout_ms);
}

bool RemoteDisplay::StartWithConfig() {
    auto config = LoadConfig();
    if (!config.enabled) {
        ESP_LOGI(TAG, "Remote display disabled in config");
        return false;
    }
    if (config.server_url.empty()) {
        ESP_LOGW(TAG, "Remote display URL not configured");
        return false;
    }
    return Start(config.server_url, config.timeout_ms);
}

RemoteDisplay::~RemoteDisplay() {
    Stop();
    if (cleanup_timer_) {
        xTimerDelete(cleanup_timer_, portMAX_DELAY);
        cleanup_timer_ = nullptr;
    }
}

void RemoteDisplay::OnCleanupTimer(TimerHandle_t timer) {
    auto* self = static_cast<RemoteDisplay*>(pvTimerGetTimerID(timer));
    std::lock_guard<std::mutex> lifecycle_lock(self->lifecycle_mutex_);
    if (!self->running_ && !self->connected_ && self->websocket_) {
        {
            std::lock_guard<std::mutex> send_lock(self->send_mutex_);
            self->websocket_.reset();
        }
        self->current_server_url_.clear();
        ESP_LOGI(TAG, "WebSocket resources freed after disconnect, free internal: %lu",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
}

bool RemoteDisplay::Start(const std::string& server_url, int timeout_ms) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    if (running_) {
        // 如果 URL 相同且连接正常，直接返回
        if (current_server_url_ == server_url && connected_) {
            ESP_LOGI(TAG, "Already connected to %s", server_url.c_str());
            return true;
        }
        // URL 不同或连接已断开，先停止再重连
        ESP_LOGI(TAG, "Reconnecting: %s (connected=%d)", server_url.c_str(), connected_.load());
        running_ = false;
        {
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            if (websocket_) {
                websocket_->Close();
                websocket_.reset();
            }
        }
        connected_ = false;
        current_server_url_.clear();
    }

    // 获取网络接口并创建 WebSocket
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    // Create cleanup timer once (one-shot, 1s delay)
    if (!cleanup_timer_) {
        cleanup_timer_ = xTimerCreate("ws_clean", pdMS_TO_TICKS(1000), pdFALSE, this, OnCleanupTimer);
    }

    connected_ = false;
    uint32_t generation = connect_generation_.fetch_add(1) + 1;

    ESP_LOGI(TAG, "Free internal RAM before WebSocket create: %lu",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    websocket_ = network->CreateWebSocket(2);  // 使用 connect_id 2
    if (!websocket_) {
        ESP_LOGE(TAG, "Failed to create WebSocket");
        return false;
    }

    // 设置回调
    websocket_->OnConnected([this, generation]() {
        if (generation != connect_generation_.load()) {
            return;
        }
        ESP_LOGI(TAG, "Connected to remote display server");
        connected_ = true;
        SendHello();
    });

    websocket_->OnDisconnected([this, generation]() {
        if (generation != connect_generation_.load()) {
            return;
        }
        ESP_LOGW(TAG, "Disconnected, free internal: %lu",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        connected_ = false;
        running_ = false;
        // Defer WebSocket cleanup to free internal RAM
        // Can't destroy from within its own callback chain (TCP receive task UAF)
        if (cleanup_timer_) {
            xTimerReset(cleanup_timer_, 0);
        }
    });

    websocket_->OnError([](int error) {
        ESP_LOGE(TAG, "WebSocket error: %d", error);
    });

    // Quick TCP reachability check to avoid 18s LWIP SYN retry timeout
    std::string host;
    uint16_t port;
    if (ParseWsUrl(server_url, host, port)) {
        int check_ms = std::min(timeout_ms, 2000);
        if (!IsTcpReachable(host.c_str(), port, check_ms)) {
            ESP_LOGW(TAG, "Server not reachable: %s:%d (checked in %dms)", host.c_str(), port, check_ms);
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            websocket_.reset();
            return false;
        }
    }

    // 尝试连接
    ESP_LOGI(TAG, "Connecting to %s (timeout %dms)...", server_url.c_str(), timeout_ms);

    if (!websocket_->Connect(server_url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to remote display server");
        {
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            websocket_.reset();
        }
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
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        if (websocket_) {
            websocket_->Close();
            websocket_.reset();
        }
        return false;
    }

    running_ = true;
    current_server_url_ = server_url;

    ESP_LOGI(TAG, "Remote display started, free internal: %lu",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

void RemoteDisplay::Stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    if (!running_ && !websocket_) {
        return;
    }

    running_ = false;
    connect_generation_.fetch_add(1);

    // Cancel any pending cleanup timer
    if (cleanup_timer_) {
        xTimerStop(cleanup_timer_, 0);
    }

    {
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        if (websocket_) {
            websocket_->Close();
            websocket_.reset();
        }
    }

    connected_ = false;
    current_server_url_.clear();
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
    if (!connected_) return;

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
        std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
        if (lock.owns_lock() && running_ && connected_ && websocket_) {
            websocket_->Send(json_str);
        }
        free(json_str);
    }
}

void RemoteDisplay::ForwardOpusAudio(const std::vector<uint8_t>& opus_data, int sample_rate, int frame_duration) {
    if (!running_ || !connected_) {
        return;
    }

    // Non-blocking: drop audio frames if send is busy, to avoid blocking the protocol callback
    std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;  // Previous send still in progress, drop this frame
    }

    // 构建二进制消息：[type(1B)][sample_rate(2B)][frame_duration(1B)][opus_data]
    size_t total_size = sizeof(OpusAudioHeader) + opus_data.size();
    std::vector<uint8_t> buffer(total_size);

    auto* header = reinterpret_cast<OpusAudioHeader*>(buffer.data());
    header->type = MSG_TYPE_AUDIO_FRAME;
    header->sample_rate = (uint16_t)sample_rate;
    header->frame_duration = (uint8_t)frame_duration;

    memcpy(buffer.data() + sizeof(OpusAudioHeader), opus_data.data(), opus_data.size());

    if (running_ && connected_ && websocket_) {
        websocket_->Send(buffer.data(), buffer.size(), true);
    }
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
        if (connected_ && websocket_) {
            websocket_->Send(json_str);
            ESP_LOGI(TAG, "Sent hello: %s", json_str);
        }
        free(json_str);
    }
}

void RemoteDisplay::SendPreviewImage(const uint8_t* jpeg_data, size_t size) {
    if (!running_ || !connected_) return;
    if (jpeg_data == nullptr || size == 0) return;

    // 计算 Base64 编码后的大小
    size_t base64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &base64_len, jpeg_data, size);

    // 分配 Base64 缓冲区（使用 PSRAM 避免占用内部 RAM）
    char* base64_buf = (char*)heap_caps_malloc(base64_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
        if (lock.owns_lock() && running_ && connected_ && websocket_) {
            websocket_->Send(json_str);
            ESP_LOGI(TAG, "Sent preview image: %zu bytes (base64: %zu)", size, written);
        }
        free(json_str);
    }
}

std::vector<DiscoveredDisplay> RemoteDisplay::DiscoverDisplays(int timeout_ms) {
    std::vector<DiscoveredDisplay> displays;

    // 初始化 mDNS（如果尚未初始化）
    static bool mdns_initialized = false;
    static std::mutex mdns_init_mutex;
    {
        std::lock_guard<std::mutex> init_lock(mdns_init_mutex);
        if (!mdns_initialized) {
            if (mdns_init() != ESP_OK) {
                ESP_LOGE(TAG, "mDNS init failed");
                return displays;
            }
            mdns_initialized = true;
        }
    }

    ESP_LOGI(TAG, "Discovering display services via mDNS (timeout: %dms)...", timeout_ms);

    // 查询 _xiaozhi-display._tcp 服务
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_xiaozhi-display", "_tcp", timeout_ms, 10, &results);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS query failed: %s", esp_err_to_name(err));
        return displays;
    }

    if (!results) {
        ESP_LOGI(TAG, "No display services found via mDNS");
        return displays;
    }

    // 遍历结果
    mdns_result_t* r = results;
    while (r) {
        DiscoveredDisplay display;
        display.name = r->instance_name ? r->instance_name : "Unknown";
        display.port = r->port;

        // 获取 IPv4 地址
        if (r->addr) {
            mdns_ip_addr_t* addr = r->addr;
            while (addr) {
                if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                    char ip_str[16];
                    esp_ip4addr_ntoa(&addr->addr.u_addr.ip4, ip_str, sizeof(ip_str));
                    display.ip = ip_str;
                    break;
                }
                addr = addr->next;
            }
        }

        if (!display.ip.empty()) {
            displays.push_back(display);
            ESP_LOGI(TAG, "Found display: %s at %s:%d",
                     display.name.c_str(), display.ip.c_str(), display.port);
        }
        r = r->next;
    }

    mdns_query_results_free(results);
    return displays;
}

std::string RemoteDisplay::GetIPPrefix() {
    // 获取当前 WiFi 接口的 IP 地址
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "Failed to get WiFi netif");
        return "";
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info");
        return "";
    }

    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));

    // 提取前三段（如 192.168.1.）
    std::string ip(ip_str);
    size_t last_dot = ip.rfind('.');
    if (last_dot != std::string::npos) {
        return ip.substr(0, last_dot + 1);
    }
    return "";
}

bool RemoteDisplay::ConnectWithIPSuffix(int suffix, int port) {
    if (suffix < 0 || suffix > 255) {
        ESP_LOGE(TAG, "Invalid IP suffix: %d", suffix);
        return false;
    }

    std::string prefix = GetIPPrefix();
    if (prefix.empty()) {
        ESP_LOGE(TAG, "Failed to get IP prefix");
        return false;
    }

    std::string url = "ws://" + prefix + std::to_string(suffix) + ":" + std::to_string(port);
    ESP_LOGI(TAG, "Connecting to: %s", url.c_str());

    return Start(url);
}
