#ifndef _SENSECAP_AUDIO_CODEC_H
#define _SENSECAP_AUDIO_CODEC_H

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <functional>
#include <mutex>

class SensecapAudioCodec : public AudioCodec {
public:
    // PCM 转发回调类型: (pcm_data, samples, sample_rate)
    using PcmForwardCallback = std::function<void(const int16_t*, int, int)>;

    // 设置 PCM 转发回调（用于远程显示等功能）
    void SetPcmForwardCallback(PcmForwardCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        pcm_forward_callback_ = callback;
    }

private:
    PcmForwardCallback pcm_forward_callback_;
    std::mutex callback_mutex_;
    std::mutex data_if_mutex_;
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    gpio_num_t pa_pin_ = GPIO_NUM_NC;
    
    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    void OpenInputDev();
    void OpenOutputDev();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    SensecapAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference);
    virtual ~SensecapAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _SENSECAP_AUDIO_CODEC_H
