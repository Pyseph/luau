// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/TypedAllocator.h"

#include "ScopedFlags.h"

#include "doctest.h"

#include <vector>

LUAU_FASTFLAG(DebugLuauFreezeArena)

using namespace Luau;

namespace
{

struct Counted
{
    static int live;

    explicit Counted(int value)
        : value(value)
    {
        ++live;
    }

    ~Counted()
    {
        --live;
    }

    int value;
    char padding[60];
};

int Counted::live = 0;

// Enough values to fill every growing block and then several full-size ones.
constexpr int kManyValues = 3000;

void allocateAndCheck(TypedAllocator<Counted>& allocator, int count)
{
    std::vector<Counted*> values;
    for (int i = 0; i < count; ++i)
        values.push_back(allocator.allocate(i));

    CHECK(allocator.size() == size_t(count));
    CHECK(Counted::live == count);

    bool intact = true;
    for (int i = 0; i < count; ++i)
        intact &= values[i]->value == i && allocator.contains(values[i]);

    CHECK(intact);
}

} // namespace

TEST_SUITE_BEGIN("TypedAllocator");

TEST_CASE("values_stay_in_the_blocks_that_hold_them")
{
    for (bool growBlocks : {false, true})
    {
        {
            TypedAllocator<Counted> allocator{growBlocks};
            CHECK(allocator.empty());
            CHECK(allocator.size() == 0);

            allocateAndCheck(allocator, kManyValues);

            Counted outside{-1};
            CHECK(!allocator.contains(&outside));
        }

        CHECK(Counted::live == 0);
    }
}

TEST_CASE("cleared_allocator_starts_over")
{
    TypedAllocator<Counted> allocator{/* growBlocks */ true};
    allocateAndCheck(allocator, kManyValues);

    allocator.clear();
    CHECK(allocator.empty());
    CHECK(allocator.size() == 0);
    CHECK(Counted::live == 0);

    allocateAndCheck(allocator, 10);
}

TEST_CASE("growing_blocks_can_be_frozen")
{
    ScopedFastFlag sff{FFlag::DebugLuauFreezeArena, true};

    TypedAllocator<Counted> allocator{/* growBlocks */ true};
    allocateAndCheck(allocator, kManyValues);

    allocator.freeze();
    CHECK(allocator.isFrozen());

    allocator.unfreeze();
    CHECK(!allocator.isFrozen());

    allocator.allocate(kManyValues);
    CHECK(allocator.size() == size_t(kManyValues + 1));
}

TEST_SUITE_END();
