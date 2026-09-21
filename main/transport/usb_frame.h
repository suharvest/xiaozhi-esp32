#ifndef _USB_FRAME_H_
#define _USB_FRAME_H_

// Wire framing for the ReSpeaker Flex <-> host USB link.
//
//   offset 0  SYNC0   0xA5
//   offset 1  SYNC1   0x5A
//   offset 2  TYPE
//   offset 3  SEQ
//   offset 4  LEN     payload bytes, little endian, 0..2048
//   offset 6  PAYLOAD
//   offset 6+LEN CRC  CRC-16/CCITT-FALSE over TYPE..PAYLOAD, little endian
//
// This header deliberately pulls in nothing from ESP-IDF so the same source
// can be compiled and tested on a host.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace usb_frame {

constexpr uint8_t kSync0 = 0xA5;
constexpr uint8_t kSync1 = 0x5A;
constexpr size_t kHeaderSize = 6;
constexpr size_t kCrcSize = 2;
constexpr size_t kOverhead = kHeaderSize + kCrcSize;
constexpr uint16_t kMaxPayload = 2048;
constexpr size_t kMaxRxBuffer = 8 * 1024;
constexpr uint32_t kCandidateTimeoutMs = 200;

enum Type : uint8_t {
    kTypeLog = 0x01,
    kTypeAudioUp = 0x10,
    kTypeAudioDown = 0x11,
    kTypeCtrlUp = 0x20,
    kTypeCtrlDown = 0x21,
    kTypePing = 0x7E,
    kTypePong = 0x7F,
};

bool IsKnownType(uint8_t type);

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor.
// Self test: Crc16Ccitt("123456789", 9) == 0x29B1.
uint16_t Crc16Ccitt(const uint8_t* data, size_t len);

// Appends a complete frame to `out`. False when the payload is too long.
bool Encode(uint8_t type, uint8_t seq, const uint8_t* payload, size_t len,
            std::vector<uint8_t>& out);

struct Frame {
    uint8_t type = 0;
    uint8_t seq = 0;
    std::vector<uint8_t> payload;
};

// Resynchronising decoder. `now_ms` is supplied by the caller so the candidate
// timeout can be exercised without a real clock.
class Decoder {
public:
    void Feed(const uint8_t* data, size_t len, uint32_t now_ms, std::vector<Frame>& out);
    // Drives the candidate timeout when no bytes arrive. The link can fall
    // silent with half a candidate frame buffered, and nothing would ever
    // re-check it if the timeout were only evaluated on incoming data
    // (spec §7.6 item 4). Call this periodically, every 50 ms or so.
    void Tick(uint32_t now_ms, std::vector<Frame>& out) { Feed(nullptr, 0, now_ms, out); }
    void Reset();

    uint32_t resyncs() const { return resyncs_; }
    uint32_t crc_errors() const { return crc_errors_; }
    uint32_t skipped_bytes() const { return skipped_bytes_; }
    uint32_t overflows() const { return overflows_; }

private:
    enum class State { kSearching, kCandidate, kSynced };

    void Compact();
    // Restarts the search one byte past the current candidate.
    void RestartSearch();

    std::vector<uint8_t> buf_;
    size_t pos_ = 0;              // start of unparsed data inside buf_
    State state_ = State::kSearching;
    uint32_t candidate_since_ms_ = 0;
    bool candidate_timed_ = false;

    uint32_t resyncs_ = 0;
    uint32_t crc_errors_ = 0;
    uint32_t skipped_bytes_ = 0;
    uint32_t overflows_ = 0;
};

}  // namespace usb_frame

#endif  // _USB_FRAME_H_
