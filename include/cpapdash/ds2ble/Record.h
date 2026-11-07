#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpapdash::ds2ble {

/**
 * The machine's records come in four streams. Each record body arrives in
 * plaintext and holds the content of the session file of the same session
 * (observed by comparing records with the machine's own files):
 *
 *   stream 1  session summary   <session, 8 hex digits>.B01   in the P folder
 *   stream 2  session events    <session>.B02                 in the P folder
 *   stream 3  machine log       an entry of a .B03 file in the D folder
 *   stream 4  machine log       an entry of a .B04 file in the E folder
 *
 * Bluetooth never carries the waveform (.B05). Stream 0 exists and is always
 * empty.
 */
enum class Stream : uint8_t { Empty = 0, Summary = 1, Events = 2, LogD = 3, LogE = 4 };

/// The 10-byte position of a record in one stream, as the machine hands it
/// out. Opaque: it is stored and echoed back, never interpreted. Empty means
/// "no record read yet": the next open starts at the oldest record kept.
using Bookmark = std::vector<uint8_t>;
constexpr size_t kBookmarkSize = 10;

/**
 * One record as it arrived. This is the type a writer of the plaintext card
 * tree takes: `body` is the file's content, `stream` and `session` say which
 * file (cardFileName), and nothing else is needed to place it.
 */
struct Record {
    uint8_t stream = 0;        ///< 1..4, see Stream
    uint32_t session = 0;      ///< the machine's session number, from the cursor header
    std::vector<uint8_t> body; ///< the plaintext file content, all chunks joined
    Bookmark bookmark_in;      ///< the bookmark the record was opened after ("" = oldest)
    Bookmark bookmark_out;     ///< this record's own bookmark: store it, resume after it
};

/// "B01".."B04" for streams 1..4, "" otherwise.
const char* streamExtension(uint8_t stream);

/// "000004D2.B01" for a summary of session 1234: the stem is the session in
/// eight uppercase hex digits, the extension the stream's.
std::string cardFileName(uint8_t stream, uint32_t session);

/// A body that fails the container checks.
class RecordError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * One block of the DataFormatVersion 3 container a body is made of (the
 * card's format, publicly documented): a 15-byte header (version, u16 LE
 * length, file type, family, family version, extension, u32 LE session, u32
 * LE time), a record-size table, an 8-bit header sum, the data, and a CRC-32
 * of the data (MSB first, polynomial 0x04C11DB7, init 0xFFFFFFFF, each byte
 * fed as the 32-bit word 00 00 00 bb, no final xor), stored u32 LE.
 */
struct BlockInfo {
    uint8_t format_version = 0;
    uint16_t length = 0;
    uint8_t file_type = 0;
    uint8_t family = 0;
    uint8_t family_version = 0;
    uint8_t extension = 0;
    uint32_t session = 0;
    uint32_t timestamp = 0;   ///< Unix seconds, UTC
};

/// Every block of a body, each checked (header sum and data CRC). Throws
/// RecordError on the first block that fails or overruns. Over Bluetooth the
/// machine sends only FileType 0 blocks of format version 3.
std::vector<BlockInfo> verifyBody(const std::vector<uint8_t>& body);

/// The container's CRC over `data`, as above.
uint32_t blockCrc(const uint8_t* data, size_t len);

}  // namespace cpapdash::ds2ble
