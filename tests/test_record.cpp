#include "FakeMachine.h"
#include "cpapdash/ds2ble/Record.h"

#include <gtest/gtest.h>

using namespace cpapdash::ds2ble;
using ds2test::Bytes;
using ds2test::makeBlock;

TEST(Record, CardFileNames) {
    EXPECT_EQ(cardFileName(1, 1234), "000004D2.B01");
    EXPECT_EQ(cardFileName(2, 0x29f), "0000029F.B02");
    EXPECT_EQ(cardFileName(3, 1), "00000001.B03");
    EXPECT_EQ(cardFileName(4, 0xFFFFFFFF), "FFFFFFFF.B04");
    EXPECT_STREQ(streamExtension(0), "");
    EXPECT_STREQ(streamExtension(5), "");
}

TEST(Record, CrcOfTheContainer) {
    // MSB-first CRC-32, poly 0x04C11DB7, init all ones, no final xor: of no
    // bytes it is the init value.
    EXPECT_EQ(blockCrc(nullptr, 0), 0xFFFFFFFFu);
    // Each byte is fed as the word 00 00 00 bb, so 1 byte and its word form agree.
    const uint8_t one[] = {0x31};
    const uint32_t a = blockCrc(one, 1);
    EXPECT_NE(a, 0xFFFFFFFFu);
    uint32_t crc = 0xFFFFFFFFu ^ 0x31u;
    for (int k = 0; k < 32; ++k) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    EXPECT_EQ(a, crc);
}

TEST(Record, VerifiesEveryBlock) {
    Bytes body = makeBlock(100, 1700000000, 1, {{0x03, {0x00, 0x00}}, {0x04, {0x10, 0x0e}}});
    const Bytes second = makeBlock(100, 1700003600, 1, {{0x02, {0x20, 0x00}}});
    body.insert(body.end(), second.begin(), second.end());
    const auto blocks = verifyBody(body);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].session, 100u);
    EXPECT_EQ(blocks[0].timestamp, 1700000000u);
    EXPECT_EQ(blocks[0].extension, 1);
    EXPECT_EQ(blocks[0].family_version, 6);
    EXPECT_EQ(blocks[1].timestamp, 1700003600u);
}

TEST(Record, RefusesDamage) {
    const Bytes good = makeBlock(7, 1700000000, 2, {{0x06, {0x05, 0x00, 0x09}}});
    ASSERT_NO_THROW(verifyBody(good));

    Bytes data_hit = good;
    data_hit[good.size() - 6] ^= 0xff;   // a data byte
    EXPECT_THROW(verifyBody(data_hit), RecordError);

    Bytes head_hit = good;
    head_hit[11] ^= 0x01;                // the time, covered by the header sum
    EXPECT_THROW(verifyBody(head_hit), RecordError);

    Bytes version = good;
    version[0] = 2;
    EXPECT_THROW(verifyBody(version), RecordError);

    EXPECT_THROW(verifyBody(Bytes(good.begin(), good.end() - 1)), RecordError);
    EXPECT_THROW(verifyBody(Bytes(10, 0)), RecordError);
    EXPECT_TRUE(verifyBody({}).empty());
}
