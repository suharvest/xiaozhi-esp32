#ifndef REMOTE_DISPLAY_H
#define REMOTE_DISPLAY_H

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WebSocket;

// 远程显示消息类型
enum RemoteDisplayMessageType : uint8_t {
    MSG_TYPE_UI_STATE     = 0x10,   // UI 状态 (JSON)
    MSG_TYPE_AUDIO_FRAME  = 0x02,   // 音频帧 (Opus)
    MSG_TYPE_HEARTBEAT    = 0x04,   // 心跳
};

// 远程显示服务 - 将 UI 状态同步到树莓派
class RemoteDisplay {
public:
    static RemoteDisplay* GetInstance();

    // 尝试连接到远程服务器，失败返回 false
    bool Start(const std::string& server_url, int timeout_ms = 3000);
    void Stop();
    bool IsRunning() const { return running_; }

    // UI 状态发送接口
    void SendEmotion(const char* emotion);
    void SendStatus(const char* status);
    void SendChatMessage(const char* role, const char* content);
    void SendTheme(const char* theme_name);
    void SendVolume(int volume, bool muted);

    // 图像发送接口
    void SendPreviewImage(const uint8_t* jpeg_data, size_t size);

    // 音频转发 - 由 AudioService 回调调用
    void ForwardAudioPacket(const std::vector<uint8_t>& opus_data,
                            int sample_rate, int frame_duration);

private:
    RemoteDisplay() = default;
    ~RemoteDisplay();

    void SendUIState();
    bool SendAudioFrame(const std::vector<uint8_t>& opus_data,
                        int sample_rate, int frame_duration);
    void SendHello();

    std::unique_ptr<WebSocket> websocket_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // UI 状态缓存
    std::string current_emotion_;
    std::string current_status_;
    std::string current_chat_role_;
    std::string current_chat_text_;
    std::string current_theme_;
    int current_volume_ = 70;
    bool current_muted_ = false;

    std::mutex state_mutex_;
    std::mutex send_mutex_;
};

#endif // REMOTE_DISPLAY_H
