#include "cpapdash/ds2ble/SyncSession.h"

#include "cpapdash/ds2ble/Protocol.h"

#include <cstdio>

namespace cpapdash::ds2ble {

namespace {

std::string cstr(const std::vector<uint8_t>& d, size_t from) {
    std::string s;
    for (size_t i = from; i < d.size() && d[i] != 0; ++i) s.push_back(static_cast<char>(d[i]));
    return s;
}

uint32_t u32be(const std::vector<uint8_t>& d, size_t at) {
    return (static_cast<uint32_t>(d[at]) << 24) | (static_cast<uint32_t>(d[at + 1]) << 16) |
           (static_cast<uint32_t>(d[at + 2]) << 8) | static_cast<uint32_t>(d[at + 3]);
}

std::vector<uint8_t> be32(uint32_t v) {
    return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
            static_cast<uint8_t>(v)};
}

std::string cmdName(uint16_t cmd, uint8_t sub) {
    char b[16];
    std::snprintf(b, sizeof b, "%04x/%02x", cmd, sub);
    return b;
}

}  // namespace

SyncSession::SyncSession(SyncOptions options) : opt_(std::move(options)) {
    for (const auto& [stream, bm] : opt_.bookmarks)
        if (!bm.empty()) bookmarks_[stream] = bm;
}

std::vector<uint8_t> SyncSession::request(Phase next, uint16_t cmd, uint8_t sub, std::vector<uint8_t> data) {
    phase_ = next;
    want_cmd_ = cmd;
    want_sub_ = sub;
    ++requests_;
    return encode(cmd, sub, data);
}

std::vector<uint8_t> SyncSession::fail(const std::string& why) {
    phase_ = Phase::Failed;
    error_ = why;
    return {};
}

void SyncSession::abort(const std::string& why) {
    if (phase_ != Phase::Done && phase_ != Phase::Failed) fail(why);
}

std::vector<uint8_t> SyncSession::begin() {
    if (phase_ != Phase::Idle) return fail("begin() called twice");
    return request(Phase::Serial, kCmdSerial, 0x01);
}

std::vector<uint8_t> SyncSession::feed(const uint8_t* bytes, size_t len) {
    if (phase_ == Phase::Done || phase_ == Phase::Failed || phase_ == Phase::Idle) return {};
    std::vector<Frame> frames;
    try {
        frames = deframer_.feed(bytes, len);
    } catch (const FrameError& e) {
        return fail(std::string("bad frame: ") + e.what());
    }
    if (frames.empty()) return {};
    if (frames.size() > 1)
        return fail("the machine sent " + std::to_string(frames.size()) + " answers to one request");
    return handle(frames.front());
}

std::vector<uint8_t> SyncSession::handle(const Frame& f) {
    const std::string asked = cmdName(want_cmd_, want_sub_);
    if (f.cmd != want_cmd_ || f.sub != want_sub_)
        return fail("asked " + asked + ", got " + cmdName(f.cmd, f.sub));
    if (f.data.empty()) return fail(asked + ": answer has no status byte");
    const uint8_t status = f.data[0];
    const bool open = phase_ == Phase::Open;
    if (status != kStatusOk && !(open && status == kStatusNoNext)) {
        char b[8];
        std::snprintf(b, sizeof b, "%02x", status);
        return fail(asked + " status " + b);
    }
    const std::vector<uint8_t> d(f.data.begin() + 1, f.data.end());

    switch (phase_) {
        case Phase::Serial:
            identity_.serial = cstr(d, 0);
            return request(Phase::Commands, kCmdCommands, 0x00);
        case Phase::Commands:
            for (size_t i = 0; i + 1 < d.size(); i += 2)
                identity_.commands.push_back(static_cast<uint16_t>((d[i] << 8) | d[i + 1]));
            if (opt_.set_clock_utc)
                return request(Phase::SetClock, kCmdSetClock, 0x00, be32(*opt_.set_clock_utc));
            return request(Phase::Firmware, kCmdFirmware, 0x03);
        case Phase::SetClock:
            identity_.clock_set = true;
            return request(Phase::Firmware, kCmdFirmware, 0x03);
        case Phase::Firmware:
            identity_.firmware = cstr(d, 0);
            return request(Phase::Model, kCmdModel, 0x01);
        case Phase::Model:
            identity_.model = cstr(d, 0);
            return request(Phase::Newest, kCmdCursor, kCurNewest);
        case Phase::Newest:
            if (d.size() < 4) return fail("newest session answer is " + std::to_string(d.size()) + " bytes");
            identity_.newest_session = u32be(d, 0);
            return request(Phase::Counters, kCmdCounters, 0x00,
                           std::vector<uint8_t>(kCountersRequest, kCountersRequest + sizeof kCountersRequest));
        case Phase::Counters:
            identity_.counters = d;
            return request(Phase::Stream0Open, kCmdCursor, kCurOpen, {0x00});
        case Phase::Stream0Open:
            for (uint8_t b : d)
                if (b) return fail("stream 0 is not empty: " + toHex(d));
            return request(Phase::Stream0Bookmark, kCmdCursor, kCurBookmark);
        case Phase::Stream0Bookmark:
            bookmarks_[0] = d;
            return afterIdentity();
        case Phase::Open: {
            const uint8_t stream = opt_.streams[stream_index_];
            if (status == kStatusNoNext) return nextStream();   // nothing after the bookmark
            if (d.size() != 12) return fail("cursor header is " + std::to_string(d.size()) + " bytes: " + toHex(d));
            chunks_ = u32be(d, 0);
            length_ = u32be(d, 4);
            session_ = u32be(d, 8);
            if (chunks_ == 0 && length_ == 0 && session_ == 0) return nextStream();
            if (chunks_ == 0 || length_ == 0)
                return fail("stream " + std::to_string(stream) + " header " + toHex(d) + " is not a record");
            body_.clear();
            next_chunk_ = 0;
            return request(Phase::Chunk, kCmdCursor, kCurChunk, be32(next_chunk_));
        }
        case Phase::Chunk:
            body_.insert(body_.end(), d.begin(), d.end());
            if (body_.size() > length_)
                return fail("session " + std::to_string(session_) + ": more than the " + std::to_string(length_) +
                            " bytes the header gave");
            if (++next_chunk_ < chunks_) return request(Phase::Chunk, kCmdCursor, kCurChunk, be32(next_chunk_));
            if (body_.size() != length_)
                return fail("session " + std::to_string(session_) + ": " + std::to_string(body_.size()) +
                            " bytes, the header said " + std::to_string(length_));
            return request(Phase::Bookmark, kCmdCursor, kCurBookmark);
        case Phase::Bookmark: {
            const uint8_t stream = opt_.streams[stream_index_];
            if (d.size() != kBookmarkSize)
                return fail("bookmark is " + std::to_string(d.size()) + " bytes: " + toHex(d));
            Record r;
            r.stream = stream;
            r.session = session_;
            r.body = std::move(body_);
            body_.clear();
            auto it = bookmarks_.find(stream);
            if (it != bookmarks_.end()) r.bookmark_in = it->second;
            r.bookmark_out = d;
            bookmarks_[stream] = d;
            records_.push_back(std::move(r));
            if (opt_.on_record) opt_.on_record(records_.back());
            return openStream();
        }
        default:
            return fail("answer in phase " + std::to_string(static_cast<int>(phase_)));
    }
}

std::vector<uint8_t> SyncSession::afterIdentity() {
    stream_index_ = 0;
    if (opt_.streams.empty()) {
        phase_ = Phase::Done;
        return {};
    }
    return openStream();
}

std::vector<uint8_t> SyncSession::openStream() {
    const uint8_t stream = opt_.streams[stream_index_];
    std::vector<uint8_t> data{stream};
    auto it = bookmarks_.find(stream);
    if (it != bookmarks_.end()) data.insert(data.end(), it->second.begin(), it->second.end());
    return request(Phase::Open, kCmdCursor, kCurOpen, std::move(data));
}

std::vector<uint8_t> SyncSession::nextStream() {
    if (++stream_index_ >= opt_.streams.size()) {
        phase_ = Phase::Done;
        return {};
    }
    return openStream();
}

}  // namespace cpapdash::ds2ble
