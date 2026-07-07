#ifndef SSCMA_CAMERA_H
#define SSCMA_CAMERA_H

#include <cstdint>
#include <lvgl.h>
#include <thread>
#include <memory>
#include <atomic>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_jpeg_dec.h>
#include <mbedtls/base64.h>

#include "sscma_client.h"
#include "camera.h"
#include "face_database.h"
#include "face_recognition.h"

struct SscmaData {
    uint8_t* img;
    size_t len;
};
struct JpegData {
    uint8_t* buf;
    size_t len;
};

class SscmaCamera : public Camera {
private:
    lv_img_dsc_t preview_image_;
    std::string explain_url_;
    std::string explain_token_;
    sscma_client_io_handle_t sscma_client_io_handle_;
    sscma_client_handle_t sscma_client_handle_;
    QueueHandle_t sscma_data_queue_;
    JpegData jpeg_data_;
    jpeg_dec_handle_t jpeg_dec_;
    jpeg_dec_io_t *jpeg_io_;
    jpeg_dec_header_info_t *jpeg_out_;
    // 检测状态机
    enum DetectionState {
        IDLE,           // 空闲状态
        VALIDATING,     // 验证中（连续检测3秒）
        COOLDOWN        // 冷却期（等待重新检测）
    };

    // 摄像头工作模式
    enum CameraMode {
        MODE_OBJECT_DETECT,     // 物体检测模式
        MODE_FACE_RECOGNITION   // 人脸识别模式
    };
    
    DetectionState detection_state = IDLE;
    int64_t state_start_time = 0;
    bool need_start_cooldown = false; // 是否需要开始冷却期
    int64_t last_detected_time = 0; // 验证期间最后一次检测到物体的时间
    
    std::atomic<int> detect_target{0};
    std::atomic<int> detect_threshold{75};
    std::atomic<int> detect_duration_sec{3}; // 检测持续时间3秒，确认人员持续存在
    std::atomic<int> detect_invoke_interval_sec{5}; // 默认5秒冷却期，避免频繁开始会话
    int detect_debounce_sec = 1; // 验证期间人员离开的去抖动时间1秒
    std::atomic<int> inference_en{0}; // 推理使能开关（0: 关闭, 1: 开启）
    std::atomic<bool> sscma_restarted_{false};
    
    sscma_client_model_t *model;
    int model_class_cnt = 0;

    // 人脸识别相关
    CameraMode camera_mode_ = MODE_OBJECT_DETECT;
    std::atomic<bool> face_recognition_en_{false};  // 待命时人脸识别开关
    // Actually-applied vision wake mode (VisionWakeMode value), updated by the
    // camera main task at the points where a mode really starts/stops running.
    // May lag the configured intent (GetVisionWakeMode) e.g. during a voice
    // conversation, when face/object engines are suspended. Read by the
    // self.vision.mode MCP tool to report status active/switching.
    std::atomic<int> applied_vision_mode_{0};
public:
    SscmaCamera(esp_io_expander_handle_t io_exp_handle);
    ~SscmaCamera();
    void InitializeMcpTools();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);
    virtual std::string FaceRecognition();

    // Configured standby mode (persisted face_recognition_en_), used by the
    // watcher status-bar mode indicator. Stable across device state.
    bool IsFaceRecognitionEnabled() const { return face_recognition_en_.load(); }

    // Wake-window guard: when true, sscma_client drops incoming event frames
    // (type:1) before cJSON parsing, so the ~8KB-per-frame parse tree cannot
    // starve internal SRAM while the wake path runs its TLS handshake.
    // Set on every wake trigger (face/object), cleared by the camera main loop
    // whenever it (re)configures a mode, and unconditionally by single-shot
    // face embeds (which need their own type:1 event to get through).
    static void SetDropEvents(bool drop);

    // Unified proactive-wake mode — a single switch over the two underlying
    // detection engines (object detection + face recognition). The four modes
    // are mutually exclusive and persisted across the existing NVS keys
    // ("model"/"enable", "face"/"enable", "face"/"familiar_mode").
    enum VisionWakeMode {
        VISION_OFF      = 0,  // no proactive wake (both engines off)
        VISION_OBJECT   = 1,  // wake on object detection (inference_en)
        VISION_FACE     = 2,  // face recognition, greet everyone
        VISION_FACE_DND = 3,  // face recognition, DND: only strangers wake
    };
    // Atomically apply a wake mode: writes the three underlying switches,
    // persists them, and syncs the FaceRecognition state machine.
    void SetVisionWakeMode(int mode);
    // Derive the current wake mode from the underlying switches.
    int GetVisionWakeMode() const;

    // 人脸识别相关方法
    bool SetCameraMode(CameraMode mode);
    bool SendFaceModeCommand(bool enable);
    void InitializeFaceMcpTools();

    // Pause/resume all SPI inference so external UART enrollment can take over Himax
    void PauseInference();
    void ResumeInference();

    // Bench: single-shot face embedding (path Z).
    // Drives Himax through one face inference, captures the embedding in
    // `out_embedding` (FACE_EMBEDDING_DIM floats), records phase timings
    // (us) in `out_timing_us` if non-null, and bypasses the voting buffer.
    // Returns true if a face was found within ~3 s, false on timeout.
    // Thread-safety: serialized with single_shot_pending_; concurrent calls
    // return false.
    struct SingleShotTiming {
        int64_t at_face_us = 0;       // sscma_break + AT+FACE=1 + sensor setup
        int64_t invoke_to_result_us = 0;  // sscma_invoke(1) -> on_event embedding ready
        int64_t teardown_us = 0;      // AT+FACE=0 + break
        int64_t total_us = 0;
        int   face_score = 0;
        float face_quality = 0.0f;
    };
    bool BenchSingleShotFaceEmbedding(float* out_embedding, SingleShotTiming* out_timing);

    // Gate for HTTP/MCP callers. Returns nullptr if a single-shot face embed is
    // allowed right now, otherwise a short machine-readable reason:
    //   "upgrading"       — OTA / firmware upgrade in progress (must not touch Himax)
    //   "greeting_active" — passive greeting is actively running (idle + enabled);
    //                       external face calls are not supported in that mode
    //   "busy"            — UART enrollment, photo capture, or another single-shot
    // Allowed during a voice conversation (the intended use → warm path).
    const char* FaceEmbedBlockReason() const;

private:
    static SscmaCamera* instance_;  // for the static SetDropEvents accessor
    bool paused_inference_en_ = false;
    bool paused_face_recognition_en_ = false;
    std::atomic<bool> inference_paused_{false};
    std::atomic<int64_t> inference_paused_at_{0};
    std::atomic<bool> capture_in_progress_{false};
    std::atomic<int64_t> capture_started_at_{0};
    std::mutex sscma_mutex_;
    std::mutex pause_state_mutex_;
    static constexpr int INFERENCE_PAUSE_TIMEOUT_SEC = 300;  // 5 min auto-resume
    static constexpr int CAPTURE_TIMEOUT_SEC = 30;  // 30s capture timeout

    // Bench (path Z) single-shot state. on_event populates these when
    // single_shot_pending_ is set; BenchSingleShotFaceEmbedding waits on
    // single_shot_valid_.
    std::atomic<bool> single_shot_pending_{false};
    std::atomic<bool> single_shot_valid_{false};
    float single_shot_embedding_[FACE_EMBEDDING_DIM] = {0};
    std::atomic<int> single_shot_face_score_{0};
    std::atomic<int> single_shot_face_quality_x1000_{0};  // quality * 1000, int for atomicity

    // Hot-standby (热待命): true means Himax is left resident in face mode with
    // the sensor warm at 240x240 (AT+FACE=1 active, invoke broken). A single-shot
    // can then skip the ~400ms cold setup (break + AT+FACE=1 + set_sensor + 200ms
    // settle). Only trusted while the device is voice-busy (conversation), because
    // the main camera task does not touch Himax in that state. The main task clears
    // this flag whenever it reconfigures the sensor, so a stale warm flag can never
    // drive a single-shot against a mismatched sensor/mode.
    std::atomic<bool> himax_face_warm_{false};
};

#endif // ESP32_CAMERA_H
