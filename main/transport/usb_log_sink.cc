#include "usb_log_sink.h"

#include "usb_frame.h"
#include "usb_tx_queue.h"

#include <esp_log.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace usb_transport {

namespace {

// One line at most, matching the frame payload limit. Kept out of the stack:
// the hook runs on whatever task happened to log.
char g_line[usb_frame::kMaxPayload + 1];
std::mutex g_line_mutex;

// The hook must never log. Anything that goes wrong inside is dropped in
// silence, otherwise a failure to build the frame would recurse forever.
thread_local bool t_inside = false;

int UsbLogVprintf(const char* fmt, va_list args) {
    if (t_inside) {
        return 0;
    }
    t_inside = true;
    int written = 0;
    {
        std::unique_lock<std::mutex> lock(g_line_mutex, std::try_to_lock);
        if (lock.owns_lock()) {
            const int n = vsnprintf(g_line, sizeof(g_line), fmt, args);
            if (n > 0) {
                // vsnprintf reports what it would have written; over the limit
                // the line is truncated, never split across frames.
                size_t len = static_cast<size_t>(n) >= sizeof(g_line) ? sizeof(g_line) - 1
                                                                      : static_cast<size_t>(n);
                while (len > 0 && (g_line[len - 1] == '\n' || g_line[len - 1] == '\r')) {
                    len--;
                }
                if (len > 0) {
                    UsbTxQueue::GetInstance().Post(usb_frame::kTypeLog, g_line, len);
                }
                written = n;
            }
        }
    }
    t_inside = false;
    return written;
}

}  // namespace

void InstallUsbLogSink() {
    esp_log_set_vprintf(&UsbLogVprintf);
}

}  // namespace usb_transport
