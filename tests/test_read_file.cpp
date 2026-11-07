#include "cpapdash/ds2ble/ReadFile.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace cpapdash::ds2ble;
using Bytes = std::vector<uint8_t>;

namespace {

// A read file built by hand, byte by byte, from the documented layout, so the
// parser is checked against the format and not against its own writer.
void item(Bytes& out, uint8_t kind, const Bytes& payload) {
    out.push_back(kind);
    const auto n = static_cast<uint32_t>(payload.size());
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(n >> (8 * k)));
    out.insert(out.end(), payload.begin(), payload.end());
}

Bytes text(const std::string& s) {
    Bytes b(s.begin(), s.end());
    b.push_back(0);
    return b;
}

Bytes answer(uint16_t cmd, uint8_t sub, uint8_t status, const Bytes& data) {
    Bytes p{static_cast<uint8_t>(cmd >> 8), static_cast<uint8_t>(cmd), sub, status};
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

Bytes bookmark(uint8_t seed) {
    Bytes b(kBookmarkSize);
    for (size_t k = 0; k < b.size(); ++k) b[k] = static_cast<uint8_t>(seed + k);
    return b;
}

Bytes record(uint8_t stream, uint32_t session, const Bytes& bm, const Bytes& body) {
    Bytes p{stream, static_cast<uint8_t>(session >> 24), static_cast<uint8_t>(session >> 16),
            static_cast<uint8_t>(session >> 8), static_cast<uint8_t>(session)};
    p.insert(p.end(), bm.begin(), bm.end());
    p.insert(p.end(), body.begin(), body.end());
    return p;
}

Bytes streamEnd(uint8_t stream, uint8_t how, uint16_t records, const Bytes& bm) {
    Bytes p{stream, how, static_cast<uint8_t>(records), static_cast<uint8_t>(records >> 8)};
    p.insert(p.end(), bm.begin(), bm.end());
    return p;
}

Bytes header() { return {'D', 'S', '2', 'R', 0x01}; }

// The identity a read opens with, then two summaries, one events record, the
// stream ends and END. The serial is made up.
Bytes sample() {
    Bytes f = header();
    item(f, 0x01, answer(0x002b, 0x01, 0, text("X000000000001Z")));
    item(f, 0x01, answer(0x000d, 0x03, 0, text("V9.9.9.1")));
    item(f, 0x01, answer(0x002c, 0x01, 0, text("999X000Z")));
    item(f, 0x01, answer(0x009a, 0x09, 0, {0x00, 0x00, 0x01, 0x02}));
    item(f, 0x02, record(1, 256, bookmark(0x10), {0xAA, 0xBB, 0xCC}));
    item(f, 0x02, record(1, 257, bookmark(0x20), {0xDD}));
    item(f, 0x03, streamEnd(1, kStreamEndNothingAfter, 2, bookmark(0x20)));
    item(f, 0x02, record(2, 257, bookmark(0x30), {0x01, 0x02}));
    item(f, 0x03, streamEnd(2, kStreamEndStopped, 1, bookmark(0x30)));
    item(f, 0x04, {0x00, 0x01, 0x00});
    return f;
}

}  // namespace

TEST(ReadFile, ReadsEveryItemOfTheLayout) {
    const ReadFile f = parseReadFile(sample());
    EXPECT_EQ(f.version, 1);
    ASSERT_EQ(f.answers.size(), 4u);
    EXPECT_EQ(f.answers[0].command, 0x002b);
    EXPECT_EQ(f.answers[0].sub, 0x01);
    EXPECT_EQ(f.answers[0].status, 0);

    ASSERT_EQ(f.records.size(), 3u);
    EXPECT_EQ(f.records[0].stream, 1);
    EXPECT_EQ(f.records[0].session, 256u);
    EXPECT_EQ(f.records[0].bookmark, bookmark(0x10));
    EXPECT_EQ(f.records[0].body, (Bytes{0xAA, 0xBB, 0xCC}));
    EXPECT_EQ(f.records[1].session, 257u);
    EXPECT_EQ(f.records[1].body, Bytes{0xDD});
    EXPECT_EQ(f.records[2].stream, 2);
    EXPECT_EQ(f.records[2].bookmark, bookmark(0x30));

    ASSERT_EQ(f.stream_ends.size(), 2u);
    EXPECT_EQ(f.stream_ends[0].stream, 1);
    EXPECT_EQ(f.stream_ends[0].how, kStreamEndNothingAfter);
    EXPECT_EQ(f.stream_ends[0].records, 2);
    EXPECT_TRUE(f.stream_ends[0].reachedEnd());
    EXPECT_EQ(f.stream_ends[1].how, kStreamEndStopped);
    EXPECT_FALSE(f.stream_ends[1].reachedEnd());

    ASSERT_TRUE(f.end.has_value());
    EXPECT_EQ(f.end->result, 0);
    EXPECT_TRUE(f.end->more);
    EXPECT_EQ(f.end->status, 0);
}

TEST(ReadFile, TheIdentityAnswers) {
    const ReadFile f = parseReadFile(sample());
    EXPECT_EQ(f.machineSerial(), "X000000000001Z");
    EXPECT_EQ(f.firmware(), "V9.9.9.1");
    EXPECT_EQ(f.model(), "999X000Z");
    ASSERT_TRUE(f.newestSession().has_value());
    EXPECT_EQ(*f.newestSession(), 258u);
    EXPECT_EQ(f.answer(0x0098, 0x00), nullptr);
}

TEST(ReadFile, AnAnswerWithAStatusIsNotTheIdentity) {
    Bytes f = header();
    item(f, 0x01, answer(0x002b, 0x01, 0x05, text("NOPE")));
    const ReadFile r = parseReadFile(f);
    EXPECT_EQ(r.machineSerial(), "");
    EXPECT_FALSE(r.newestSession().has_value());
    EXPECT_FALSE(r.end.has_value());
}

TEST(ReadFile, AHeaderAloneIsAnEmptyRead) {
    const ReadFile r = parseReadFile(header());
    EXPECT_TRUE(r.answers.empty());
    EXPECT_TRUE(r.records.empty());
    EXPECT_FALSE(r.end.has_value());
}

TEST(ReadFile, RefusesWhatIsNotAVersionOneFile) {
    EXPECT_THROW(parseReadFile(Bytes{}), ReadFileError);
    EXPECT_THROW(parseReadFile(Bytes{'D', 'S', '2'}), ReadFileError);
    EXPECT_THROW(parseReadFile(Bytes{'D', 'S', '2', 'X', 1}), ReadFileError);
    EXPECT_THROW(parseReadFile(Bytes{'D', 'S', '2', 'R', 2}), ReadFileError);
}

TEST(ReadFile, RefusesAnItemCutShort) {
    Bytes f = sample();
    f.pop_back();   // END's last byte
    EXPECT_THROW(parseReadFile(f), ReadFileError);
    Bytes g = header();
    g.push_back(0x02);
    g.push_back(0x10);   // a length cut after one byte
    EXPECT_THROW(parseReadFile(g), ReadFileError);
}

TEST(ReadFile, RefusesItemsTooShortForTheirKind) {
    for (const auto& [kind, payload] : std::vector<std::pair<uint8_t, Bytes>>{
             {0x01, {0x00, 0x2b, 0x01}},                     // answer without status
             {0x02, Bytes(14, 0x01)},                        // record without a whole bookmark
             {0x03, Bytes(13, 0x01)},                        // stream end short
             {0x04, {0x00, 0x00}}}) {                        // end short
        Bytes f = header();
        item(f, kind, payload);
        EXPECT_THROW(parseReadFile(f), ReadFileError) << "kind " << int(kind);
    }
}

TEST(ReadFile, RefusesAnUnknownKindAStreamOutOfRangeAndItemsAfterEnd) {
    Bytes unknown = header();
    item(unknown, 0x09, {0x00});
    EXPECT_THROW(parseReadFile(unknown), ReadFileError);

    for (uint8_t stream : {uint8_t{0}, uint8_t{5}}) {
        Bytes f = header();
        item(f, 0x02, record(stream, 1, bookmark(1), {0x01}));
        EXPECT_THROW(parseReadFile(f), ReadFileError) << "stream " << int(stream);
        Bytes g = header();
        item(g, 0x03, streamEnd(stream, 0, 0, bookmark(1)));
        EXPECT_THROW(parseReadFile(g), ReadFileError) << "stream end " << int(stream);
    }

    Bytes after = sample();
    item(after, 0x02, record(1, 300, bookmark(2), {0x01}));
    EXPECT_THROW(parseReadFile(after), ReadFileError);
}

TEST(ReadFile, ARecordMayHaveAnEmptyBody) {
    Bytes f = header();
    item(f, 0x02, record(3, 0, bookmark(4), {}));
    const ReadFile r = parseReadFile(f);
    ASSERT_EQ(r.records.size(), 1u);
    EXPECT_TRUE(r.records[0].body.empty());
}

TEST(ReadFile, TheWriterRoundTrips) {
    // Same items, kind by kind (the writer does not keep how a reader
    // interleaved records and stream ends).
    const ReadFile f = parseReadFile(sample());
    const Bytes again = writeReadFile(f);
    EXPECT_EQ(again.size(), sample().size());
    const ReadFile g = parseReadFile(again);
    ASSERT_EQ(g.answers.size(), f.answers.size());
    ASSERT_EQ(g.records.size(), f.records.size());
    for (size_t k = 0; k < f.records.size(); ++k) {
        EXPECT_EQ(g.records[k].stream, f.records[k].stream);
        EXPECT_EQ(g.records[k].session, f.records[k].session);
        EXPECT_EQ(g.records[k].bookmark, f.records[k].bookmark);
        EXPECT_EQ(g.records[k].body, f.records[k].body);
    }
    ASSERT_EQ(g.stream_ends.size(), f.stream_ends.size());
    EXPECT_EQ(g.stream_ends[1].bookmark, f.stream_ends[1].bookmark);
    ASSERT_TRUE(g.end.has_value());
    EXPECT_TRUE(g.end->more);
    EXPECT_EQ(g.machineSerial(), f.machineSerial());
    EXPECT_EQ(writeReadFile(g), again);
}

TEST(ReadFile, TheWriterPadsABookmarkToTenBytes) {
    ReadFile f;
    ReadRecord r;
    r.stream = 1;
    r.session = 1;
    r.bookmark = {0x01, 0x02};   // short: written as ten bytes, zero padded
    r.body = {0x07};
    f.records.push_back(r);
    const ReadFile g = parseReadFile(writeReadFile(f));
    ASSERT_EQ(g.records.size(), 1u);
    EXPECT_EQ(g.records[0].bookmark, (Bytes{0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0}));
}
