#include <gtest/gtest.h>

#include <vector>

#include "fh/parser/parser.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;

namespace {
std::vector<Msg> parse_all(const std::vector<uint8_t>& buf, ParseStats* st_out = nullptr) {
    std::vector<Msg> out;
    ParseStats st = parse_buffer(buf.data(), buf.data() + buf.size(), [&](const Msg& m) { out.push_back(m); });
    if (st_out) *st_out = st;
    return out;
}
}  // namespace

// Fully hand-written bytes, independent of ItchWriter, so a bug in the encoder
// cannot hide a matching bug in the decoder.
TEST(Parser, AddOrderFromLiteralBytes) {
    const std::vector<uint8_t> buf = {
        0x00, 0x24,                                      // length 36, big endian
        'A',                                             // type
        0x01, 0x02,                                      // locate 0x0102 = 258
        0x00, 0x07,                                      // tracking
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,              // timestamp 0x123456789ABC
        0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF,  // order ref 0xDEADBEEF
        'S',                                             // side
        0x00, 0x00, 0x01, 0x2C,                          // shares 300
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',          // stock
        0x00, 0x12, 0xD6, 0x44,                          // price 1234500 = $123.45
    };
    ASSERT_EQ(buf.size(), 38u);
    ParseStats st;
    const auto msgs = parse_all(buf, &st);
    ASSERT_EQ(msgs.size(), 1u);
    const Msg& m = msgs[0];
    EXPECT_EQ(m.type, MsgType::AddOrder);
    EXPECT_EQ(m.locate, 258);
    EXPECT_EQ(m.timestamp, 0x123456789ABCull);
    EXPECT_EQ(m.ref, 0xDEADBEEFull);
    EXPECT_EQ(m.side, Side::Sell);
    EXPECT_EQ(m.shares, 300u);
    EXPECT_EQ(m.price, 1234500u);
    EXPECT_DOUBLE_EQ(m.price / 10000.0, 123.45);
    EXPECT_EQ(st.messages, 1u);
    EXPECT_EQ(st.count('A'), 1u);
    EXPECT_EQ(st.bytes, buf.size());
}

TEST(Parser, EndianHelpers) {
    const uint8_t b[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(load_be16(b), 0x0102);
    EXPECT_EQ(load_be32(b), 0x01020304u);
    EXPECT_EQ(load_be48(b), 0x010203040506ull);
    EXPECT_EQ(load_be64(b), 0x0102030405060708ull);
    uint8_t w[8];
    store_be48(w, 0xA1B2C3D4E5F6ull);
    EXPECT_EQ(load_be48(w), 0xA1B2C3D4E5F6ull);
}

TEST(Parser, EveryBookTypeDecodes) {
    synth::ItchWriter w;
    const uint64_t ts = 34'200'000'000'123ull;  // 09:30:00.000000123
    w.stock_directory(7, ts, "MSFT");
    w.add(7, ts, 1001, 'B', 100, "MSFT", 1575000);
    w.add_mpid(7, ts, 1002, 'S', 250, "MSFT", 1576000, "GSCO");
    w.executed(7, ts, 1001, 40, 555);
    w.executed_price(7, ts, 1002, 50, 556, 'Y', 1575500);
    w.cancel(7, ts, 1002, 25);
    w.del(7, ts, 1001);
    w.replace(7, ts, 1002, 2002, 400, 4294967000u);
    const auto msgs = parse_all(w.bytes());
    ASSERT_EQ(msgs.size(), 8u);
    for (const auto& m : msgs) {
        EXPECT_EQ(m.locate, 7);
        EXPECT_EQ(m.timestamp, ts);
    }
    EXPECT_EQ(msgs[0].type, MsgType::StockDirectory);
    EXPECT_EQ(std::string(msgs[0].symbol, 8), "MSFT    ");

    EXPECT_EQ(msgs[1].type, MsgType::AddOrder);
    EXPECT_EQ(msgs[1].ref, 1001u);
    EXPECT_EQ(msgs[1].side, Side::Buy);
    EXPECT_EQ(msgs[1].shares, 100u);
    EXPECT_EQ(msgs[1].price, 1575000u);

    EXPECT_EQ(msgs[2].type, MsgType::AddOrderMpid);
    EXPECT_EQ(msgs[2].ref, 1002u);
    EXPECT_EQ(msgs[2].side, Side::Sell);
    EXPECT_EQ(msgs[2].shares, 250u);
    EXPECT_EQ(msgs[2].price, 1576000u);

    EXPECT_EQ(msgs[3].type, MsgType::OrderExecuted);
    EXPECT_EQ(msgs[3].ref, 1001u);
    EXPECT_EQ(msgs[3].shares, 40u);

    EXPECT_EQ(msgs[4].type, MsgType::OrderExecutedPrice);
    EXPECT_EQ(msgs[4].ref, 1002u);
    EXPECT_EQ(msgs[4].shares, 50u);
    EXPECT_EQ(msgs[4].price, 1575500u);

    EXPECT_EQ(msgs[5].type, MsgType::OrderCancel);
    EXPECT_EQ(msgs[5].ref, 1002u);
    EXPECT_EQ(msgs[5].shares, 25u);

    EXPECT_EQ(msgs[6].type, MsgType::OrderDelete);
    EXPECT_EQ(msgs[6].ref, 1001u);

    EXPECT_EQ(msgs[7].type, MsgType::OrderReplace);
    EXPECT_EQ(msgs[7].ref, 1002u);
    EXPECT_EQ(msgs[7].new_ref, 2002u);
    EXPECT_EQ(msgs[7].shares, 400u);
    EXPECT_EQ(msgs[7].price, 4294967000u);  // full 32-bit range survives the swap
}

TEST(Parser, LargeFieldValuesByteSwapCorrectly) {
    synth::ItchWriter w;
    const uint64_t max_ts = (uint64_t{1} << 48) - 1;
    w.add(0xFFFF, max_ts, 0xFEDCBA9876543210ull, 'B', 0xFFFFFFFFu, "ZZZZZZZZ", 0x77359400u);  // max price 200000.0000
    const auto msgs = parse_all(w.bytes());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].locate, 0xFFFF);
    EXPECT_EQ(msgs[0].timestamp, max_ts);
    EXPECT_EQ(msgs[0].ref, 0xFEDCBA9876543210ull);
    EXPECT_EQ(msgs[0].shares, 0xFFFFFFFFu);
    EXPECT_EQ(msgs[0].price, 2'000'000'000u);
}

TEST(Parser, SkipsAndCountsOtherTypes) {
    synth::ItchWriter w;
    w.system_event(1, 'O');
    w.trade(1, 2, 0, 'B', 100, "X", 10000, 1);
    w.raw('I', 50, 1, 3);
    w.raw('H', 25, 1, 4);
    w.add(1, 5, 9, 'B', 1, "X", 1);
    w.raw('Q', 40, 1, 6);
    ParseStats st;
    const auto msgs = parse_all(w.bytes(), &st);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(st.messages, 6u);
    EXPECT_EQ(st.count('S'), 1u);
    EXPECT_EQ(st.count('P'), 1u);
    EXPECT_EQ(st.count('I'), 1u);
    EXPECT_EQ(st.count('H'), 1u);
    EXPECT_EQ(st.count('A'), 1u);
    EXPECT_EQ(st.count('Q'), 1u);
    EXPECT_FALSE(st.truncated);
}

TEST(Parser, ShortMessageIsMalformedNotDecoded) {
    synth::ItchWriter w;
    w.raw('A', 20, 1, 1);  // an Add must be 36 bytes
    w.add(1, 2, 3, 'S', 10, "X", 100);
    ParseStats st;
    const auto msgs = parse_all(w.bytes(), &st);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].ref, 3u);
    EXPECT_EQ(st.malformed, 1u);
    EXPECT_EQ(st.count('A'), 2u);
}

TEST(Parser, TruncatedTailStopsCleanly) {
    synth::ItchWriter w;
    w.add(1, 1, 1, 'B', 10, "X", 100);
    w.add(1, 2, 2, 'B', 10, "X", 100);
    auto buf = w.bytes();
    buf.resize(buf.size() - 5);  // cut the second message
    ParseStats st;
    const auto msgs = parse_all(buf, &st);
    EXPECT_EQ(msgs.size(), 1u);
    EXPECT_TRUE(st.truncated);
    EXPECT_EQ(st.bytes, 38u);
}

TEST(Parser, MessageStructIs48Bytes) { EXPECT_EQ(sizeof(Msg), 48u); }
