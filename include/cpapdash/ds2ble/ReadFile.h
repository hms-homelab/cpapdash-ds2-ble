#pragma once

#include "cpapdash/ds2ble/Record.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpapdash::ds2ble {

/**
 * The read file (`.ds2r`): one request's worth of a DreamStation 2 read, as a
 * reader that walks the record streams for someone else hands it back. It
 * keeps what crossed the link, in the order it came, so the requestor needs
 * nothing else to store the records and resume each stream later.
 *
 * Format version 1:
 *
 *     "DS2R" | version u8 = 1
 *     then items, to the end of the file:  kind u8 | len u32 LE | payload[len]
 *
 *     0x01 ANSWER      cmd u16 BE | sub u8 | status u8 | data       one per identity request
 *     0x02 RECORD      stream u8 | session u32 BE | bookmark[10] | body
 *     0x03 STREAM_END  stream u8 | how u8 | records u16 LE | bookmark[10]
 *     0x04 END         result u8 | more u8 | status u8
 *     0x05 SEALED      stream u8 | session u32 BE | bookmark[10] | path_len u8 | path | card file
 *
 * A SEALED item is a record's file as the machine itself would write it to a
 * card (header and ciphertext), placed at `path` under the card's machine
 * folder: after its RECORD, or instead of it, as the request asked. Stream 0
 * carries the card-level files (PROP.BIN, LOG.SEQ). This library does not
 * open it; it hands the bytes over as they are.
 *
 * An item of a kind this version does not know is SKIPPED (counted in
 * `skipped_items`), so a newer reader's file still yields everything this
 * version understands. Its length must still fit the file.
 *
 * A RECORD is written only once its body is whole and the bookmark that
 * resumes AFTER it is in hand, so every record in a file is complete and its
 * bookmark can be stored with it. A STREAM_END says why that stream stopped
 * (`how`: 0 the open answered all zero, 6 nothing after the bookmark, 0xFF
 * stopped by the reader's cap or a failure) and where it resumes (ten zero
 * bytes: nothing read yet). END, when present, is the last item: `result` is
 * the reader's outcome (0 = ok), `more` is 1 when the cap stopped the walk
 * with records left, `status` the machine's status byte when one other than
 * 00 or 06 ended it.
 */

inline constexpr uint8_t kReadFileVersion = 1;

enum class ReadItemKind : uint8_t {
    Answer = 0x01, Record = 0x02, StreamEnd = 0x03, End = 0x04, Sealed = 0x05
};

/// STREAM_END `how`.
inline constexpr uint8_t kStreamEndAllZero = 0x00;
inline constexpr uint8_t kStreamEndNothingAfter = 0x06;
inline constexpr uint8_t kStreamEndStopped = 0xFF;

/// The machine's answer to one identity request, as it sent it.
struct ReadAnswer {
    uint16_t command = 0;
    uint8_t sub = 0;
    uint8_t status = 0;
    std::vector<uint8_t> data;
};

/// One record: which file it is (stream, session), the bookmark that resumes
/// after it, and the body (the plaintext file content, see Record.h).
struct ReadRecord {
    uint8_t stream = 0;
    uint32_t session = 0;
    Bookmark bookmark;          ///< always kBookmarkSize bytes
    std::vector<uint8_t> body;
};

struct ReadStreamEnd {
    uint8_t stream = 0;
    uint8_t how = 0;
    uint16_t records = 0;
    Bookmark bookmark;          ///< always kBookmarkSize bytes; all zero = none yet

    /// The stream was read to its end (how 0 or 6), not stopped.
    bool reachedEnd() const { return how == kStreamEndAllZero || how == kStreamEndNothingAfter; }
};

/// One record's card file, sealed as the machine writes it (see above).
struct ReadSealed {
    uint8_t stream = 0;         ///< 0 for a card-level file, else 1..4
    uint32_t session = 0;
    Bookmark bookmark;          ///< always kBookmarkSize bytes
    std::string path;           ///< under P-SERIES/<machine folder>/, at most 255 bytes
    std::vector<uint8_t> card;  ///< the file's bytes, as they would sit on the card
};

struct ReadEnd {
    uint8_t result = 0;
    bool more = false;
    uint8_t status = 0;
};

struct ReadFile {
    uint8_t version = kReadFileVersion;
    std::vector<ReadAnswer> answers;
    std::vector<ReadRecord> records;
    std::vector<ReadStreamEnd> stream_ends;
    std::vector<ReadSealed> sealed;
    std::optional<ReadEnd> end;   ///< absent when the file stops before END
    size_t skipped_items = 0;     ///< items of a kind this version does not know

    /// The first answer to (command, sub) with status 0, or nullptr.
    const ReadAnswer* answer(uint16_t command, uint8_t sub) const;

    /// The machine's serial number (002b/01), model (002c/01) and firmware
    /// (000d/03), as text up to the first NUL; "" when the file has no such
    /// answer.
    std::string machineSerial() const;
    std::string model() const;
    std::string firmware() const;

    /// The newest session the machine holds (009a/09, u32 BE), when answered.
    std::optional<uint32_t> newestSession() const;
};

/// A file that is not a version 1 read file, or whose items do not add up.
class ReadFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Parse a read file. Throws ReadFileError on the wrong magic or version, an
 * item cut short, an item too short for its kind, a record stream outside
 * 1..4 (0..4 for SEALED), a SEALED path past its item, or anything after END.
 * An item of an unknown kind is skipped. Nothing is returned for a file that
 * fails: a requestor stores all of it or none.
 */
ReadFile parseReadFile(const uint8_t* data, size_t size);
ReadFile parseReadFile(const std::vector<uint8_t>& bytes);

/// The file's bytes, items in the order: answers, records, sealed, stream
/// ends, end. For tests and tools; a parse of the result gives `file` back.
/// Throws ReadFileError for a sealed path over 255 bytes.
std::vector<uint8_t> writeReadFile(const ReadFile& file);

}  // namespace cpapdash::ds2ble
