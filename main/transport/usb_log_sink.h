#ifndef _USB_LOG_SINK_H_
#define _USB_LOG_SINK_H_

// Redirects ESP-IDF logging into LOG frames on the USB link (spec §4).
//
// Everything that reaches the serial port has to go through the one send
// queue, otherwise a log line lands in the middle of a binary frame. Logging
// therefore never writes to the port itself.
//
// Known limits, stated in the spec: ROM, bootloader and second-stage output
// cannot be intercepted and appears as plain text at the head of the stream,
// and any code calling printf or fwrite(stdout) directly bypasses this hook.

namespace usb_transport {

// Installs the vprintf hook. Call after the send queue is running.
void InstallUsbLogSink();

}  // namespace usb_transport

#endif  // _USB_LOG_SINK_H_
