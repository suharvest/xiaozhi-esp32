#include "usb_tx_queue.h"

#include "usb_frame.h"

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>

#define TAG "UsbTx"

namespace usb_transport {

namespace {
constexpr int kDriverRxBuffer = 2048;
constexpr int kDriverTxBuffer = 4096;
constexpr TickType_t kWriteTimeout = pdMS_TO_TICKS(100);
}  // namespace

UsbTxQueue& UsbTxQueue::GetInstance() {
    static UsbTxQueue instance;
    return instance;
}

bool UsbTxQueue::Start() {
    if (task_ != nullptr) {
        return true;
    }
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = kDriverRxBuffer;
    cfg.tx_buffer_size = kDriverTxBuffer;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %d", (int)err);
        return false;
    }
    wakeup_ = xSemaphoreCreateBinary();
    if (wakeup_ == nullptr) {
        return false;
    }
    if (xTaskCreate(WriterTask, "usb_tx", 4096, this, 5, &task_) != pdPASS) {
        task_ = nullptr;
        return false;
    }
    return true;
}

bool UsbTxQueue::MakeRoomLocked(TxStats& counters) {
    // 1. logs first, they carry no timing meaning
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->type == usb_frame::kTypeLog) {
            queue_.erase(it);
            counters.dropped_log++;
            return true;
        }
    }
    // 2. then the oldest unpinned uplink audio; stale real-time audio is
    //    useless to the host
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->type == usb_frame::kTypeAudioUp && !it->pinned) {
            queue_.erase(it);
            counters.dropped_audio++;
            return true;
        }
    }
    // 3. uplink control may go too, but it has to be counted and reported
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->type == usb_frame::kTypeCtrlUp && !it->pinned) {
            queue_.erase(it);
            counters.dropped_ctrl++;
            return true;
        }
    }
    return false;
}

bool UsbTxQueue::Post(uint8_t type, const void* payload, size_t len, bool order_barrier) {
    Item item;
    item.type = type;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueuedFrames && !MakeRoomLocked(stats_)) {
            stats_.queue_full++;
            return false;
        }
        // SEQ is taken here so the wire order matches the queue order.
        if (!usb_frame::Encode(type, seq_, static_cast<const uint8_t*>(payload), len,
                               item.bytes)) {
            return false;
        }
        seq_++;
        if (order_barrier) {
            // Everything already queued must reach the host before this frame.
            for (auto& queued : queue_) {
                queued.pinned = true;
            }
            item.pinned = true;
        }
        queue_.push_back(std::move(item));
    }
    if (wakeup_ != nullptr) {
        xSemaphoreGive(wakeup_);
    }
    return true;
}

TxStats UsbTxQueue::TakeStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    TxStats out = stats_;
    out.depth = static_cast<uint32_t>(queue_.size());
    stats_.dropped_audio = 0;
    stats_.dropped_ctrl = 0;
    stats_.dropped_log = 0;
    stats_.queue_full = 0;
    stats_.write_errors = 0;
    return out;
}

void UsbTxQueue::WriterTask(void* arg) {
    static_cast<UsbTxQueue*>(arg)->Run();
}

void UsbTxQueue::Run() {
    std::vector<uint8_t> frame;
    for (;;) {
        xSemaphoreTake(wakeup_, portMAX_DELAY);
        // Drain whatever accumulated: one give may cover several posts.
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    break;
                }
                frame = std::move(queue_.front().bytes);
                queue_.pop_front();
            }
            size_t written = 0;
            while (written < frame.size()) {
                const int n = usb_serial_jtag_write_bytes(frame.data() + written,
                                                          frame.size() - written, kWriteTimeout);
                if (n <= 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stats_.write_errors++;
                    break;
                }
                written += static_cast<size_t>(n);
            }
        }
    }
}

}  // namespace usb_transport
