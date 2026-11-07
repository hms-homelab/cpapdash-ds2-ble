#include "FakeMachine.h"
#include "cpapdash/ds2ble/Protocol.h"
#include "cpapdash/ds2ble/SyncSession.h"

#include <gtest/gtest.h>

using namespace cpapdash::ds2ble;
using ds2test::Bytes;
using ds2test::FakeMachine;
using ds2test::makeBlock;

namespace {

/// A machine with `n` sessions from 500 on: a summary and an events record
/// each (the summary over two chunks), and one record in each log stream.
FakeMachine machineWith(int n, uint32_t first = 500) {
    FakeMachine m;
    for (int i = 0; i < n; ++i) {
        const uint32_t s = first + static_cast<uint32_t>(i);
        Bytes pad(250, static_cast<uint8_t>(i));
        m.streams[1].push_back({s, makeBlock(s, 1700000000 + 86400 * i, 1, {{0x00, pad}})});
        m.streams[2].push_back({s, makeBlock(s, 1700000000 + 86400 * i, 2, {{0x06, {1, 0, 9}}})});
    }
    m.streams[3].push_back({first, makeBlock(first, 1700000000, 3, {{0x00, Bytes(16, 3)}})});
    m.streams[4].push_back({first, makeBlock(first, 1700000000, 4, {{0x01, Bytes(25, 4)}})});
    return m;
}

SyncSession read(FakeMachine& m, SyncOptions o = {}, size_t split = 0) {
    SyncSession s(std::move(o));
    ds2test::drive(s, m, split);
    return s;
}

}  // namespace

TEST(Sync, FirstReadWalksEveryStreamFromTheOldest) {
    FakeMachine m = machineWith(3);
    SyncSession s = read(m);
    ASSERT_TRUE(s.done()) << s.error();
    EXPECT_EQ(s.identity().serial, "TEST0000000001");
    EXPECT_EQ(s.identity().model, "520X");
    EXPECT_EQ(s.identity().firmware, "V9.9");
    EXPECT_EQ(s.identity().newest_session, 502u);
    EXPECT_EQ(s.identity().commands, (std::vector<uint16_t>{0x0000, 0x0020, 0x002b, 0x009a}));
    EXPECT_FALSE(s.identity().clock_set);
    EXPECT_TRUE(m.clock_writes.empty());

    ASSERT_EQ(s.records().size(), 8u);
    for (const auto& r : s.records()) {
        EXPECT_NO_THROW(verifyBody(r.body));
        EXPECT_EQ(r.bookmark_out.size(), kBookmarkSize);
    }
    EXPECT_EQ(s.records()[0].stream, 1);
    EXPECT_EQ(s.records()[0].session, 500u);
    EXPECT_TRUE(s.records()[0].bookmark_in.empty());
    EXPECT_EQ(s.records()[1].bookmark_in, s.records()[0].bookmark_out);
    EXPECT_EQ(s.records()[0].body, m.streams[1][0].body);
    EXPECT_EQ(s.records()[3].stream, 2);
    EXPECT_EQ(s.records()[6].stream, 3);
    EXPECT_EQ(s.records()[7].stream, 4);

    EXPECT_EQ(s.bookmarks().at(1), FakeMachine::bookmarkFor(1, 2));
    EXPECT_EQ(s.bookmarks().at(2), FakeMachine::bookmarkFor(2, 2));
    EXPECT_EQ(s.bookmarks().at(3), FakeMachine::bookmarkFor(3, 0));
    EXPECT_EQ(s.bookmarks().at(0), FakeMachine::bookmarkFor(0, 0));
    EXPECT_EQ(m.oldest_opens, 5);   // stream 0 and the four record streams, once each
}

TEST(Sync, ResumesFromTheStoredBookmarkNeverFromTheOldest) {
    FakeMachine m = machineWith(3);
    SyncSession first = read(m);
    ASSERT_TRUE(first.done());

    // Two nights later.
    for (uint32_t s : {503u, 504u}) {
        m.streams[1].push_back({s, makeBlock(s, 1700400000 + s, 1, {{0x00, Bytes(20, 1)}})});
        m.streams[2].push_back({s, makeBlock(s, 1700400000 + s, 2, {{0x06, {1, 0, 9}}})});
    }
    m.oldest_opens = 0;
    SyncOptions o;
    o.bookmarks = first.bookmarks();
    SyncSession next = read(m, o);
    ASSERT_TRUE(next.done()) << next.error();
    EXPECT_EQ(m.oldest_opens, 1);   // stream 0 only: it has no bookmark to send
    ASSERT_EQ(next.records().size(), 4u);
    EXPECT_EQ(next.records()[0].session, 503u);
    EXPECT_EQ(next.records()[0].bookmark_in, first.bookmarks().at(1));
    EXPECT_EQ(next.records()[1].session, 504u);
    EXPECT_EQ(next.records()[2].stream, 2);
    EXPECT_EQ(next.records()[2].session, 503u);
    EXPECT_EQ(next.bookmarks().at(1), FakeMachine::bookmarkFor(1, 4));
    // Streams with nothing new keep the bookmark they were given.
    EXPECT_EQ(next.bookmarks().at(3), first.bookmarks().at(3));
    EXPECT_EQ(next.bookmarks().at(4), first.bookmarks().at(4));
}

TEST(Sync, Status06MeansNothingAfterTheBookmark) {
    FakeMachine m = machineWith(2);
    m.status06_at_end = true;
    SyncSession first = read(m);
    ASSERT_TRUE(first.done()) << first.error();
    EXPECT_EQ(first.records().size(), 6u);

    SyncOptions o;
    o.bookmarks = first.bookmarks();
    SyncSession again = read(m, o);
    ASSERT_TRUE(again.done()) << again.error();
    EXPECT_TRUE(again.records().empty());
    EXPECT_EQ(again.bookmarks(), first.bookmarks());
}

TEST(Sync, Status06OnAnyOtherCommandFails) {
    // 06 is accepted on an open only.
    struct Bad : FakeMachine {
        Bytes answer(const Bytes& req) override {
            const Frame f = decode(req);
            if (f.cmd == kCmdModel) return encode(f.cmd, f.sub, {0x06});
            return FakeMachine::answer(req);
        }
    } m;
    SyncSession s = read(m);
    EXPECT_TRUE(s.failed());
    EXPECT_NE(s.error().find("002c/01 status 06"), std::string::npos) << s.error();
}

TEST(Sync, TheSessionInTherapyIsNotServed) {
    FakeMachine m = machineWith(2);
    m.open_session = 501;   // the mask is on: the machine withholds 501
    SyncSession during = read(m);
    ASSERT_TRUE(during.done());
    for (const auto& r : during.records()) EXPECT_NE(r.session, 501u);
    EXPECT_EQ(during.identity().newest_session, 501u);

    m.open_session.reset();   // mask off
    SyncOptions o;
    o.bookmarks = during.bookmarks();
    SyncSession after = read(m, o);
    ASSERT_TRUE(after.done());
    ASSERT_EQ(after.records().size(), 2u);
    EXPECT_EQ(after.records()[0].session, 501u);
    EXPECT_EQ(after.records()[1].session, 501u);
}

TEST(Sync, SplitNotificationsGiveTheSameRecords) {
    FakeMachine a = machineWith(3), b = machineWith(3);
    SyncSession whole = read(a);
    SyncSession bytewise = read(b, {}, 1);
    SyncSession odd = read(b, {}, 7);
    ASSERT_TRUE(whole.done() && bytewise.done() && odd.done());
    ASSERT_EQ(whole.records().size(), bytewise.records().size());
    for (size_t i = 0; i < whole.records().size(); ++i) {
        EXPECT_EQ(whole.records()[i].body, bytewise.records()[i].body);
        EXPECT_EQ(whole.records()[i].body, odd.records()[i].body);
    }
}

TEST(Sync, SetClockOnlyWhenAsked) {
    FakeMachine m = machineWith(1);
    SyncOptions o;
    o.set_clock_utc = 0x6aee38d1;
    SyncSession s = read(m, o);
    ASSERT_TRUE(s.done());
    EXPECT_TRUE(s.identity().clock_set);
    EXPECT_EQ(m.clock_writes, (std::vector<uint32_t>{0x6aee38d1}));
}

TEST(Sync, RecordsArriveAsTheyCome) {
    FakeMachine m = machineWith(2);
    std::vector<std::pair<uint8_t, uint32_t>> seen;
    SyncOptions o;
    o.on_record = [&](const Record& r) { seen.emplace_back(r.stream, r.session); };
    SyncSession s = read(m, o);
    ASSERT_TRUE(s.done());
    EXPECT_EQ(seen, (std::vector<std::pair<uint8_t, uint32_t>>{
                        {1, 500}, {1, 501}, {2, 500}, {2, 501}, {3, 500}, {4, 500}}));
}

TEST(Sync, OnlyTheStreamsAsked) {
    FakeMachine m = machineWith(2);
    SyncOptions o;
    o.streams = {1};
    SyncSession s = read(m, o);
    ASSERT_TRUE(s.done());
    EXPECT_EQ(s.records().size(), 2u);
    EXPECT_FALSE(s.bookmarks().count(2));
}

TEST(Sync, WrongAnswerFails) {
    struct Liar : FakeMachine {
        Bytes answer(const Bytes& req) override {
            const Frame f = decode(req);
            if (f.cmd == kCmdFirmware) return encode(kCmdModel, 0x01, {0, 'X', 0});
            return FakeMachine::answer(req);
        }
    } m;
    SyncSession s = read(m);
    EXPECT_TRUE(s.failed());
    EXPECT_NE(s.error().find("asked 000d/03, got 002c/01"), std::string::npos) << s.error();
}

TEST(Sync, ShortBodyFails) {
    struct Short : FakeMachine {
        Bytes answer(const Bytes& req) override {
            const Frame f = decode(req);
            Bytes a = FakeMachine::answer(req);
            if (f.cmd == kCmdCursor && f.sub == kCurChunk) {
                Frame g = decode(a);
                g.data.pop_back();
                return encode(g.cmd, g.sub, g.data);
            }
            return a;
        }
    } m;
    for (uint32_t s = 1; s <= 2; ++s)
        m.streams[1].push_back({s, makeBlock(s, 1700000000, 1, {{0x00, Bytes(250, 1)}})});
    SyncSession s = read(m);
    EXPECT_TRUE(s.failed());
    EXPECT_NE(s.error().find("the header said"), std::string::npos) << s.error();
    EXPECT_TRUE(s.records().empty());
}

TEST(Sync, AbortStopsIt) {
    SyncSession s;
    const Bytes first = s.begin();
    EXPECT_EQ(toHex(first), "1001002b0110021003d1ac");
    s.abort("link dropped");
    EXPECT_TRUE(s.failed());
    EXPECT_EQ(s.error(), "link dropped");
    EXPECT_TRUE(s.feed(encode(kCmdSerial, 0x01, {0, 'A', 0})).empty());
}

TEST(Sync, TwoAnswersToOneRequestFail) {
    SyncSession s;
    s.begin();
    Bytes two = encode(kCmdSerial, 0x01, {0, 'A', 0});
    const Bytes again = two;
    two.insert(two.end(), again.begin(), again.end());
    EXPECT_TRUE(s.feed(two).empty());
    EXPECT_TRUE(s.failed());
}
