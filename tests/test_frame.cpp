#include "cpapdash/ds2ble/Frame.h"

#include <gtest/gtest.h>

using namespace cpapdash::ds2ble;

TEST(Frame, KnownRequestsAreByteExact) {
    // The bytes the machine's own app writes for these requests.
    EXPECT_EQ(toHex(encode(0x002B, 0x01)), "1001002b0110021003d1ac");
    EXPECT_EQ(toHex(encode(0x0000, 0x00)), "1001000000100210035555");
    EXPECT_EQ(toHex(encode(0x009A, 0x09)), "1001009a09100210037393");
    EXPECT_EQ(toHex(encode(0x009A, 0x02, {0, 0, 0, 1})), "1001009a021002000000011003b755");
    EXPECT_EQ(toHex(encode(0x009A, 0x01, {0x00})), "1001009a011002001003927c");
}

TEST(Frame, RoundTrips) {
    for (int n = 0; n < 300; ++n) {
        std::vector<uint8_t> data;
        for (int k = 0; k < n % 40; ++k) data.push_back(static_cast<uint8_t>((n * 31 + k * 7) & 0xff));
        const auto raw = encode(static_cast<uint16_t>(n * 0x0101), static_cast<uint8_t>(n), data);
        const Frame f = decode(raw);
        EXPECT_EQ(f.cmd, static_cast<uint16_t>(n * 0x0101));
        EXPECT_EQ(f.sub, static_cast<uint8_t>(n));
        EXPECT_EQ(f.data, data);
    }
}

TEST(Frame, DleInsideIsDoubledAndUndoubled) {
    const auto raw = encode(0x0010, 0x10, {0x00, 0x10, 0x03});
    EXPECT_EQ(toHex(raw).substr(0, 14), "10010010101010");   // cmd 00 10, sub 10, both doubled
    EXPECT_NE(toHex(raw).find("101003"), std::string::npos);
    const Frame f = decode(raw);
    EXPECT_EQ(f.cmd, 0x0010);
    EXPECT_EQ(f.sub, 0x10);
    EXPECT_EQ(f.data, (std::vector<uint8_t>{0x00, 0x10, 0x03}));
}

TEST(Frame, CheckIsInTheOneTo255Form) {
    // A Fletcher-16 mod 255 in the 0..254 form would put a 0x00 where this
    // form puts 0xff. Find payloads that hit it and check both directions.
    int hits = 0;
    for (uint32_t v = 0; v < 70000 && hits < 5; ++v) {
        std::vector<uint8_t> d{static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        std::vector<uint8_t> p{0x00, 0x9a, 0x02, d[0], d[1]};
        const auto c = checksum(p.data(), p.size());
        EXPECT_NE(c[0], 0);
        EXPECT_NE(c[1], 0);
        if (c[0] == 0xff || c[1] == 0xff) {
            ++hits;
            EXPECT_EQ(decode(encode(0x009a, 0x02, d)).data, d);
        }
    }
    EXPECT_EQ(hits, 5);
}

TEST(Frame, CheckBytesAreNeverDoubled) {
    // A frame may end in a bare 0x10 check byte.
    for (uint32_t v = 0; v < 70000; ++v) {
        std::vector<uint8_t> d{static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        std::vector<uint8_t> p{0x00, 0x9a, 0x02, d[0], d[1]};
        const auto c = checksum(p.data(), p.size());
        if (c[1] == 0x10) {
            const auto raw = encode(0x009a, 0x02, d);
            EXPECT_EQ(raw.back(), 0x10);
            EXPECT_EQ(raw[raw.size() - 2], c[0]);
            EXPECT_EQ(decode(raw).data, d);
            return;
        }
    }
    FAIL() << "no payload with a 0x10 check byte";
}

TEST(Frame, BadCheckThrows) {
    auto raw = encode(0x002B, 0x01);
    raw.back() ^= 1;
    EXPECT_THROW(decode(raw), FrameError);
}

TEST(Frame, ShortOrTrailingThrows) {
    auto raw = encode(0x002B, 0x01, {1, 2, 3});
    EXPECT_THROW(decode(std::vector<uint8_t>(raw.begin(), raw.end() - 1)), FrameError);
    raw.push_back(0);
    EXPECT_THROW(decode(raw), FrameError);
    EXPECT_THROW(decode(fromHex("10011002")), FrameError);
}

TEST(Deframer, SplitJoinedAndNoise) {
    const auto a = encode(0x002B, 0x01, {0x00, 'D', '1', 0x00});
    const auto b = encode(0x009A, 0x02, {0x00, 0x10, 0x10, 0x03});
    std::vector<uint8_t> stream{0x55, 0x10};   // noise before a frame, and a lone 0x10
    stream.insert(stream.end(), a.begin(), a.end());
    stream.insert(stream.end(), b.begin(), b.end());
    for (size_t step : {1u, 2u, 3u, 7u, 1000u}) {
        Deframer d;
        std::vector<Frame> got;
        for (size_t k = 0; k < stream.size(); k += step) {
            auto part = d.feed(stream.data() + k, std::min(step, stream.size() - k));
            got.insert(got.end(), part.begin(), part.end());
        }
        ASSERT_EQ(got.size(), 2u) << "step " << step;
        EXPECT_EQ(got[0].data, (std::vector<uint8_t>{0x00, 'D', '1', 0x00}));
        EXPECT_EQ(got[1].data, (std::vector<uint8_t>{0x00, 0x10, 0x10, 0x03}));
        EXPECT_EQ(d.pending(), 0u);
    }
}

TEST(Hex, RoundTrip) {
    EXPECT_EQ(toHex(fromHex("00Ff10a5")), "00ff10a5");
    EXPECT_THROW(fromHex("abc"), std::invalid_argument);
    EXPECT_THROW(fromHex("zz"), std::invalid_argument);
}
