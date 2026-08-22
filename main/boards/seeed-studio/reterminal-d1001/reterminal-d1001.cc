#include "wifi_board.h"

#include "config.h"
#include "reterminal_d1001_audio_codec.h"
#include "reterminal_d1001_expander.h"

#include <esp_log.h>

#define TAG "ReTerminalD1001"

class ReTerminalD1001Board : public WifiBoard {
private:
    ReTerminalD1001Expander expander_;
    ReTerminalD1001AudioCodec* audio_codec_ = nullptr;

public:
    ReTerminalD1001Board() {
        ESP_LOGI(TAG, "initializing reTerminal D1001");

        // Minimal power bring-up: I2C1 + PCA9535, power hold and panel
        // rails. The power amplifier stays off until the audio path is
        // ready (pop-free order).
        expander_.Initialize();
        expander_.ApplyMinimalPowerSequence();

        audio_codec_ = new ReTerminalD1001AudioCodec(expander_.GetI2cBus(), AUDIO_INPUT_SAMPLE_RATE,
                                                     AUDIO_OUTPUT_SAMPLE_RATE);
        audio_codec_->SetPowerAmpCallback([this](bool on) { expander_.SetPowerAmp(on); });
    }

    virtual AudioCodec* GetAudioCodec() override { return audio_codec_; }
};

DECLARE_BOARD(ReTerminalD1001Board);
