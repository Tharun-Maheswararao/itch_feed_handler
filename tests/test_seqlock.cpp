#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "fh/view/live_view.hpp"
#include "fh/view/seqlock.hpp"

using namespace fh;

namespace {
struct Wide {
    uint64_t v[12];  // 96 bytes: a torn read would mix values
};
}  // namespace

TEST(Seqlock, EmptyUntilFirstStore) {
    Seqlock<Wide> s;
    Wide w;
    EXPECT_FALSE(s.load(w));
    s.store(Wide{{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}});
    EXPECT_TRUE(s.load(w));
    EXPECT_EQ(w.v[11], 1u);
}

// Writer stores snapshots whose words are all equal; a reader must never see
// a mix of two snapshots.
TEST(Seqlock, ReaderNeverSeesTornSnapshot) {
    Seqlock<Wide> s;
    std::atomic<bool> done{false};
    uint64_t torn = 0, reads = 0, last = 0, backwards = 0;
    std::thread reader([&] {
        Wide w;
        while (!done.load(std::memory_order_relaxed)) {
            if (!s.load(w)) continue;
            ++reads;
            for (int i = 1; i < 12; ++i) torn += (w.v[i] != w.v[0]);
            backwards += (w.v[0] < last);
            last = w.v[0];
        }
    });
    for (uint64_t k = 1; k <= 3'000'000; ++k) {
        Wide w;
        for (auto& x : w.v) x = k;
        s.store(w);
    }
    done = true;
    reader.join();
    EXPECT_EQ(torn, 0u);
    EXPECT_EQ(backwards, 0u);
    EXPECT_GT(reads, 0u);
}

TEST(LiveView, RenderShowsLevels) {
    BookSnapshot s;
    std::memcpy(s.symbol, "AAPL    ", 8);
    s.timestamp = (9ull * 3600 + 30 * 60) * 1'000'000'000ull;
    s.messages = 42;
    s.n_bids = 1;
    s.n_asks = 1;
    s.bids[0] = {2930500, 3, 500};
    s.asks[0] = {2930600, 1, 100};
    const std::string out = render_snapshot(s);
    EXPECT_NE(out.find("AAPL"), std::string::npos);
    EXPECT_NE(out.find("09:30:00.000"), std::string::npos);
    EXPECT_NE(out.find("293.0500"), std::string::npos);
    EXPECT_NE(out.find("293.0600"), std::string::npos);
    EXPECT_NE(out.find("spread 0.0100"), std::string::npos);
}
