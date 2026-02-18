#include "remote_display_http_server.h"
#include "remote_display.h"

#include <esp_log.h>
#include <mdns.h>
#include <cJSON.h>
#include <cstring>

static const char* TAG = "RemoteDisplayHttp";

// Max request body size (256 bytes is plenty for {"ws_url":"ws://..."})
#define MAX_REQ_BODY 256

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

// ---------- Start / Stop ----------

bool RemoteDisplayHttpServer::Start(int port) {
    if (server_) {
        ESP_LOGW(TAG, "HTTP server already running");
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.stack_size = 4096;

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

    // Register mDNS service
    RegisterMdns(port);

    return true;
}

void RemoteDisplayHttpServer::Stop() {
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

void RemoteDisplayHttpServer::RegisterMdns(int port) {
    // mDNS should already be initialized by the system.
    // If not, initialize it here.
    esp_err_t err = mdns_init();
    if (err == ESP_ERR_INVALID_STATE) {
        // Already initialized — that's fine
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set("sensecap-watcher");
    mdns_instance_name_set("SenseCAP Watcher");

    err = mdns_service_add("SenseCAP Watcher", "_xiaozhi-watcher", "_tcp", port, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_txt_item_t txt[] = {
        {(char*)"version", (char*)"1.0"},
        {(char*)"board", (char*)"sensecap-watcher"},
    };
    mdns_service_txt_set("_xiaozhi-watcher", "_tcp", txt, 2);

    ESP_LOGI(TAG, "mDNS service registered: _xiaozhi-watcher._tcp port %d", port);
}
