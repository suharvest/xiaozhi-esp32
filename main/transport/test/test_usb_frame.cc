// Host-side unit test for the USB framing layer.
//
//   c++ -std=c++17 -Wall -Wextra -o /tmp/test_usb_frame \
//       main/transport/test/test_usb_frame.cc main/transport/usb_frame.cc
//   /tmp/test_usb_frame
//
// Every CRC below is produced by our own Crc16Ccitt(); the literals are the
// expected values copied from the specification, never a shortcut around the
// implementation.

#include "../usb_frame.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace usb_frame;

static int g_failures = 0;
static int g_checks = 0;

static void Report(const char* name, bool ok, const std::string& detail = "") {
    g_checks++;
    if (!ok) {
        g_failures++;
    }
    printf("%-46s %s%s%s\n", name, ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ",
           detail.c_str());
}

static std::string Hex(const std::vector<uint8_t>& v) {
    std::string s;
    char b[4];
    for (uint8_t x : v) {
        snprintf(b, sizeof(b), "%02X ", x);
        s += b;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

// Parses "A5 5A 7E ..." into bytes.
static std::vector<uint8_t> FromHex(const char* text) {
    std::vector<uint8_t> out;
    unsigned v = 0;
    int nibbles = 0;
    for (const char* p = text; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else { nibbles = 0; v = 0; continue; }
        v = (v << 4) | static_cast<unsigned>(d);
        if (++nibbles == 2) {
            out.push_back(static_cast<uint8_t>(v));
            v = 0;
            nibbles = 0;
        }
    }
    return out;
}

static std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Encodes and checks both the CRC value and the full frame bytes.
static void CheckVector(const char* name, uint8_t type, uint8_t seq,
                        const std::vector<uint8_t>& payload, uint16_t expect_crc,
                        const char* expect_frame_hex) {
    // The CRC covers TYPE, SEQ, LEN and PAYLOAD, so rebuild that span here and
    // run our own implementation over it.
    std::vector<uint8_t> span;
    span.push_back(type);
    span.push_back(seq);
    span.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    span.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    span.insert(span.end(), payload.begin(), payload.end());
    const uint16_t crc = Crc16Ccitt(span.data(), span.size());

    char detail[96];
    snprintf(detail, sizeof(detail), "crc=0x%04X expected=0x%04X", crc, expect_crc);
    Report((std::string(name) + " crc").c_str(), crc == expect_crc, detail);

    std::vector<uint8_t> encoded;
    const bool ok = Encode(type, seq, payload.data(), payload.size(), encoded);
    const std::vector<uint8_t> expected = FromHex(expect_frame_hex);
    Report((std::string(name) + " encode").c_str(), ok && encoded == expected,
           encoded == expected ? "" : Hex(encoded));

    std::vector<Frame> got;
    Decoder dec;
    dec.Feed(expected.data(), expected.size(), 0, got);
    const bool decoded = got.size() == 1 && got[0].type == type && got[0].seq == seq &&
                         got[0].payload == payload;
    Report((std::string(name) + " decode").c_str(), decoded,
           decoded ? "" : ("frames=" + std::to_string(got.size())));
}

int main() {
    printf("== self test ==\n");
    {
        const char* s = "123456789";
        const uint16_t crc = Crc16Ccitt(reinterpret_cast<const uint8_t*>(s), strlen(s));
        char d[64];
        snprintf(d, sizeof(d), "0x%04X expected 0x29B1", crc);
        Report("CRC-16/CCITT-FALSE(\"123456789\")", crc == 0x29B1, d);
    }

    printf("\n== 7.1 PING, 8 zero bytes ==\n");
    CheckVector("7.1 PING", kTypePing, 0x00, std::vector<uint8_t>(8, 0), 0x5A94,
                "A5 5A 7E 00 08 00 00 00 00 00 00 00 00 00 94 5A");

    printf("\n== 7.2 LOG \"hello\" ==\n");
    CheckVector("7.2 LOG", kTypeLog, 0x2A, Bytes("hello"), 0xA380,
                "A5 5A 01 2A 05 00 68 65 6C 6C 6F 80 A3");

    printf("\n== 7.3 CTRL_UP config ==\n");
    CheckVector("7.3 CTRL_UP", kTypeCtrlUp, 0xFF,
                Bytes("{\"type\":\"config\",\"sample_rate\":16000}"), 0x2E5E,
                "A5 5A 20 FF 25 00 7B 22 74 79 70 65 22 3A 22 63 6F 6E 66 69 67"
                "22 2C 22 73 61 6D 70 6C 65 5F 72 61 74 65 22 3A 31 36 30 30 30 7D 5E 2E");

    printf("\n== 7.4 AUDIO_UP with magic inside the payload ==\n");
    CheckVector("7.4 AUDIO_UP", kTypeAudioUp, 0x01, FromHex("A5 5A A5 5A 00 01 02 03"),
                0x66A1, "A5 5A 10 01 08 00 A5 5A A5 5A 00 01 02 03 A1 66");

    const std::vector<uint8_t> v71 =
        FromHex("A5 5A 7E 00 08 00 00 00 00 00 00 00 00 00 94 5A");
    const std::vector<uint8_t> v72 = FromHex("A5 5A 01 2A 05 00 68 65 6C 6C 6F 80 A3");
    const std::vector<uint8_t> v73 = FromHex(
        "A5 5A 20 FF 25 00 7B 22 74 79 70 65 22 3A 22 63 6F 6E 66 69 67"
        "22 2C 22 73 61 6D 70 6C 65 5F 72 61 74 65 22 3A 31 36 30 30 30 7D 5E 2E");
    const std::vector<uint8_t> v74 = FromHex("A5 5A 10 01 08 00 A5 5A A5 5A 00 01 02 03 A1 66");

    printf("\n== back to back, magic inside payload must not resync ==\n");
    {
        std::vector<uint8_t> in = v74;
        in.insert(in.end(), v72.begin(), v72.end());
        std::vector<Frame> got;
        Decoder dec;
        dec.Feed(in.data(), in.size(), 0, got);
        const bool ok = got.size() == 2 && got[0].type == kTypeAudioUp &&
                        got[0].payload.size() == 8 && got[1].type == kTypeLog;
        Report("7.4 then 7.2 back to back", ok, "frames=" + std::to_string(got.size()));
    }

    printf("\n== 7.5 negative cases ==\n");
    {
        // CRC error: corrupt the low CRC byte of 7.2, then send a clean 7.2.
        std::vector<uint8_t> bad = v72;
        bad[bad.size() - 2] = static_cast<uint8_t>(bad[bad.size() - 2] + 1);
        std::vector<uint8_t> in = bad;
        in.insert(in.end(), v72.begin(), v72.end());
        std::vector<Frame> got;
        Decoder dec;
        dec.Feed(in.data(), in.size(), 0, got);
        const bool ok = got.size() == 1 && got[0].type == kTypeLog &&
                        got[0].payload == Bytes("hello") && dec.crc_errors() >= 1;
        Report("bad CRC does not swallow the next frame", ok,
               "frames=" + std::to_string(got.size()) +
                   " crc_errors=" + std::to_string(dec.crc_errors()));
    }
    {
        // Truncated 7.3 followed by a complete 7.2: only the candidate timeout
        // can break the deadlock.
        std::vector<uint8_t> head(v73.begin(), v73.begin() + 20);
        std::vector<Frame> got;
        Decoder dec;
        dec.Feed(head.data(), head.size(), 0, got);
        const bool nothing_yet = got.empty();
        dec.Feed(v72.data(), v72.size(), 250, got);
        const bool ok = nothing_yet && got.size() == 1 && got[0].type == kTypeLog &&
                        got[0].payload == Bytes("hello");
        Report("truncated frame recovers after the timeout", ok,
               "frames=" + std::to_string(got.size()));
    }
    {
        // Fake header with LEN beyond the limit.
        std::vector<uint8_t> in = FromHex("A5 5A 10 00 FF FF 11 22 33 44 55 66 77 88");
        std::vector<Frame> got;
        Decoder dec;
        dec.Feed(in.data(), in.size(), 0, got);
        const bool rejected = got.empty();
        dec.Feed(v72.data(), v72.size(), 1, got);
        const bool ok = rejected && got.size() == 1 && got[0].type == kTypeLog;
        Report("oversized LEN rejected, stream recovers", ok,
               "frames=" + std::to_string(got.size()));
    }
    {
        // A lone handshake frame has to be delivered without a second frame.
        std::vector<Frame> got;
        Decoder dec;
        dec.Feed(v73.data(), v73.size(), 0, got);
        const bool delivered = got.size() == 1;
        // Ten seconds of silence must not change anything.
        dec.Feed(nullptr, 0, 10000, got);
        const bool ok = delivered && got.size() == 1 && got[0].type == kTypeCtrlUp &&
                        got[0].seq == 0xFF;
        Report("single handshake frame delivered immediately", ok,
               "frames=" + std::to_string(got.size()));
    }
    {
        // Boot-time plain text in front of the first frame.
        std::vector<uint8_t> in = Bytes("I (123) boot: hi\n");
        in.insert(in.end(), v72.begin(), v72.end());
        std::vector<Frame> got;
        Decoder dec;
        dec.Feed(in.data(), in.size(), 0, got);
        const bool ok = got.size() == 1 && got[0].type == kTypeLog &&
                        got[0].payload == Bytes("hello");
        Report("plain text prefix skipped", ok, "frames=" + std::to_string(got.size()));
    }

    printf("\n== buffer cap ==\n");
    {
        // More than 8 KiB of junk with no magic must not grow without bound.
        std::vector<uint8_t> junk(12000, 0x41);
        std::vector<Frame> got;
        Decoder dec;
        dec.Feed(junk.data(), junk.size(), 0, got);
        dec.Feed(v71.data(), v71.size(), 0, got);
        const bool ok = got.size() == 1 && got[0].type == kTypePing;
        Report("12 KiB of junk then a PING", ok, "frames=" + std::to_string(got.size()));
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
