#pragma once

#include "Replay.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ds2test {

/// One FileType 0, format version 3 block with a correct header sum and CRC:
/// `records` are (code, body) pairs; every code's size comes from its first
/// record. Made up for tests, no machine wrote it.
Bytes makeBlock(uint32_t session, uint32_t unix_time, uint8_t extension,
                const std::vector<std::pair<uint8_t, Bytes>>& records);

/**
 * A synthetic DreamStation 2: its own serial, a ring of records per stream,
 * bookmarks it makes up (10 bytes, stream number in byte 4), chunks of 190
 * bytes. It answers like the real one: an open after the last record gives
 * an all-zero header, or status 06 when `status06_at_end` is set; an open
 * with no bookmark starts at the oldest record; a session marked `open`
 * (therapy running) is not served.
 */
class FakeMachine : public Machine {
public:
    struct Rec {
        uint32_t session;
        Bytes body;
    };
    std::map<uint8_t, std::vector<Rec>> streams;
    bool status06_at_end = false;
    std::optional<uint32_t> open_session;   ///< therapy running: this session is withheld
    std::string serial = "TEST0000000001";
    std::vector<uint32_t> clock_writes;     ///< set-clock values received
    int oldest_opens = 0;
    std::vector<Bytes> requests;            ///< every request received

    Bytes answer(const Bytes& request) override;
    /// The bookmark the fake gives record `index` of `stream`.
    static Bytes bookmarkFor(uint8_t stream, size_t index);

private:
    uint8_t cur_stream_ = 0;
    long cur_index_ = -1;   ///< the record the last open answered, -1 = none
};

}  // namespace ds2test
