#include "cpapdash/ds2ble/Frame.h"

#include <algorithm>
#include <cstdio>

namespace cpapdash::ds2ble {

std::array<uint8_t, 2> checksum(const uint8_t* payload, size_t len) {
    unsigned s1 = 0x55, s2 = 0x55;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + payload[i]) % 255;
        s2 = (s2 + s1) % 255;
    }
    const unsigned c0 = 255 - ((s1 + s2) % 255);
    const unsigned c1 = 255 - ((s1 + c0) % 255);
    return {static_cast<uint8_t>(c0), static_cast<uint8_t>(c1)};
}

namespace {

void stuff(std::vector<uint8_t>& out, uint8_t b) {
    out.push_back(b);
    if (b == kDle) out.push_back(kDle);
}

std::string hexAt(const std::vector<uint8_t>& buf, size_t from, size_t n) {
    std::vector<uint8_t> part(buf.begin() + static_cast<long>(from),
                              buf.begin() + static_cast<long>(std::min(buf.size(), from + n)));
    return toHex(part);
}

/// Decodes the frame at buf[0]. Returns false (and leaves `used` alone) when
/// the buffer does not hold all of it yet.
bool decodeAt(const std::vector<uint8_t>& buf, Frame& out, size_t& used) {
    if (buf.size() < 2) return false;
    if (buf[0] != kDle || buf[1] != kStx)
        throw FrameError("frame does not start with 10 01: " + hexAt(buf, 0, 8));
    std::vector<uint8_t> head, data;
    std::vector<uint8_t>* target = &head;
    size_t i = 2;
    for (;;) {
        if (i >= buf.size()) return false;
        const uint8_t b = buf[i];
        if (b != kDle) {
            target->push_back(b);
            ++i;
            continue;
        }
        if (i + 1 >= buf.size()) return false;
        const uint8_t n = buf[i + 1];
        i += 2;
        if (n == kDle) {
            target->push_back(kDle);
        } else if (n == kData && target == &head) {
            target = &data;
        } else if (n == kEtx && target == &data) {
            break;
        } else {
            char msg[64];
            std::snprintf(msg, sizeof msg, "unexpected 10 %02x at %zu", n, i - 2);
            throw FrameError(msg);
        }
    }
    if (i + 2 > buf.size()) return false;
    if (head.size() != 3)
        throw FrameError("header is " + std::to_string(head.size()) + " bytes, expected 3");
    std::vector<uint8_t> payload = head;
    payload.insert(payload.end(), data.begin(), data.end());
    const auto want = checksum(payload.data(), payload.size());
    if (buf[i] != want[0] || buf[i + 1] != want[1])
        throw FrameError("check bytes " + hexAt(buf, i, 2) + " != " +
                         toHex({want[0], want[1]}));
    out.cmd = static_cast<uint16_t>((head[0] << 8) | head[1]);
    out.sub = head[2];
    out.data = std::move(data);
    used = i + 2;
    return true;
}

}  // namespace

std::vector<uint8_t> encode(uint16_t cmd, uint8_t sub, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> payload{static_cast<uint8_t>(cmd >> 8), static_cast<uint8_t>(cmd & 0xff), sub};
    payload.insert(payload.end(), data.begin(), data.end());
    const auto check = checksum(payload.data(), payload.size());

    std::vector<uint8_t> out{kDle, kStx};
    for (size_t k = 0; k < 3; ++k) stuff(out, payload[k]);
    out.push_back(kDle);
    out.push_back(kData);
    for (uint8_t b : data) stuff(out, b);
    out.push_back(kDle);
    out.push_back(kEtx);
    out.push_back(check[0]);
    out.push_back(check[1]);
    return out;
}

Frame decode(const std::vector<uint8_t>& raw) {
    Frame f;
    size_t used = 0;
    if (!decodeAt(raw, f, used)) throw FrameError("incomplete frame: " + toHex(raw));
    if (used != raw.size())
        throw FrameError(std::to_string(raw.size() - used) + " trailing bytes after frame: " + toHex(raw));
    return f;
}

std::vector<Frame> Deframer::feed(const uint8_t* bytes, size_t len) {
    buf_.insert(buf_.end(), bytes, bytes + len);
    std::vector<Frame> out;
    while (!buf_.empty()) {
        size_t start = buf_.size();
        for (size_t k = 0; k + 1 < buf_.size(); ++k) {
            if (buf_[k] == kDle && buf_[k + 1] == kStx) {
                start = k;
                break;
            }
        }
        if (start == buf_.size()) {
            // No start yet. Keep a trailing 0x10: it may be the first half of 10 01.
            const bool keep = buf_.back() == kDle;
            buf_.erase(buf_.begin(), buf_.end() - (keep ? 1 : 0));
            break;
        }
        if (start) buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(start));
        Frame f;
        size_t used = 0;
        bool whole = false;
        try {
            whole = decodeAt(buf_, f, used);
        } catch (...) {
            buf_.clear();
            throw;
        }
        if (!whole) break;
        out.push_back(std::move(f));
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(used));
    }
    return out;
}

std::string toHex(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 0x0f]);
    }
    return s;
}

std::vector<uint8_t> fromHex(const std::string& hex) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2) throw std::invalid_argument("odd hex length");
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) throw std::invalid_argument("not hex: " + hex.substr(i, 2));
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

}  // namespace cpapdash::ds2ble
