#ifndef _V2V_PROTOCOL_H_
#define _V2V_PROTOCOL_H_

// Protocol implementation over the USB-Serial-JTAG link (usb-frame-spec v0.3).
//
// The device is the client and the host agent is the server. The agent
// translates between this link and the upstream SLV server, so the vocabulary
// here is the v2v one, not the xiaozhi WebSocket one.

#include "protocol.h"
#include "usb_frame.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string>

class V2vProtocol : public Protocol {
public:
    V2vProtocol();
    ~V2vProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    // Spec §3.2 replaces the xiaozhi vocabulary on this link.
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;

protected:
    bool SendText(const std::string& text) override;

private:
    static void RxTaskEntry(void* arg);
    void RxTask();
    void HandleFrame(const usb_frame::Frame& frame);
    void HandleCtrlDown(const std::string& json);
    void ApplyMode(const std::string& wake, const std::string& vad);
    void SendConfig();
    void SendPing(uint32_t now_ms);
    void SendStats();
    // order_barrier marks asr_eos and abort, which must not overtake audio
    // that was queued before them (spec §5.2).
    bool SendCtrl(const std::string& json, bool order_barrier);

    usb_frame::Decoder decoder_;
    TaskHandle_t rx_task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> audio_channel_opened_{false};

    bool handshaked_ = false;
    uint32_t last_config_ms_ = 0;
    uint32_t last_ping_ms_ = 0;
    uint32_t last_stats_ms_ = 0;
    bool stats_due_ = false;  // set by abort, so the flushed depth is reported at once
    int missed_pongs_ = 0;

    // Mode actually in effect, capped by what this board can do.
    std::string mode_wake_ = "device";
    std::string mode_vad_ = "device";
};

#endif  // _V2V_PROTOCOL_H_
