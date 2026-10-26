#include <gtest/gtest.h>

#include <random>
#include <set>

#include "lob/bitmap.hpp"

namespace lob {
namespace {

TEST(LevelBitmap, EmptySearches) {
    LevelBitmap bits(10'000);
    EXPECT_EQ(bits.next_up(0), LevelBitmap::npos);
    EXPECT_EQ(bits.next_up(9'999), LevelBitmap::npos);
    EXPECT_EQ(bits.next_down(0), LevelBitmap::npos);
    EXPECT_EQ(bits.next_down(9'999), LevelBitmap::npos);
}

TEST(LevelBitmap, WordAndSummaryBoundaries) {
    LevelBitmap bits(1 << 16);
    for (std::size_t i : {0u, 63u, 64u, 4095u, 4096u, 65535u}) {
        bits.set(i);
        EXPECT_TRUE(bits.test(i));
        EXPECT_EQ(bits.next_up(i), i);
        EXPECT_EQ(bits.next_down(i), i);
        EXPECT_EQ(bits.next_up(0), i);
        EXPECT_EQ(bits.next_down(65535), i);
        bits.clear(i);
        EXPECT_EQ(bits.next_up(0), LevelBitmap::npos);
    }
    bits.set(5);
    bits.set(65535);
    EXPECT_EQ(bits.next_up(6), 65535u);
    EXPECT_EQ(bits.next_down(65534), 5u);
}

TEST(LevelBitmap, MatchesStdSetUnderRandomOps) {
    constexpr std::size_t kBits = 70'000;  // not a multiple of 64 or 4096
    LevelBitmap bits(kBits);
    std::set<std::size_t> model;
    std::mt19937_64 rng(7);
    for (int step = 0; step < 200'000; ++step) {
        // Cluster most bits so searches cross words but also sometimes go far.
        const std::size_t i = rng() % 8 ? 30'000 + rng() % 2'000 : rng() % kBits;
        if (rng() % 2) {
            bits.set(i);
            model.insert(i);
        } else {
            bits.clear(i);
            model.erase(i);
        }
        const std::size_t q = rng() % kBits;
        auto up = model.lower_bound(q);
        EXPECT_EQ(bits.next_up(q), up == model.end() ? LevelBitmap::npos : *up);
        auto down = model.upper_bound(q);
        EXPECT_EQ(bits.next_down(q), down == model.begin() ? LevelBitmap::npos : *std::prev(down));
    }
    bits.reset();
    EXPECT_EQ(bits.next_up(0), LevelBitmap::npos);
}

}  // namespace
}  // namespace lob
