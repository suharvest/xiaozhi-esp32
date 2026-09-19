#ifndef _XVF3800_CONTROL_H_
#define _XVF3800_CONTROL_H_

#include <driver/i2c_master.h>
#include <cstdint>

// Read-only client for the XVF3800 host control interface over I2C.
//
// Frame format (reSpeaker's own ESP32-S3 reference implementation):
//   write: [resid] [cmd]        [payload_len]   [payload...]
//   read : [resid] [cmd | 0x80] [read_len + 1]  -- STOP -- then read
//          (read_len + 1) bytes back, of which byte 0 is a status code
//          (0 = success, 64 = retry) and the rest is the payload.
class Xvf3800Control {
public:
    bool Initialize(i2c_master_bus_handle_t bus, uint8_t address);
    // Reads `len` payload bytes of the (resid, cmd) parameter.
    bool Read(uint8_t resid, uint8_t cmd, uint8_t* out, uint8_t len);
    // Reads everything that tells us what the two I2S slots carry and how the
    // gain chain is set, and logs it.
    void DumpConfiguration();
    // Writes `len` payload bytes to the (resid, cmd) parameter.
    bool Write(uint8_t resid, uint8_t cmd, const uint8_t* payload, uint8_t len);
    // Applies the board's tuning overrides, one parameter at a time, reading
    // the value back before and after each write.
    void ApplyTuning();

private:
#if CONFIG_XVF3800_APPLY_TUNING
    bool TuneFloat(const char* name, uint8_t resid, uint8_t cmd, float target);
#endif
    i2c_master_dev_handle_t dev_ = nullptr;
};

#endif  // _XVF3800_CONTROL_H_
