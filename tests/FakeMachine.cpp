#include "FakeMachine.h"

#include "cpapdash/ds2ble/Protocol.h"
#include "cpapdash/ds2ble/Record.h"

#include <algorithm>

using namespace cpapdash::ds2ble;

namespace ds2test {

namespace {
void le16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void le32(Bytes& b, uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
void be32(Bytes& b, uint32_t v) {
    for (int k = 3; k >= 0; --k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
Bytes reply(uint16_t cmd, uint8_t sub, uint8_t status, const Bytes& data = {}) {
    Bytes d{status};
    d.insert(d.end(), data.begin(), data.end());
    return encode(cmd, sub, d);
}
}  // namespace

Bytes makeBlock(uint32_t session, uint32_t unix_time, uint8_t extension,
                const std::vector<std::pair<uint8_t, Bytes>>& records) {
    std::map<uint8_t, uint8_t> sizes;
    Bytes data;
    for (const auto& [code, body] : records) {
        sizes.emplace(code, static_cast<uint8_t>(body.size()));
        data.push_back(code);
        data.insert(data.end(), body.begin(), body.end());
    }
    Bytes head{3};
    const size_t hend = 16 + 2 * sizes.size();
    const size_t length = hend + 1 + data.size() + 4;
    le16(head, static_cast<uint16_t>(length));
    head.push_back(0);           // file type 0
    head.push_back(0);           // family
    head.push_back(6);           // family version
    head.push_back(extension);
    le32(head, session);
    le32(head, unix_time);
    head.push_back(static_cast<uint8_t>(sizes.size()));
    for (const auto& [code, size] : sizes) {
        head.push_back(code);
        head.push_back(size);
    }
    unsigned sum = 0;
    for (uint8_t b : head) sum += b;
    head.push_back(static_cast<uint8_t>(sum));
    head.insert(head.end(), data.begin(), data.end());
    le32(head, blockCrc(data.data(), data.size()));
    return head;
}

Bytes FakeMachine::bookmarkFor(uint8_t stream, size_t index) {
    Bytes b;
    le32(b, static_cast<uint32_t>(0x1000 + 0x100 * index));
    b.push_back(stream);
    b.push_back(0x08);
    for (uint8_t k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(0xa0 + stream + index + k));
    return b;
}

Bytes FakeMachine::answer(const Bytes& request) {
    requests.push_back(request);
    const Frame f = decode(request);
    switch (f.cmd) {
        case kCmdSerial: {
            Bytes s(serial.begin(), serial.end());
            s.push_back(0);
            return reply(f.cmd, f.sub, 0, s);
        }
        case kCmdCommands:
            return reply(f.cmd, f.sub, 0, {0x00, 0x00, 0x00, 0x20, 0x00, 0x2b, 0x00, 0x9a});
        case kCmdSetClock: {
            uint32_t v = 0;
            for (uint8_t b : f.data) v = (v << 8) | b;
            clock_writes.push_back(v);
            return reply(f.cmd, f.sub, 0);
        }
        case kCmdFirmware: return reply(f.cmd, f.sub, 0, {'V', '9', '.', '9', 0});
        case kCmdModel: return reply(f.cmd, f.sub, 0, {'5', '2', '0', 'X', 0});
        case kCmdCounters: return reply(f.cmd, f.sub, 0, {0, 0, 0, 0, 1, 1});
        case kCmdCursor: break;
        default: throw UnexpectedRequest("fake does not know " + toHex(request));
    }
    if (f.sub == kCurNewest) {
        uint32_t newest = 0;
        for (const auto& [s, recs] : streams)
            for (const auto& r : recs) newest = std::max(newest, r.session);
        if (open_session) newest = std::max(newest, *open_session);
        Bytes d;
        be32(d, newest);
        return reply(f.cmd, f.sub, 0, d);
    }
    if (f.sub == kCurOpen) {
        if (f.data.empty()) throw UnexpectedRequest("open with no stream");
        cur_stream_ = f.data[0];
        const Bytes bm(f.data.begin() + 1, f.data.end());
        const auto& recs = streams[cur_stream_];
        size_t next = 0;
        if (bm.empty()) {
            ++oldest_opens;
        } else {
            bool found = false;
            for (size_t i = 0; i < recs.size(); ++i)
                if (bookmarkFor(cur_stream_, i) == bm) {
                    next = i + 1;
                    found = true;
                }
            if (!found) throw UnexpectedRequest("open after a bookmark this machine never gave");
        }
        while (next < recs.size() && open_session && recs[next].session == *open_session) ++next;
        if (next >= recs.size()) {
            cur_index_ = -1;
            if (status06_at_end && !bm.empty()) return reply(f.cmd, f.sub, kStatusNoNext);
            return reply(f.cmd, f.sub, 0, Bytes(12, 0));
        }
        cur_index_ = static_cast<long>(next);
        const auto& r = recs[next];
        Bytes d;
        be32(d, static_cast<uint32_t>((r.body.size() + 189) / 190));
        be32(d, static_cast<uint32_t>(r.body.size()));
        be32(d, r.session);
        return reply(f.cmd, f.sub, 0, d);
    }
    if (f.sub == kCurChunk) {
        if (cur_index_ < 0) throw UnexpectedRequest("chunk with no record open");
        uint32_t n = 0;
        for (uint8_t b : f.data) n = (n << 8) | b;
        const auto& body = streams[cur_stream_][static_cast<size_t>(cur_index_)].body;
        const size_t from = size_t(n) * 190;
        if (from >= body.size()) throw UnexpectedRequest("chunk past the end");
        const size_t to = std::min(body.size(), from + 190);
        return reply(f.cmd, f.sub, 0, Bytes(body.begin() + static_cast<long>(from), body.begin() + static_cast<long>(to)));
    }
    if (f.sub == kCurBookmark) {
        if (cur_stream_ == 0) return reply(f.cmd, f.sub, 0, bookmarkFor(0, 0));
        if (cur_index_ < 0) throw UnexpectedRequest("bookmark with no record open");
        return reply(f.cmd, f.sub, 0, bookmarkFor(cur_stream_, static_cast<size_t>(cur_index_)));
    }
    throw UnexpectedRequest("fake does not know " + toHex(request));
}

}  // namespace ds2test
