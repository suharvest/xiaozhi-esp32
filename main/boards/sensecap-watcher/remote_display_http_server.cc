#include "remote_display_http_server.h"
#include "remote_display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <cJSON.h>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char* TAG = "RemoteDisplayHttp";

RemoteDisplayHttpServer* RemoteDisplayHttpServer::instance_ = nullptr;

// Max request body size (256 bytes is plenty for {"ws_url":"ws://..."})
#define MAX_REQ_BODY 256

// UDP beacon port and interval
#define BEACON_UDP_PORT 12321
#define BEACON_INTERVAL_US (2 * 1000 * 1000)  // 2 seconds

RemoteDisplayHttpServer::~RemoteDisplayHttpServer() {
    Stop();
}

// ---------- HTTP handlers (static) ----------

esp_err_t RemoteDisplayHttpServer::HandleStartCast(httpd_req_t* req) {
    char buf[MAX_REQ_BODY];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* ws_url = cJSON_GetObjectItem(root, "ws_url");
    if (!ws_url || !cJSON_IsString(ws_url) || strlen(ws_url->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ws_url");
        return ESP_FAIL;
    }

    auto* remote = RemoteDisplay::GetInstance();
    bool ok = false;

    if (remote->IsRunning()) {
        // Already casting — stop first then reconnect
        remote->Stop();
    }

    // Stop beacon before WebSocket connect — RPi already found us
    if (instance_) {
        instance_->StopBeacon();
    }

    ok = remote->Start(ws_url->valuestring, 3000);

    if (ok) {
        // Save to NVS so auto-reconnect works after reboot
        auto config = RemoteDisplay::LoadConfig();
        config.server_url = ws_url->valuestring;
        config.enabled = true;
        RemoteDisplay::SaveConfig(config);
    }

    cJSON_Delete(root);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", ok);
    if (!ok) {
        cJSON_AddStringToObject(resp, "error", "Failed to connect");
    }
    char* resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str);
    free(resp_str);
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t RemoteDisplayHttpServer::HandleStopCast(httpd_req_t* req) {
    auto* remote = RemoteDisplay::GetInstance();
    remote->Stop();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

esp_err_t RemoteDisplayHttpServer::HandleStatus(httpd_req_t* req) {
    auto* remote = RemoteDisplay::GetInstance();
    auto config = RemoteDisplay::LoadConfig();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "casting", remote->IsRunning());
    cJSON_AddStringToObject(resp, "server_url", config.server_url.c_str());

    char* resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str);
    free(resp_str);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ---------- UDP Beacon ----------

void RemoteDisplayHttpServer::StartBeacon(int port) {
    beacon_port_ = port;

    // Create UDP socket
    beacon_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (beacon_sock_ < 0) {
        ESP_LOGE(TAG, "Failed to create beacon socket: errno %d", errno);
        return;
    }

    // Enable broadcast
    int broadcast = 1;
    setsockopt(beacon_sock_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Create periodic timer
    esp_timer_create_args_t timer_args = {
        .callback = BeaconTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "beacon",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &beacon_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create beacon timer: %s", esp_err_to_name(err));
        close(beacon_sock_);
        beacon_sock_ = -1;
        return;
    }

    esp_timer_start_periodic(beacon_timer_, BEACON_INTERVAL_US);

    // Send first beacon immediately
    SendBeacon();

    ESP_LOGI(TAG, "UDP beacon started on port %d (broadcast every 2s)", BEACON_UDP_PORT);
}

void RemoteDisplayHttpServer::SendBeacon() {
    if (beacon_sock_ < 0) return;

    // Get own IP address
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return;
    if (ip_info.ip.addr == 0) return;

    // Format: XZWATCH|name|ip|port|board|version
    char packet[128];
    int len = snprintf(packet, sizeof(packet),
        "XZWATCH|SenseCAP Watcher|" IPSTR "|%d|sensecap-watcher|1.0",
        IP2STR(&ip_info.ip), beacon_port_);

    // Send broadcast
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(BEACON_UDP_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    int ret = sendto(beacon_sock_, packet, len, 0, (struct sockaddr*)&dest, sizeof(dest));

    // Log first 3 beacons and then every 30th (~1 min)
    static int count = 0;
    count++;
    if (count <= 3 || count % 30 == 0) {
        ESP_LOGI(TAG, "Beacon #%d sent (%d bytes, ret=%d): %s", count, len, ret, packet);
    }
}

void RemoteDisplayHttpServer::StopBeacon() {
    if (beacon_timer_) {
        esp_timer_stop(beacon_timer_);
        esp_timer_delete(beacon_timer_);
        beacon_timer_ = nullptr;
    }
    if (beacon_sock_ >= 0) {
        close(beacon_sock_);
        beacon_sock_ = -1;
    }
    ESP_LOGI(TAG, "UDP beacon stopped");
}

void RemoteDisplayHttpServer::BeaconTimerCallback(void* arg) {
    auto* self = static_cast<RemoteDisplayHttpServer*>(arg);
    self->SendBeacon();
}

// ---------- Start / Stop ----------

bool RemoteDisplayHttpServer::Start(int port, bool with_discovery) {
    if (server_) {
        ESP_LOGW(TAG, "HTTP server already running");
        return true;
    }

    instance_ = this;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.stack_size = 3072;
    config.open_fn = [](httpd_handle_t hd, int sockfd) -> esp_err_t {
        // Reduce LWIP socket buffers to save ~6KB internal SRAM per connection
        int sndbuf = 2920;
        int rcvbuf = 2920;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        return ESP_OK;
    };

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return false;
    }

    // POST /api/start_cast
    httpd_uri_t start_uri = {
        .uri = "/api/start_cast",
        .method = HTTP_POST,
        .handler = HandleStartCast,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &start_uri);

    // POST /api/stop_cast
    httpd_uri_t stop_uri = {
        .uri = "/api/stop_cast",
        .method = HTTP_POST,
        .handler = HandleStopCast,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &stop_uri);

    // GET /api/status
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = HandleStatus,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &status_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", port);

    // Start UDP beacon for discovery (only for first-time pairing)
    if (with_discovery) {
        StartBeacon(port);
    }

    return true;
}

void RemoteDisplayHttpServer::Stop() {
    StopBeacon();
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
