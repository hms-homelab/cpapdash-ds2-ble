#pragma once

#include "cpapdash/ds2ble/Frame.h"
#include "cpapdash/ds2ble/Record.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cpapdash::ds2ble {

/// What the machine says about itself at the start of a read.
struct Identity {
    std::string serial;                 ///< as the machine reports it (ASCII)
    std::string model;                  ///< e.g. "520X110C"
    std::string firmware;               ///< e.g. "V1.0.2.3590"
    std::vector<uint16_t> commands;     ///< the command ids it supports
    uint32_t newest_session = 0;        ///< its newest session number
    std::vector<uint8_t> counters;      ///< the 0x0098 answer, kept raw (meaning unknown)
    bool clock_set = false;             ///< this read set the machine's clock
};

struct SyncOptions {
    /// The bookmark stored after the last record of each stream that was read
    /// before. A stream with a bookmark resumes AFTER it; only a stream with
    /// none (the first read of a machine) starts at the oldest record kept.
    std::map<uint8_t, Bookmark> bookmarks;
    /// Which record streams to walk, in this order.
    std::vector<uint8_t> streams{1, 2, 3, 4};
    /// Set the machine's clock to this UTC time (Unix seconds) during the
    /// identity step, as the machine's own app does. It WRITES to the
    /// machine, so it is off unless a caller asks.
    std::optional<uint32_t> set_clock_utc;
    /// Called once per record, the moment its last chunk and its bookmark
    /// are in. A caller that stores records as they come stores the record
    /// first and the stream's bookmark after it.
    std::function<void(const Record&)> on_record;
};

/**
 * One read of a DreamStation 2, with no radio in it: the caller writes the
 * bytes this hands out and feeds back every notification, and this says what
 * to write next. One request is outstanding at a time.
 *
 *     SyncSession s(opts);
 *     write(s.begin());
 *     on each notification:  auto next = s.feed(bytes, n);
 *                            if (!next.empty()) write(next);
 *                            if (s.done() || s.failed()) stop;
 *
 * The order is the machine's own app's: serial, supported commands, [clock],
 * firmware, model, newest session, counters, stream 0 (open, bookmark), then
 * each stream: open after its bookmark, read its chunks, ask the record's
 * bookmark, open after that, until the open answers "none" (an all-zero
 * header, or status 06, both meaning nothing after the bookmark).
 *
 * The session still being recorded is never served: during therapy every open
 * is empty, and the night arrives once the mask is off.
 */
class SyncSession {
public:
    explicit SyncSession(SyncOptions options = {});

    /// The first request to write. Call once.
    std::vector<uint8_t> begin();

    /// One notification's bytes. Returns the next request to write, or empty
    /// when a frame is still incomplete, or when the read has ended (done()
    /// or failed()).
    std::vector<uint8_t> feed(const uint8_t* bytes, size_t len);
    std::vector<uint8_t> feed(const std::vector<uint8_t>& bytes) { return feed(bytes.data(), bytes.size()); }

    /// Ends the read from outside (the link dropped, an answer timed out).
    void abort(const std::string& why);

    bool done() const { return phase_ == Phase::Done; }
    bool failed() const { return phase_ == Phase::Failed; }
    const std::string& error() const { return error_; }

    const Identity& identity() const { return identity_; }
    /// Every record read so far, in the order read.
    const std::vector<Record>& records() const { return records_; }
    /// The bookmark to store per stream: the last record's, or the one given
    /// in the options when nothing new came. Stream 0 holds what the machine
    /// answered for it.
    const std::map<uint8_t, Bookmark>& bookmarks() const { return bookmarks_; }
    /// Requests written so far (for logs and tests).
    size_t requests() const { return requests_; }
    /// The request that is waiting for its answer (to re-send or to log).
    uint16_t pendingCmd() const { return want_cmd_; }
    uint8_t pendingSub() const { return want_sub_; }

private:
    enum class Phase {
        Idle, Serial, Commands, SetClock, Firmware, Model, Newest, Counters,
        Stream0Open, Stream0Bookmark, Open, Chunk, Bookmark, Done, Failed,
    };

    std::vector<uint8_t> request(Phase next, uint16_t cmd, uint8_t sub, std::vector<uint8_t> data = {});
    std::vector<uint8_t> handle(const Frame& f);
    std::vector<uint8_t> afterIdentity();
    std::vector<uint8_t> openStream();
    std::vector<uint8_t> nextStream();
    std::vector<uint8_t> fail(const std::string& why);

    SyncOptions opt_;
    Deframer deframer_;
    Phase phase_ = Phase::Idle;
    std::string error_;
    uint16_t want_cmd_ = 0;
    uint8_t want_sub_ = 0;
    size_t requests_ = 0;

    Identity identity_;
    std::vector<Record> records_;
    std::map<uint8_t, Bookmark> bookmarks_;

    size_t stream_index_ = 0;
    // The record being read.
    uint32_t chunks_ = 0, length_ = 0, session_ = 0, next_chunk_ = 0;
    std::vector<uint8_t> body_;
};

}  // namespace cpapdash::ds2ble
