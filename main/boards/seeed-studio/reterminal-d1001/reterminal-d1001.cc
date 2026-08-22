#include "wifi_board.h"

#include "audio/codecs/no_audio_codec.h"
#include "config.h"

#include <esp_log.h>

#define TAG "ReTerminalD1001"

class ReTerminalD1001Board : public WifiBoard {
public:
    ReTerminalD1001Board() {
        ESP_LOGI(TAG, "reTerminal D1001 board skeleton");
    }

    virtual AudioCodec* GetAudioCodec() override {
        // Placeholder until the dedicated ES8311/ES7210 codec lands.
        // The D1001 uses separate TX (GPIO30-33) and RX (GPIO26-29) I2S
        // buses, so it cannot reuse BoxAudioCodec as-is.
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
        return &audio_codec;
    }
};

DECLARE_BOARD(ReTerminalD1001Board);