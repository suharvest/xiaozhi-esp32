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

    // 人脸识别相关方法
    bool SetCameraMode(CameraMode mode);
    bool SendFaceModeCommand(bool enable);
    void InitializeFaceMcpTools();

    // Pause/resume all SPI inference so external UART enrollment can take over Himax
    void PauseInference();
    void ResumeInference();

private:
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
};

#endif // ESP32_CAMERA_H
