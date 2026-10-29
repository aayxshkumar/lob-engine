#include "lob/id_table.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

using namespace lob;

TEST(IdTable, InsertFindErase) {
    IdTable t(8);
    EXPECT_EQ(t.find(42), IdTable::kEmpty);
    t.insert(42, 7);
    EXPECT_EQ(t.find(42), 7u);
    EXPECT_EQ(t.size(), 1u);
    EXPECT_TRUE(t.erase(42));
    EXPECT_FALSE(t.erase(42));
    EXPECT_FALSE(t.contains(42));
    EXPECT_EQ(t.size(), 0u);
}

TEST(IdTable, ZeroAndMaxAreValidKeys) {
    IdTable t(4);
    t.insert(0, 1);
    t.insert(~OrderId{0}, 2);
    EXPECT_EQ(t.find(0), 1u);
    EXPECT_EQ(t.find(~OrderId{0}), 2u);
}

TEST(IdTable, RejectsInsertPastCapacity) {
    IdTable t(2);
    t.insert(1, 0);
    t.insert(2, 0);
    EXPECT_THROW(t.insert(3, 0), std::length_error);
}

// Differential test against std::unordered_map. The table is tiny (16
// entries, up to 8 live) so probe runs are long and wrap around the end of
// the array, which is where backward-shift deletion bugs hide.
TEST(IdTable, MatchesUnorderedMapUnderChurn) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        std::mt19937_64 rng(seed);
        IdTable t(8);
        std::unordered_map<OrderId, std::uint32_t> ref;
        for (int step = 0; step < 20'000; ++step) {
            const OrderId id = rng() % 64;
            const bool present = ref.contains(id);
            if (!present && ref.size() < 8 && (rng() & 1)) {
                const auto v = static_cast<std::uint32_t>(rng() % 1000);
                t.insert(id, v);
                ref.emplace(id, v);
            } else if (present) {
                ASSERT_TRUE(t.erase(id)) << "seed " << seed << " step " << step;
                ref.erase(id);
            }
            ASSERT_EQ(t.size(), ref.size());
            for (OrderId k = 0; k < 64; ++k) {
                auto it = ref.find(k);
                ASSERT_EQ(t.find(k), it == ref.end() ? IdTable::kEmpty : it->second)
                    << "seed " << seed << " step " << step << " key " << k;
            }
        }
    }
}
