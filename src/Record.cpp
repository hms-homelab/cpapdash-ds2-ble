#include "cpapdash/ds2ble/Record.h"

#include <cstdio>

namespace cpapdash::ds2ble {

const char* streamExtension(uint8_t stream) {
    switch (stream) {
        case 1: return "B01";
        case 2: return "B02";
        case 3: return "B03";
        case 4: return "B04";
        default: return "";
    }
}

std::string cardFileName(uint8_t stream, uint32_t session) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%08X.%s", static_cast<unsigned>(session), streamExtension(stream));
    return buf;
}

uint32_t blockCrc(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        // The byte is the low byte of the word 00 00 00 bb, fed MSB first:
        // the 24 zero bits leave the register unchanged under the xor, so the
        // byte goes in at the bottom and all 32 bits are shifted through.
        crc ^= data[i];
        for (int k = 0; k < 32; ++k)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    }
    return crc;
}

namespace {
uint16_t u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

std::vector<BlockInfo> verifyBody(const std::vector<uint8_t>& body) {
    std::vector<BlockInfo> out;
    size_t i = 0;
    while (i < body.size()) {
        const size_t left = body.size() - i;
        if (left < 21) throw RecordError("short block at " + std::to_string(i));
        const uint8_t* b = body.data() + i;
        BlockInfo info;
        info.format_version = b[0];
        info.length = u16le(b + 1);
        info.file_type = b[3];
        info.family = b[4];
        info.family_version = b[5];
        info.extension = b[6];
        info.session = u32le(b + 7);
        info.timestamp = u32le(b + 11);
        if (info.format_version != 3)
            throw RecordError("format version " + std::to_string(info.format_version) + " at " +
                              std::to_string(i) + ", expected 3");
        if (info.length > left || info.length < 21)
            throw RecordError("block at " + std::to_string(i) + " claims " + std::to_string(info.length) +
                              " bytes, " + std::to_string(left) + " left");
        if (info.file_type != 0)
            throw RecordError("file type " + std::to_string(info.file_type) + " not expected over Bluetooth");
        const size_t n = b[15];
        const size_t hend = 16 + 2 * n;
        if (hend + 1 + 4 > info.length) throw RecordError("header table overruns the block, session " +
                                                          std::to_string(info.session));
        unsigned sum = 0;
        for (size_t k = 0; k < hend; ++k) sum += b[k];
        if ((sum & 0xff) != b[hend])
            throw RecordError("header checksum mismatch, session " + std::to_string(info.session));
        const size_t data_len = info.length - (hend + 1) - 4;
        if (blockCrc(b + hend + 1, data_len) != u32le(b + info.length - 4))
            throw RecordError("data CRC mismatch, session " + std::to_string(info.session));
        out.push_back(info);
        i += info.length;
    }
    return out;
}

}  // namespace cpapdash::ds2ble
