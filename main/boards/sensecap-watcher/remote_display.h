#ifndef REMOTE_DISPLAY_H
#define REMOTE_DISPLAY_H

#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WebSocket;
class LvglDisplay;

// 远程显示消息类型
enum RemoteDisplayMessageType : uint8_t {
    MSG_TYPE_SCREEN_FRAME = 0x01,   // 屏幕帧 (JPEG)
    MSG_TYPE_AUDIO_FRAME  = 0x02,   // 音频帧 (Opus)
    MSG_TYPE_HEARTBEAT    = 0x04,   // 心跳
};

// 远程显示服务 - 将屏幕和音频转发到树莓派
class RemoteDisplay {
public:
    RemoteDisplay(LvglDisplay* display);
    ~RemoteDisplay();

    // 尝试连接到远程服务器，失败返回 false
    bool Start(const std::string& server_url, int fps, int quality, int timeout_ms = 3000);
    void Stop();
    bool IsRunning() const { return running_; }

    // 音频转发 - 由 AudioService 回调调用
    void ForwardAudioPacket(const std::vector<uint8_t>& opus_data,
                            int sample_rate, int frame_duration);

private:
    void ScreenCaptureTask();
    bool SendScreenFrame(const std::string& jpeg_data);
    bool SendAudioFrame(const std::vector<uint8_t>& opus_data,
                        int sample_rate, int frame_duration);
    void SendHello();

    LvglDisplay* display_;
    std::unique_ptr<WebSocket> websocket_;
    TaskHandle_t task_handle_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    int fps_ = 5;
    int quality_ = 50;

    std::mutex send_mutex_;
};

#endif // REMOTE_DISPLAY_H
