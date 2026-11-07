#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpapdash::ds2ble {

/**
 * One frame on the DreamStation 2's data characteristic, in either direction:
 *
 *     10 01 | cmd (u16 BE) | sub (u8) | 10 02 | data | 10 03 | c0 c1
 *
 * Inside cmd, sub and data a 0x10 byte is sent twice (10 10). The two check
 * bytes are never doubled. The check is a Fletcher-16, modulo 255, over the
 * UNDOUBLED cmd, sub and data, with both running sums seeded 0x55, in the
 * 1..255 form:
 *
 *     s1 = s2 = 0x55; for each byte b: s1 = (s1 + b) % 255; s2 = (s2 + s1) % 255
 *     c0 = 255 - ((s1 + s2) % 255);  c1 = 255 - ((s1 + c0) % 255)
 *
 * so a check byte that a 0..254 Fletcher would make 0x00 is sent as 0xff.
 */
struct Frame {
    uint16_t cmd = 0;
    uint8_t sub = 0;
    std::vector<uint8_t> data;

    bool operator==(const Frame& o) const { return cmd == o.cmd && sub == o.sub && data == o.data; }
};

/// A frame that does not parse, or whose check bytes do not match.
class FrameError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr uint8_t kDle = 0x10;
constexpr uint8_t kStx = 0x01;
constexpr uint8_t kData = 0x02;
constexpr uint8_t kEtx = 0x03;

/// The two check bytes over the undoubled cmd, sub and data.
std::array<uint8_t, 2> checksum(const uint8_t* payload, size_t len);

/// The bytes to write for one request.
std::vector<uint8_t> encode(uint16_t cmd, uint8_t sub, const std::vector<uint8_t>& data = {});

/// Exactly one complete frame; anything short, long or wrong throws FrameError.
Frame decode(const std::vector<uint8_t>& raw);

/**
 * Frames out of notifications. One notification normally carries one whole
 * frame, but nothing in the link promises it, so bytes are kept until a frame
 * is complete, and several frames in one notification are handled too. Bytes
 * before a frame's start are dropped. A frame that is complete but malformed
 * throws FrameError (the link is then not to be trusted).
 */
class Deframer {
public:
    std::vector<Frame> feed(const uint8_t* bytes, size_t len);
    std::vector<Frame> feed(const std::vector<uint8_t>& bytes) { return feed(bytes.data(), bytes.size()); }
    /// Bytes held for a frame not complete yet.
    size_t pending() const { return buf_.size(); }
    void reset() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

/// Lowercase hex, no separators; and back (throws std::invalid_argument).
std::string toHex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> fromHex(const std::string& hex);

}  // namespace cpapdash::ds2ble
