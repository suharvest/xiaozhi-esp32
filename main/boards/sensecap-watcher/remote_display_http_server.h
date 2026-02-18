#ifndef REMOTE_DISPLAY_HTTP_SERVER_H
#define REMOTE_DISPLAY_HTTP_SERVER_H

#include <esp_http_server.h>

// Lightweight HTTP server for remote display control + mDNS registration.
// Endpoints:
//   POST /api/start_cast  {"ws_url":"ws://rpi:8765"}
//   POST /api/stop_cast
//   GET  /api/status
class RemoteDisplayHttpServer {
public:
    RemoteDisplayHttpServer() = default;
    ~RemoteDisplayHttpServer();

    // Start HTTP server on given port and register mDNS service.
    bool Start(int port = 80);
    void Stop();

private:
    static esp_err_t HandleStartCast(httpd_req_t* req);
    static esp_err_t HandleStopCast(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);

    void RegisterMdns(int port);

    httpd_handle_t server_ = nullptr;
};

#endif // REMOTE_DISPLAY_HTTP_SERVER_H
