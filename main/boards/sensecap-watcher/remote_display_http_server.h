#ifndef REMOTE_DISPLAY_HTTP_SERVER_H
#define REMOTE_DISPLAY_HTTP_SERVER_H

#include <esp_http_server.h>
#include <esp_timer.h>

// Lightweight HTTP server for remote display control + UDP beacon discovery.
// Endpoints:
//   POST /api/start_cast  {"ws_url":"ws://rpi:8765"}
//   POST /api/stop_cast
//   GET  /api/status
class RemoteDisplayHttpServer {
public:
    RemoteDisplayHttpServer() = default;
    ~RemoteDisplayHttpServer();

    // Start HTTP server on given port.
    // with_discovery: true = also start UDP beacon for discovery (first-time pairing)
    bool Start(int port = 80, bool with_discovery = true);
    void Stop();

    bool IsRunning() const { return server_ != nullptr; }

private:
    static esp_err_t HandleStartCast(httpd_req_t* req);
    static esp_err_t HandleStopCast(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);

    void StartBeacon(int port);
    void StopBeacon();
    void SendBeacon();
    static void BeaconTimerCallback(void* arg);

    httpd_handle_t server_ = nullptr;
    esp_timer_handle_t beacon_timer_ = nullptr;
    int beacon_sock_ = -1;
    int beacon_port_ = 80;
    static RemoteDisplayHttpServer* instance_;
};

#endif // REMOTE_DISPLAY_HTTP_SERVER_H
