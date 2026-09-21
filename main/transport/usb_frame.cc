#include "usb_frame.h"

#include <cstring>

namespace usb_frame {

bool IsKnownType(uint8_t type) {
    switch (type) {
        case kTypeLog:
        case kTypeAudioUp:
        case kTypeAudioDown:
        case kTypeCtrlUp:
        case kTypeCtrlDown:
        case kTypePing:
        case kTypePong:
            return true;
        default:
            return false;
    }
}

uint16_t Crc16Ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

bool Encode(uint8_t type, uint8_t seq, const uint8_t* payload, size_t len,
            std::vector<uint8_t>& out) {
    if (len > kMaxPayload) {
        return false;
    }
    const size_t base = out.size();
    out.resize(base + kOverhead + len);
    uint8_t* p = out.data() + base;
    p[0] = kSync0;
    p[1] = kSync1;
    p[2] = type;
    p[3] = seq;
    p[4] = static_cast<uint8_t>(len & 0xFF);
    p[5] = static_cast<uint8_t>((len >> 8) & 0xFF);
    if (len != 0 && payload != nullptr) {
        memcpy(p + kHeaderSize, payload, len);
    }
    // CRC covers TYPE..PAYLOAD, i.e. offsets 2..5+LEN.
    const uint16_t crc = Crc16Ccitt(p + 2, 4 + len);
    p[kHeaderSize + len] = static_cast<uint8_t>(crc & 0xFF);
    p[kHeaderSize + len + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return true;
}

void Decoder::Reset() {
    buf_.clear();
    pos_ = 0;
    state_ = State::kSearching;
    candidate_timed_ = false;
}

void Decoder::Compact() {
    if (pos_ == 0) {
        return;
    }
    buf_.erase(buf_.begin(), buf_.begin() + pos_);
    pos_ = 0;
}

void Decoder::RestartSearch() {
    // The candidate starts at pos_; resume scanning one byte later so a real
    // header hidden inside a false positive is still found.
    pos_ += 1;
    skipped_bytes_ += 1;
    state_ = State::kSearching;
    candidate_timed_ = false;
    resyncs_++;
}

void Decoder::Feed(const uint8_t* data, size_t len, uint32_t now_ms, std::vector<Frame>& out) {
    if (data != nullptr && len != 0) {
        buf_.insert(buf_.end(), data, data + len);
    }

    for (;;) {
        // Hard cap on buffered bytes: drop the oldest and force a rescan.
        if (buf_.size() - pos_ > kMaxRxBuffer) {
            const size_t excess = (buf_.size() - pos_) - kMaxRxBuffer;
            pos_ += excess;
            skipped_bytes_ += static_cast<uint32_t>(excess);
            overflows_++;
            state_ = State::kSearching;
            candidate_timed_ = false;
        }

        const size_t avail = buf_.size() - pos_;

        if (state_ == State::kSearching) {
            size_t i = pos_;
            bool found = false;
            while (i + 1 < buf_.size()) {
                if (buf_[i] == kSync0 && buf_[i + 1] == kSync1) {
                    found = true;
                    break;
                }
                i++;
            }
            if (found) {
                skipped_bytes_ += static_cast<uint32_t>(i - pos_);
                pos_ = i;
                state_ = State::kCandidate;
                candidate_since_ms_ = now_ms;
                candidate_timed_ = true;
                continue;
            }
            // Keep the final byte: it may be the first half of a magic split
            // across two feeds.
            const size_t keep = buf_.empty() ? 0 : 1;
            if (buf_.size() - keep > pos_) {
                skipped_bytes_ += static_cast<uint32_t>(buf_.size() - keep - pos_);
                pos_ = buf_.size() - keep;
            }
            Compact();
            return;
        }

        // Both CANDIDATE and SYNCED parse a frame that starts at pos_.
        if (avail < kHeaderSize) {
            if (state_ == State::kCandidate && candidate_timed_ &&
                now_ms - candidate_since_ms_ >= kCandidateTimeoutMs) {
                RestartSearch();
                continue;
            }
            Compact();
            return;
        }

        const uint8_t* f = buf_.data() + pos_;
        const bool magic_ok = f[0] == kSync0 && f[1] == kSync1;
        const uint8_t type = f[2];
        const uint16_t plen = static_cast<uint16_t>(f[4] | (f[5] << 8));

        if (!magic_ok || !IsKnownType(type) || plen > kMaxPayload) {
            RestartSearch();
            continue;
        }

        const size_t total = kOverhead + plen;
        if (avail < total) {
            if (state_ == State::kCandidate && candidate_timed_ &&
                now_ms - candidate_since_ms_ >= kCandidateTimeoutMs) {
                RestartSearch();
                continue;
            }
            Compact();
            return;
        }

        const uint16_t want = Crc16Ccitt(f + 2, 4 + plen);
        const uint16_t got =
            static_cast<uint16_t>(f[kHeaderSize + plen] | (f[kHeaderSize + plen + 1] << 8));
        if (want != got) {
            crc_errors_++;
            RestartSearch();
            continue;
        }

        Frame frame;
        frame.type = type;
        frame.seq = f[3];
        frame.payload.assign(f + kHeaderSize, f + kHeaderSize + plen);
        out.push_back(std::move(frame));

        pos_ += total;
        state_ = State::kSynced;
        candidate_timed_ = false;
    }
}

}  // namespace usb_frame
