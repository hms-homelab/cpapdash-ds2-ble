// Replays of a real capture: the machine's own app syncing a DreamStation 2.
// A capture holds a person's therapy records, so none is kept in this
// repository: point DS2BLE_CAPTURE at one (the text form, one frame per line,
// see Replay.h) to run these. Without it they are skipped.
//
// What they prove: the reader sends byte for byte what the app sent (the
// replay refuses anything else), every record body passes the container's
// checks, and the reader ends on the bookmarks the app itself saved and sent
// back on its next sync.

#include "Replay.h"
#include "cpapdash/ds2ble/Protocol.h"
#include "cpapdash/ds2ble/SyncSession.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>

using namespace cpapdash::ds2ble;
using namespace ds2test;

namespace {

const char* capturePath() { return std::getenv("DS2BLE_CAPTURE"); }

/// The bookmark the app sent with its LAST open of each stream: what it had
/// saved at the end of its reading.
std::map<uint8_t, Bytes> appsLastBookmarks(const std::vector<std::pair<bool, Bytes>>& cap) {
    std::map<uint8_t, Bytes> out;
    for (const auto& [app, raw] : cap) {
        if (!app) continue;
        const Frame f = decode(raw);
        if (f.cmd == kCmdCursor && f.sub == kCurOpen && f.data.size() == 1 + kBookmarkSize)
            out[f.data[0]] = Bytes(f.data.begin() + 1, f.data.end());
    }
    return out;
}

/// Expected record count per stream, "1:247,2:30,3:1,4:1", from DS2BLE_COUNTS.
std::map<uint8_t, int> expectedCounts() {
    std::map<uint8_t, int> out;
    const char* e = std::getenv("DS2BLE_COUNTS");
    if (!e) return out;
    std::string s = e;
    size_t i = 0;
    while (i < s.size()) {
        const size_t colon = s.find(':', i), comma = s.find(',', i);
        const int stream = std::stoi(s.substr(i, colon - i));
        out[static_cast<uint8_t>(stream)] = std::stoi(s.substr(colon + 1, comma - colon - 1));
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return out;
}

}  // namespace

class Capture : public ::testing::Test {
protected:
    void SetUp() override {
        if (!capturePath()) GTEST_SKIP() << "DS2BLE_CAPTURE not set";
        cap_ = readCapture(capturePath());
    }
    std::vector<std::pair<bool, Bytes>> cap_;
};

TEST_F(Capture, EveryFrameRoundTrips) {
    ASSERT_FALSE(cap_.empty());
    for (const auto& [app, raw] : cap_) {
        const Frame f = decode(raw);
        EXPECT_EQ(encode(f.cmd, f.sub, f.data), raw);
    }
    std::cout << "  " << cap_.size() << " frames" << std::endl;
}

TEST_F(Capture, FirstReadGivesEveryRecordVerified) {
    ReplayMachine m(cap_);
    SyncSession s;
    drive(s, m);
    ASSERT_TRUE(s.done()) << s.error();
    EXPECT_EQ(s.identity().model, "520X110C");
    EXPECT_FALSE(s.identity().serial.empty());
    EXPECT_GT(s.identity().newest_session, 0u);

    std::map<uint8_t, int> by;
    for (const auto& r : s.records()) {
        ++by[r.stream];
        ASSERT_NO_THROW(verifyBody(r.body)) << "stream " << int(r.stream) << " session " << r.session;
        const auto blocks = verifyBody(r.body);
        EXPECT_EQ(blocks.front().extension, r.stream);
        if (r.stream <= 2) EXPECT_EQ(blocks.front().session, r.session);
    }
    std::cout << "  records:";
    for (const auto& [k, v] : by) std::cout << " stream " << int(k) << " = " << v;
    std::cout << ", total " << s.records().size() << std::endl;
    const auto want = expectedCounts();
    if (!want.empty()) EXPECT_EQ(by, want);

    // The newest summary is the newest session the machine announced.
    uint32_t newest = 0;
    for (const auto& r : s.records())
        if (r.stream == 1) newest = std::max(newest, r.session);
    EXPECT_EQ(newest, s.identity().newest_session);

    // The reader ends where the app ended: the bookmarks it sent back later.
    const auto app = appsLastBookmarks(cap_);
    for (const auto& [stream, bm] : app) {
        ASSERT_TRUE(s.bookmarks().count(stream)) << "stream " << int(stream);
        EXPECT_EQ(toHex(s.bookmarks().at(stream)), toHex(bm)) << "stream " << int(stream);
    }
}

TEST_F(Capture, SplitNotificationsGiveTheSameBodies) {
    ReplayMachine a(cap_), b(cap_);
    SyncSession whole, split;
    drive(whole, a);
    drive(split, b, 5);
    ASSERT_TRUE(whole.done() && split.done());
    ASSERT_EQ(whole.records().size(), split.records().size());
    for (size_t i = 0; i < whole.records().size(); ++i)
        EXPECT_EQ(whole.records()[i].body, split.records()[i].body);
}

TEST_F(Capture, ResumingFromTheAppsBookmarksReadsNothingAndNeverTheOldest) {
    const auto app = appsLastBookmarks(cap_);
    ASSERT_FALSE(app.empty()) << "the capture has no sync that resumed from a bookmark";
    ReplayMachine m(cap_);
    SyncOptions o;
    for (const auto& [stream, bm] : app)
        if (stream != 0) o.bookmarks[stream] = bm;
    SyncSession s(o);
    drive(s, m);
    ASSERT_TRUE(s.done()) << s.error();
    EXPECT_TRUE(s.records().empty());
    EXPECT_EQ(m.oldestOpens, 1);   // stream 0, which is always opened bare
}

TEST_F(Capture, ResumingMidStreamReadsOnlyWhatFollows) {
    ReplayMachine first(cap_);
    SyncSession all;
    drive(all, first);
    ASSERT_TRUE(all.done());
    std::vector<const Record*> summaries;
    for (const auto& r : all.records())
        if (r.stream == 1) summaries.push_back(&r);
    ASSERT_GE(summaries.size(), 4u);
    const Record& from = *summaries[summaries.size() - 4];

    ReplayMachine m(cap_);
    SyncOptions o;
    o.streams = {1};
    o.bookmarks[1] = from.bookmark_out;
    SyncSession s(o);
    drive(s, m);
    ASSERT_TRUE(s.done()) << s.error();
    ASSERT_EQ(s.records().size(), 3u);
    EXPECT_EQ(s.records()[0].bookmark_in, from.bookmark_out);
    EXPECT_EQ(s.records()[0].body, summaries[summaries.size() - 3]->body);
    EXPECT_EQ(s.records()[2].body, summaries.back()->body);
    EXPECT_EQ(m.oldestOpens, 1);
}

TEST_F(Capture, ClockIsSetOnlyWhenAsked) {
    ReplayMachine m(cap_);
    SyncOptions o;
    o.set_clock_utc = 1800000000u;
    SyncSession s(o);
    drive(s, m);
    ASSERT_TRUE(s.done()) << s.error();
    EXPECT_TRUE(s.identity().clock_set);

    ReplayMachine bad(cap_);
    SyncSession probe;
    probe.begin();
    EXPECT_THROW(bad.answer(encode(0x00f6, 0x00)), UnexpectedRequest);
}
