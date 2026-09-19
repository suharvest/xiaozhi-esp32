#include "no_audio_codec.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#define TAG "NoAudioCodec"

#if CONFIG_AUDIO_INPUT_LEVEL_DEBUG
// Per-slot statistics over a ~1s window, so the two slots of a stereo frame
// can be compared directly. Values are shifted to the int16 scale first so the
// numbers line up with what the upper layers see.
static void LogSlotLevels(const int32_t* buf, int frames, int words_per_frame, int sample_rate) {
    static uint64_t sum_sq[2] = {0, 0};
    static int32_t peak[2] = {0, 0};
    static int32_t vmin[2] = {INT16_MAX, INT16_MAX};
    static int32_t vmax[2] = {INT16_MIN, INT16_MIN};
    static uint32_t count = 0;
    static uint32_t window = 0;

    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < words_per_frame; c++) {
            int32_t v = buf[i * words_per_frame + c] >> CONFIG_AUDIO_I2S_INPUT_SHIFT;
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < -INT16_MAX) v = -INT16_MAX;
            int32_t a = v < 0 ? -v : v;
            if (a > peak[c]) peak[c] = a;
            if (v < vmin[c]) vmin[c] = v;
            if (v > vmax[c]) vmax[c] = v;
            sum_sq[c] += (uint64_t)(v * v);
        }
    }
    count += frames;
    if (count < (uint32_t)sample_rate) {
        return;
    }

    ++window;
    for (int c = 0; c < words_per_frame; c++) {
        uint32_t rms = (uint32_t)sqrt((double)sum_sq[c] / (double)count);
        ESP_LOGW(TAG, "slot level #%u ch%d: rms=%u peak=%d min=%d max=%d n=%u",
                 (unsigned)window, c, (unsigned)rms, (int)peak[c], (int)vmin[c],
                 (int)vmax[c], (unsigned)count);
        sum_sq[c] = 0;
        peak[c] = 0;
        vmin[c] = INT16_MAX;
        vmax[c] = INT16_MIN;
    }
    count = 0;
}
#endif

NoAudioCodec::~NoAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}

NoAudioCodecDuplex::NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
#if CONFIG_AUDIO_I2S_MSB_JUSTIFIED
    // MSB/left-justified: data starts on the WS edge instead of one BCLK after.
    std_cfg.slot_cfg.bit_shift = false;
#endif
#ifdef I2S_HW_VERSION_2
#if !CONFIG_AUDIO_I2S_LEFT_ALIGN
    std_cfg.slot_cfg.left_align = false;
#endif
#endif
#if CONFIG_AUDIO_I2S_WS_WIDTH != 32
    std_cfg.slot_cfg.ws_width = CONFIG_AUDIO_I2S_WS_WIDTH;
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
#if CONFIG_AUDIO_I2S_INPUT_STEREO
    // Only RX switches to stereo. The line format is identical either way
    // (two 32-bit slots per frame); mono RX simply discards the right slot.
    // TX keeps the mono config so Write() needs no change.
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    input_stereo_ = true;
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created (input_stereo=%d)", (int)input_stereo_);
#if CONFIG_AUDIO_INPUT_LEVEL_DEBUG
    // Which channel actually owns the clock is not exposed directly, but the
    // reported BCLK tells whether a channel computed its own clock or not.
    i2s_chan_info_t info = {};
    if (i2s_channel_get_info(tx_handle_, &info) == ESP_OK) {
        ESP_LOGW(TAG, "i2s tx info: bclk=%u Hz mclk=%u Hz sclk=%u Hz",
                 (unsigned)info.bclk_hz, (unsigned)info.mclk_hz, (unsigned)info.sclk_hz);
    }
    if (i2s_channel_get_info(rx_handle_, &info) == ESP_OK) {
        ESP_LOGW(TAG, "i2s rx info: bclk=%u Hz mclk=%u Hz sclk=%u Hz",
                 (unsigned)info.bclk_hz, (unsigned)info.mclk_hz, (unsigned)info.sclk_hz);
    }
#endif
}


NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // Create a new channel for MIC
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din, i2s_std_slot_mask_t mic_slot_mask){
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = spk_slot_mask,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // Create a new channel for MIC
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.slot_cfg.slot_mask = mic_slot_mask;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

int NoAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    std::vector<int32_t> buffer(samples);

    // output_volume_: 0-100
    // volume_factor_: 0-65536
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    for (int i = 0; i < samples; i++) {
        int64_t temp = int64_t(data[i]) * volume_factor; // 使用 int64_t 进行乘法运算
        if (temp > INT32_MAX) {
            buffer[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
            buffer[i] = INT32_MIN;
        } else {
            buffer[i] = static_cast<int32_t>(temp);
        }
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    return bytes_written / sizeof(int32_t);
}

int NoAudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    constexpr uint32_t kReadTimeoutMs = 200;

    // `samples` is how many int16 values the caller wants in `dest`. The codec
    // reports one input channel, so that is also the frame count. One frame is
    // one 32-bit slot word in mono capture, two (left then right) in stereo.
    const int words_per_frame = input_stereo_ ? 2 : 1;
    std::vector<int32_t> bit32_buffer(static_cast<size_t>(samples) * words_per_frame);
    const size_t want_bytes = bit32_buffer.size() * sizeof(int32_t);
    bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle_, bit32_buffer.data(), want_bytes,
                                     &bytes_read, kReadTimeoutMs);
    // A silent `return 0` here hides the difference between "microphone is
    // quiet" and "nothing is arriving at all". Rate limited to one line per
    // second so a permanently failing read cannot flood the log.
    static int64_t last_read_log_us = 0;
    static uint32_t read_fail_count = 0;
    static uint32_t short_read_count = 0;
    if (err != ESP_OK) {
        read_fail_count++;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_read_log_us >= 1000000) {
            last_read_log_us = now_us;
            ESP_LOGE(TAG, "i2s_channel_read failed: %s (want %u bytes, got %u, %u failures)",
                     esp_err_to_name(err), (unsigned)want_bytes, (unsigned)bytes_read,
                     (unsigned)read_fail_count);
        }
        return 0;
    }
    if (bytes_read < want_bytes) {
        short_read_count++;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_read_log_us >= 1000000) {
            last_read_log_us = now_us;
            ESP_LOGW(TAG, "i2s_channel_read short: want %u bytes, got %u (%u short reads)",
                     (unsigned)want_bytes, (unsigned)bytes_read, (unsigned)short_read_count);
        }
    }

    const int words_read = bytes_read / sizeof(int32_t);
#if CONFIG_AUDIO_I2S_INPUT_BIT_OFFSET > 0
    // The payload's sign bit sits CONFIG_AUDIO_I2S_INPUT_BIT_OFFSET bits below
    // the top of each captured word, so each word carries the tail of one
    // sample and the head of the next. Re-slice the stream that many bits
    // later. The word preceding this buffer arrives from the previous read,
    // which costs one slot of delay but keeps the phase stable.
    {
        constexpr int kOff = CONFIG_AUDIO_I2S_INPUT_BIT_OFFSET;
        uint32_t prev = bit_carry_;
        for (int i = 0; i < words_read; i++) {
            const uint32_t cur = static_cast<uint32_t>(bit32_buffer[i]);
            bit32_buffer[i] = static_cast<int32_t>((prev << kOff) | (cur >> (32 - kOff)));
            prev = cur;
        }
        bit_carry_ = prev;
    }
#endif
    // Integer division drops a trailing half frame rather than desyncing.
    const int frames = words_read / words_per_frame;
    for (int i = 0; i < frames; i++) {
        // In stereo only the first slot of the frame goes upstream.
        int32_t value = bit32_buffer[i * words_per_frame] >> CONFIG_AUDIO_I2S_INPUT_SHIFT;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
    }
#if CONFIG_AUDIO_INPUT_LEVEL_DEBUG
    // One-shot: the raw slot words next to the int16 they turn into. Tells
    // where the payload bits sit and, in stereo, what each slot carries.
    static bool raw_dumped = false;
    if (!raw_dumped && frames >= 32) {
        raw_dumped = true;
        ESP_LOGW(TAG, "raw i2s dump, shift=%d stereo=%d (index 0 may be a start-up artifact)",
                 CONFIG_AUDIO_I2S_INPUT_SHIFT, (int)input_stereo_);
        for (int i = 0; i < 32; i++) {
            const int32_t w0 = bit32_buffer[i * words_per_frame];
            if (words_per_frame == 2) {
                const int32_t w1 = bit32_buffer[i * words_per_frame + 1];
                ESP_LOGW(TAG, "raw i2s[%2d] ch0=0x%08x fs=%+.4f  ch1=0x%08x fs=%+.4f", i,
                         (unsigned)w0, (double)w0 / 2147483648.0,
                         (unsigned)w1, (double)w1 / 2147483648.0);
            } else {
                ESP_LOGW(TAG, "raw i2s[%2d] = 0x%08x  fs=%+.4f  -> %d", i,
                         (unsigned)w0, (double)w0 / 2147483648.0, (int)dest[i]);
            }
        }
    }
    LogSlotLevels(bit32_buffer.data(), frames, words_per_frame, input_sample_rate_);
#endif
    return frames;
}

void NoAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
#if CONFIG_AUDIO_I2S_TX_ALWAYS_ENABLED
        // Full-duplex STD shares BCLK/WS and the driver drives them from the
        // TX channel, so RX alone leaves the bus unclocked and every read
        // times out. Bring TX up as the clock source; it sends zeros while
        // nothing is being played.
        if (duplex_ && !output_enabled_ && !tx_enabled_for_clock_) {
            ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
            tx_enabled_for_clock_ = true;
            ESP_LOGI(TAG, "TX enabled as the shared I2S clock source");
        }
#endif
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
#if CONFIG_AUDIO_I2S_TX_ALWAYS_ENABLED
        if (tx_enabled_for_clock_) {
            ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
            tx_enabled_for_clock_ = false;
        }
#endif
    }
    AudioCodec::EnableInput(enable);
}

void NoAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
#if CONFIG_AUDIO_I2S_TX_ALWAYS_ENABLED
        if (tx_enabled_for_clock_) {
            // Already running as the clock source; hand it over to playback
            // instead of enabling it twice.
            tx_enabled_for_clock_ = false;
            AudioCodec::EnableOutput(enable);
            return;
        }
#endif
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
#if CONFIG_AUDIO_I2S_TX_ALWAYS_ENABLED
        if (duplex_ && input_enabled_) {
            // The input still needs the shared clock, so keep TX running
            // rather than stopping it and losing the clock mid-capture.
            tx_enabled_for_clock_ = true;
            AudioCodec::EnableOutput(enable);
            return;
        }
#endif
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}

// Delegating constructor: calls the main constructor with default slot mask
NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_din) 
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout, I2S_STD_SLOT_LEFT, mic_sck, mic_din) {
    // All initialization is handled by the delegated constructor
}

NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(1), I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    tx_chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    tx_chan_cfg.auto_clear_after_cb = true;
    tx_chan_cfg.auto_clear_before_cb = false;
    tx_chan_cfg.intr_priority = 0;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, NULL));


    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = spk_slot_mask,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));
#if SOC_I2S_SUPPORTS_PDM_RX
    // Create a new channel for MIC in PDM mode
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(0), I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle_));
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        /* The data bit-width of PDM mode is fixed to 16 */
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = mic_sck,
            .din = mic_din,

            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_rx_cfg));
#else
    ESP_LOGE(TAG, "PDM is not supported");
#endif
    ESP_LOGI(TAG, "Simplex channels created");
}

int NoAudioCodecSimplexPdm::Read(int16_t* dest, int samples) {
    size_t bytes_read;

    // PDM 解调后的数据位宽为 16 位，直接读取到目标缓冲区
    if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }

    samples = bytes_read / sizeof(int16_t);
    if (input_gain_ > 0) {
        int gain_factor = (int)input_gain_;
        for (int i = 0; i < samples; i++) {
            int32_t amplified = dest[i] * gain_factor;
            dest[i] = (amplified > INT16_MAX) ? INT16_MAX : (amplified < -INT16_MAX) ? -INT16_MAX : (int16_t)amplified;
        }
    }
    return samples;
}
