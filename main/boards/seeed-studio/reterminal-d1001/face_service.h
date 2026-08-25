#ifndef RETERMINAL_D1001_FACE_SERVICE_H
#define RETERMINAL_D1001_FACE_SERVICE_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class EspVideo;

// One recognized face from the external recognizer.
struct FaceHit {
    std::string name;  // "unknown" = face detected but not in the library
    int score = 0;     // 0-100
};

// Face recognition backed by an external HTTP endpoint (the D1001 has no
// NPU). Mirrors the SenseCAP Watcher port's detection state machine
// (IDLE -> VALIDATING -> wake -> COOLDOWN) but sends JPEG frames to a
// configurable recognizer instead of running a local model.
//
// Endpoint contract:
//   POST <endpoint>  body: image/jpeg
//   200 -> {"faces": [{"name": "harvest", "score": 92}, ...]}
//   score accepts 0-1 floats or 0-100 numbers; empty faces = nobody there.
//
// Two consumers:
//   1. Wake mode: a face persisting for duration_s triggers
//      Application::WakeWordInvoke("<detect>...</detect>").
//   2. MCP tool self.face.verify: on-demand identification so the assistant
//      can authenticate the operator before sensitive tasks.
class FaceService {
public:
    explicit FaceService(EspVideo* camera);

    // Loads config, registers the MCP tools and spawns the watch task.
    void Start();

    // One synchronous capture + recognition round. Returns false and fills
    // `error` on capture/network/parse failure.
    bool RecognizeOnce(std::vector<FaceHit>& hits, std::string* error = nullptr);

    // Flips wake-on-face between off (0) and on (1); persists and returns
    // the new mode.
    int ToggleMode();
    int GetMode() const { return mode_.load(); }

    std::string StatusJson();
    bool ApplyConfigJson(const std::string& body, std::string* error);

private:
    void WatchLoop();
    void LoadConfig();
    void RegisterMcpTools();
    bool AnyQualifiedHit(const std::vector<FaceHit>& hits, std::string* names);

    EspVideo* camera_;
    TaskHandle_t task_ = nullptr;
    std::mutex mutex_;  // endpoint_ + last_result_

    // Settings namespace "face"
    std::atomic<int> mode_{0};        // 0 = off, 1 = wake-on-face
    std::atomic<int> interval_s_{5};  // seconds between recognitions
    std::atomic<int> threshold_{60};  // min score
    std::atomic<int> duration_s_{2};  // face must persist this long to wake
    std::atomic<int> cooldown_s_{8};  // after a wake, wait this long + face gone
    std::atomic<int> known_only_{1};  // 1 = "unknown" faces do not wake
    std::atomic<int> api_{0};         // 0 = raw JPEG POST, 1 = {"image_base64"} JSON
    std::string endpoint_;

    std::string last_result_ = "[]";
    std::atomic<int64_t> last_ts_ms_{0};
};

#endif  // RETERMINAL_D1001_FACE_SERVICE_H
