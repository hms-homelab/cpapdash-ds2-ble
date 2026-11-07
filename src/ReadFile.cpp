#include "cpapdash/ds2ble/ReadFile.h"

#include <cstring>

namespace cpapdash::ds2ble {

namespace {

constexpr uint8_t kMagic[4] = {'D', 'S', '2', 'R'};
constexpr size_t kHeaderSize = 5;       // magic and version
constexpr size_t kItemHeaderSize = 5;   // kind and u32 LE length

uint32_t u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint32_t u32be(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]);
}

void putLe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
}

void putBookmark(std::vector<uint8_t>& out, const Bookmark& b) {
    for (size_t k = 0; k < kBookmarkSize; ++k) out.push_back(k < b.size() ? b[k] : 0);
}

void putItem(std::vector<uint8_t>& out, ReadItemKind kind, const std::vector<uint8_t>& payload) {
    out.push_back(static_cast<uint8_t>(kind));
    putLe32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

void checkStream(uint8_t stream, const char* what) {
    if (stream < 1 || stream > 4)
        throw ReadFileError(std::string(what) + ": stream " + std::to_string(stream) +
                            " is not 1..4");
}

std::string textOf(const ReadAnswer* a) {
    if (a == nullptr) return {};
    std::string s;
    for (uint8_t c : a->data) {
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

}  // namespace

const ReadAnswer* ReadFile::answer(uint16_t command, uint8_t sub) const {
    for (const auto& a : answers)
        if (a.command == command && a.sub == sub && a.status == 0) return &a;
    return nullptr;
}

std::string ReadFile::machineSerial() const { return textOf(answer(0x002b, 0x01)); }
std::string ReadFile::model() const { return textOf(answer(0x002c, 0x01)); }
std::string ReadFile::firmware() const { return textOf(answer(0x000d, 0x03)); }

std::optional<uint32_t> ReadFile::newestSession() const {
    const ReadAnswer* a = answer(0x009a, 0x09);
    if (a == nullptr || a->data.size() < 4) return std::nullopt;
    return u32be(a->data.data());
}

ReadFile parseReadFile(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kHeaderSize || std::memcmp(data, kMagic, 4) != 0)
        throw ReadFileError("not a read file (no DS2R header)");
    ReadFile f;
    f.version = data[4];
    if (f.version != kReadFileVersion)
        throw ReadFileError("read file version " + std::to_string(f.version) +
                            " is not supported");

    size_t at = kHeaderSize;
    while (at < size) {
        if (f.end) throw ReadFileError("an item after END");
        if (size - at < kItemHeaderSize) throw ReadFileError("an item header cut short");
        const uint8_t kind = data[at];
        const uint32_t len = u32le(data + at + 1);
        at += kItemHeaderSize;
        if (len > size - at) throw ReadFileError("an item cut short");
        const uint8_t* p = data + at;
        at += len;

        switch (static_cast<ReadItemKind>(kind)) {
            case ReadItemKind::Answer: {
                if (len < 4) throw ReadFileError("ANSWER shorter than its header");
                ReadAnswer a;
                a.command = static_cast<uint16_t>(p[0] << 8 | p[1]);
                a.sub = p[2];
                a.status = p[3];
                a.data.assign(p + 4, p + len);
                f.answers.push_back(std::move(a));
                break;
            }
            case ReadItemKind::Record: {
                if (len < 5 + kBookmarkSize) throw ReadFileError("RECORD shorter than its header");
                ReadRecord r;
                r.stream = p[0];
                checkStream(r.stream, "RECORD");
                r.session = u32be(p + 1);
                r.bookmark.assign(p + 5, p + 5 + kBookmarkSize);
                r.body.assign(p + 5 + kBookmarkSize, p + len);
                f.records.push_back(std::move(r));
                break;
            }
            case ReadItemKind::StreamEnd: {
                if (len < 4 + kBookmarkSize) throw ReadFileError("STREAM_END too short");
                ReadStreamEnd e;
                e.stream = p[0];
                checkStream(e.stream, "STREAM_END");
                e.how = p[1];
                e.records = static_cast<uint16_t>(p[2] | p[3] << 8);
                e.bookmark.assign(p + 4, p + 4 + kBookmarkSize);
                f.stream_ends.push_back(std::move(e));
                break;
            }
            case ReadItemKind::End: {
                if (len < 3) throw ReadFileError("END too short");
                f.end = ReadEnd{p[0], p[1] != 0, p[2]};
                break;
            }
            case ReadItemKind::Sealed: {
                constexpr size_t kHead = 5 + kBookmarkSize + 1;   // stream, session, bookmark, path_len
                if (len < kHead) throw ReadFileError("SEALED shorter than its header");
                ReadSealed s;
                s.stream = p[0];
                if (s.stream > 4)
                    throw ReadFileError("SEALED: stream " + std::to_string(s.stream) +
                                        " is not 0..4");
                s.session = u32be(p + 1);
                s.bookmark.assign(p + 5, p + 5 + kBookmarkSize);
                const size_t path_len = p[5 + kBookmarkSize];
                if (path_len > len - kHead) throw ReadFileError("SEALED path past its item");
                s.path.assign(reinterpret_cast<const char*>(p + kHead), path_len);
                s.card.assign(p + kHead + path_len, p + len);
                f.sealed.push_back(std::move(s));
                break;
            }
            default:
                // A newer reader's kind: read past it (its length is known).
                ++f.skipped_items;
                break;
        }
    }
    return f;
}

ReadFile parseReadFile(const std::vector<uint8_t>& bytes) {
    return parseReadFile(bytes.data(), bytes.size());
}

std::vector<uint8_t> writeReadFile(const ReadFile& file) {
    std::vector<uint8_t> out(kMagic, kMagic + 4);
    out.push_back(file.version);
    for (const auto& a : file.answers) {
        std::vector<uint8_t> p{static_cast<uint8_t>(a.command >> 8),
                               static_cast<uint8_t>(a.command), a.sub, a.status};
        p.insert(p.end(), a.data.begin(), a.data.end());
        putItem(out, ReadItemKind::Answer, p);
    }
    for (const auto& r : file.records) {
        std::vector<uint8_t> p{r.stream, static_cast<uint8_t>(r.session >> 24),
                               static_cast<uint8_t>(r.session >> 16),
                               static_cast<uint8_t>(r.session >> 8),
                               static_cast<uint8_t>(r.session)};
        putBookmark(p, r.bookmark);
        p.insert(p.end(), r.body.begin(), r.body.end());
        putItem(out, ReadItemKind::Record, p);
    }
    for (const auto& s : file.sealed) {
        if (s.path.size() > 255) throw ReadFileError("SEALED path over 255 bytes");
        std::vector<uint8_t> p{s.stream, static_cast<uint8_t>(s.session >> 24),
                               static_cast<uint8_t>(s.session >> 16),
                               static_cast<uint8_t>(s.session >> 8),
                               static_cast<uint8_t>(s.session)};
        putBookmark(p, s.bookmark);
        p.push_back(static_cast<uint8_t>(s.path.size()));
        p.insert(p.end(), s.path.begin(), s.path.end());
        p.insert(p.end(), s.card.begin(), s.card.end());
        putItem(out, ReadItemKind::Sealed, p);
    }
    for (const auto& e : file.stream_ends) {
        std::vector<uint8_t> p{e.stream, e.how, static_cast<uint8_t>(e.records),
                               static_cast<uint8_t>(e.records >> 8)};
        putBookmark(p, e.bookmark);
        putItem(out, ReadItemKind::StreamEnd, p);
    }
    if (file.end)
        putItem(out, ReadItemKind::End,
                {file.end->result, static_cast<uint8_t>(file.end->more ? 1 : 0),
                 file.end->status});
    return out;
}

}  // namespace cpapdash::ds2ble
