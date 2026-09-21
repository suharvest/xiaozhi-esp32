#ifndef _USB_TX_QUEUE_H_
#define _USB_TX_QUEUE_H_

// Bounded application-level send queue with a single writer task.
//
// The USB-Serial-JTAG driver enqueues a whole write or nothing, and anything
// already handed to it cannot be taken back, so the drop policy can only act
// here (spec §5.1). Every producer - audio, control, logs - posts whole frames
// into this one queue and one task writes them out, so frames never interleave.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace usb_transport {

// Spec §5.2 suggests 32 frames; at 50 audio frames per second that is 640 ms
// of buffering before the drop policy starts working.
constexpr size_t kMaxQueuedFrames = 32;

struct TxStats {
    uint32_t dropped_audio = 0;  // since the last TakeStats()
    uint32_t dropped_ctrl = 0;
    uint32_t dropped_log = 0;
    uint32_t queue_full = 0;     // nothing droppable, the new frame was refused
    uint32_t write_errors = 0;
    uint32_t depth = 0;          // current depth, not reset
};

class UsbTxQueue {
public:
    static UsbTxQueue& GetInstance();

    // Installs the USB-Serial-JTAG driver and starts the writer task.
    bool Start();
    bool started() const { return task_ != nullptr; }

    // Encodes and enqueues one frame. SEQ is assigned here, at enqueue time,
    // so a gap in SEQ always means a real drop (spec §1.3).
    //
    // `order_barrier` is for asr_eos and abort: they carry timing meaning and
    // must not overtake audio that was queued before them, so everything
    // already in the queue becomes undroppable (spec §5.2).
    bool Post(uint8_t type, const void* payload, size_t len, bool order_barrier = false);

    // Restarts the sequence counter. Spec §6 and §7.6 item 6: a reconnect or a
    // fresh handshake starts over at 0, otherwise the peer records the first
    // frame as a phantom SEQ jump.
    void ResetSeq();

    // Reads and clears the incremental counters.
    TxStats TakeStats();

private:
    UsbTxQueue() = default;

    struct Item {
        uint8_t type = 0;
        bool pinned = false;  // protected by an order barrier
        std::vector<uint8_t> bytes;
    };

    static void WriterTask(void* arg);
    void Run();
    // Caller holds mutex_. Returns false when nothing could be dropped.
    bool MakeRoomLocked(TxStats& counters);

    std::deque<Item> queue_;
    std::mutex mutex_;
    SemaphoreHandle_t wakeup_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint8_t seq_ = 0;
    TxStats stats_;
};

}  // namespace usb_transport

#endif  // _USB_TX_QUEUE_H_
