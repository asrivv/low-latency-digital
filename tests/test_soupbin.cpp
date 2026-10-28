#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lle/ouch.hpp"
#include "lle/soupbin.hpp"

using namespace lle;

namespace {

struct Frame {
    char type;
    std::vector<std::byte> payload;
    bool operator==(const Frame&) const = default;
};

std::vector<std::byte> sample_stream(std::vector<Frame>& expected) {
    std::vector<std::byte> buf(4096);
    std::size_t n = 0;
    auto add = [&](std::size_t len) {
        const std::byte* p = buf.data() + n;
        expected.push_back({static_cast<char>(p[2]), {p + 3, p + len}});
        n += len;
    };
    add(soupbin::encode_login_request(buf.data() + n, {"user", "pass", "", 1}));
    for (int i = 0; i < 5; ++i) {
        ouch::EnterOrder m{Token::from("T" + std::to_string(i)), 'B', 100u * (i + 1), symbol_key("AAPL"),
                           1'900'000, 99'999, 0, 'Y', 'A', 'N', 0, 'N', 'R'};
        std::byte msg[64];
        std::size_t len = ouch::encode(m, msg);
        add(soupbin::encode_frame(buf.data() + n, 'U', msg, len));
    }
    add(soupbin::encode_empty(buf.data() + n, 'R'));
    ouch::CancelOrder c{Token::from("T1"), 0};
    std::byte msg[32];
    add(soupbin::encode_frame(buf.data() + n, 'U', msg, ouch::encode(c, msg)));
    add(soupbin::encode_empty(buf.data() + n, 'O'));
    buf.resize(n);
    return buf;
}

// Feeds `stream` in the given chunks through a residual buffer the way the gateway does.
std::vector<Frame> feed(const std::vector<std::byte>& stream, const std::vector<std::size_t>& cuts) {
    std::vector<Frame> out;
    std::vector<std::byte> pending;
    std::size_t prev = 0;
    auto chunks = cuts;
    chunks.push_back(stream.size());
    for (std::size_t cut : chunks) {
        pending.insert(pending.end(), stream.begin() + prev, stream.begin() + cut);
        prev = cut;
        auto r = soupbin::parse(pending.data(), pending.size(), [&](char t, const std::byte* p, std::size_t n) {
            out.push_back({t, {p, p + n}});
            return true;
        });
        EXPECT_FALSE(r.error);
        pending.erase(pending.begin(), pending.begin() + r.consumed);
    }
    EXPECT_TRUE(pending.empty());
    return out;
}

}  // namespace

TEST(SoupBin, ManyFramesInOneBuffer) {
    std::vector<Frame> expected;
    auto stream = sample_stream(expected);
    EXPECT_EQ(feed(stream, {}), expected);
    EXPECT_EQ(expected.size(), 9u);
}

TEST(SoupBin, SplitAtEveryByteBoundary) {
    std::vector<Frame> expected;
    auto stream = sample_stream(expected);
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) EXPECT_EQ(feed(stream, {cut}), expected) << cut;
}

TEST(SoupBin, OneByteAtATime) {
    std::vector<Frame> expected;
    auto stream = sample_stream(expected);
    std::vector<std::size_t> cuts;
    for (std::size_t i = 1; i < stream.size(); ++i) cuts.push_back(i);
    EXPECT_EQ(feed(stream, cuts), expected);
}

TEST(SoupBin, UnknownTypeIsDeliveredAndSkipped) {
    std::byte buf[64];
    std::size_t n = 0;
    std::byte junk[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
    n += soupbin::encode_frame(buf + n, '?', junk, 3);
    n += soupbin::encode_empty(buf + n, 'R');
    std::vector<char> types;
    auto r = soupbin::parse(buf, n, [&](char t, const std::byte*, std::size_t) {
        types.push_back(t);
        return true;
    });
    EXPECT_FALSE(r.error);
    EXPECT_EQ(r.consumed, n);
    EXPECT_EQ(types, (std::vector<char>{'?', 'R'}));
}

TEST(SoupBin, StopsWhenCallbackReturnsFalse) {
    std::byte buf[16];
    std::size_t n = soupbin::encode_empty(buf, 'O');
    n += soupbin::encode_empty(buf + n, 'R');
    int calls = 0;
    auto r = soupbin::parse(buf, n, [&](char, const std::byte*, std::size_t) {
        ++calls;
        return false;
    });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(r.consumed, 3u);
}

TEST(SoupBin, ZeroLengthAndOversizeAreErrors) {
    std::byte zero[4] = {};
    auto r = soupbin::parse(zero, 4, [](char, const std::byte*, std::size_t) { return true; });
    EXPECT_TRUE(r.error);
    EXPECT_EQ(r.consumed, 0u);

    std::byte big[3];
    soupbin::encode_header(big, 'U', 500);
    r = soupbin::parse(big, 3, [](char, const std::byte*, std::size_t) { return true; }, 64);
    EXPECT_TRUE(r.error);
}

TEST(SoupBin, LoginBytes) {
    std::byte buf[64];
    std::size_t n = soupbin::encode_login_request(buf, {"abc", "pw", "", 17});
    ASSERT_EQ(n, 49u);
    std::string s(reinterpret_cast<const char*>(buf), n);
    EXPECT_EQ(s, std::string("\x00\x2f", 2) + "L" + "abc   " + "pw        " + "          " +
                     "                  17");
    EXPECT_EQ(soupbin::login_requested_sequence(buf + 3), 17u);

    n = soupbin::encode_login_accepted(buf, soupbin::kSessionName, 1);
    ASSERT_EQ(n, 33u);
    s.assign(reinterpret_cast<const char*>(buf), n);
    EXPECT_EQ(s, std::string("\x00\x1f", 2) + "A" + "LLE0000001" + "                   1");

    n = soupbin::encode_login_rejected(buf, 'A');
    s.assign(reinterpret_cast<const char*>(buf), n);
    EXPECT_EQ(s, std::string("\x00\x02", 2) + "JA");

    n = soupbin::encode_empty(buf, 'H');
    s.assign(reinterpret_cast<const char*>(buf), n);
    EXPECT_EQ(s, std::string("\x00\x01", 2) + "H");
}
