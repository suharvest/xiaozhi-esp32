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
    MSG_TYPE_AUDIO_FRAME  = 0x02,   // 音频帧 (Opus) - 已废弃
    MSG_TYPE_AUDIO_PCM    = 0x03,   // 音频 PCM 数据
    MSG_TYPE_HEARTBEAT    = 0x04,   // 心跳
};

// 发现的显示设备信息
struct DiscoveredDisplay {
    std::string name;       // 设备名称（如 "客厅显示器"）
    std::string ip;         // IP 地址
    uint16_t port;          // 端口号
};

// 远程显示配置
struct RemoteDisplayConfig {
    bool enabled = false;
    std::string server_url;
    int timeout_ms = 3000;
};

// 远程显示服务 - 将 UI 状态同步到树莓派
class RemoteDisplay {
public:
    static RemoteDisplay* GetInstance();

    // 配置管理 (NVS 存储)
    static RemoteDisplayConfig LoadConfig();
    static void SaveConfig(const RemoteDisplayConfig& config);

    // 使用存储的配置启动
    bool StartWithConfig();

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

    // PCM 音频转发 - 由 AudioCodec::Write() 回调调用
    void ForwardPcmAudio(const int16_t* data, int samples, int sample_rate);

    // mDNS 发现本地网络上的投屏服务
    // timeout_ms: 搜索超时时间
    // 返回: 发现的设备列表
    std::vector<DiscoveredDisplay> DiscoverDisplays(int timeout_ms = 3000);

    // 使用简化 IP（最后一段）连接
    // suffix: IP 地址最后一段 (0-255)
    // port: 端口号，默认 8765
    bool ConnectWithIPSuffix(int suffix, int port = 8765);

    // 获取本机 IP 的网段前缀（如 192.168.1.）
    std::string GetIPPrefix();

private:
    RemoteDisplay() = default;
    ~RemoteDisplay();

    void SendUIState();
    void SendHello();

    std::unique_ptr<WebSocket> websocket_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::string current_server_url_;  // 当前连接的服务器 URL

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
