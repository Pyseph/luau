// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "Luau/ConstraintGraph.h"
#include "Luau/TypeArena.h"

#include "doctest.h"

#include <vector>

using namespace Luau;

namespace
{

std::vector<ConstraintVertex> contents(ConstraintList& list)
{
    std::vector<ConstraintVertex> result;
    for (ConstraintVertex vertex : list)
        result.push_back(vertex);
    return result;
}

} // namespace

TEST_SUITE_BEGIN("ConstraintGraph");

TEST_CASE("constraint_list_keeps_insertion_order_across_removals")
{
    TypeArena arena;

    // One list short enough to be scanned and one long enough to be indexed.
    for (size_t count : {5, 20})
    {
        std::vector<ConstraintVertex> vertices;
        for (size_t i = 0; i < count; ++i)
            vertices.emplace_back(arena.addType(BlockedType{}));

        ConstraintList list;
        for (ConstraintVertex vertex : vertices)
            list.insert(vertex);
        list.insert(vertices[1]);
        CHECK(list.size() == count);
        CHECK(contents(list) == vertices);

        std::vector<ConstraintVertex> odd;
        for (size_t i = 0; i < count; ++i)
        {
            if (i % 2 == 0)
                list.remove(vertices[i]);
            else
                odd.push_back(vertices[i]);
        }
        list.remove(vertices[0]);
        CHECK(list.size() == odd.size());
        CHECK(contents(list) == odd);
        CHECK(!list.contains(vertices[0]));
        CHECK(list.contains(vertices[1]));

        list.insert(vertices[0]);
        odd.insert(odd.begin(), vertices[0]);
        CHECK(list.contains(vertices[0]));
        CHECK(contents(list) == odd);

        list.clear();
        CHECK(list.size() == 0);
        CHECK(!list.contains(vertices[1]));
        CHECK(contents(list).empty());

        list.insert(vertices[count - 1]);
        CHECK(contents(list) == std::vector<ConstraintVertex>{vertices[count - 1]});
    }
}

TEST_SUITE_END();
