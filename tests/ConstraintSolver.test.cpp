// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "Fixture.h"
#include "Luau/ConstraintGraph.h"
#include "doctest.h"

LUAU_FASTFLAG(LuauCopyDependenciesOnce)

using namespace Luau;

TEST_SUITE_BEGIN("ConstraintSolver");

TEST_CASE_FIXTURE(Fixture, "constraint_basics")
{
    check(R"(
        local a = 55
        local b = a
    )");

    CHECK("number" == toString(requireType("b")));
}

TEST_CASE_FIXTURE(Fixture, "generic_function")
{
    check(R"(
        local function id(a)
            return a
        end
    )");


    CHECK("<T>(T) -> T" == toString(requireType("id")));
}

TEST_CASE_FIXTURE(Fixture, "proper_let_generalization")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();

    check(R"(
        local function a(c)
            local function d(e)
                return c
            end

            return d
        end

        local b = a(5)
    )");

    CHECK("(unknown) -> number" == toString(requireType("b")));
}

TEST_CASE_FIXTURE(Fixture, "table_prop_access_diamond")
{
    CheckResult result = check(R"(
        export type ItemDetails = { Id: number }

        export type AssetDetails = ItemDetails & {}
        export type BundleDetails = ItemDetails & {}

        export type CatalogPage = { AssetDetails | BundleDetails }

        local function isRestricted(item: number) end

        -- Clear all item tiles and create new ones for the items in the specified page
        local function displayPage(catalogPage: CatalogPage)
            for _, itemDetails in catalogPage do
                if isRestricted(itemDetails.Id) then
                    continue
                end
            end
        end
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(Fixture, "copying_dependencies_again_picks_up_ones_the_source_gained")
{
    ScopedFastFlag sff{FFlag::LuauCopyDependenciesOnce, true};

    TypeArena arena;
    Scope scope{getBuiltins()->anyTypePack};
    ConstraintGraph graph{getBuiltins()};

    TypeId source = arena.freshType(getBuiltins(), &scope);
    TypeId target = arena.freshType(getBuiltins(), &scope);
    const Constraint first{NotNull{&scope}, Location{}, EqualityConstraint{source, target}};
    const Constraint second{NotNull{&scope}, Location{}, EqualityConstraint{source, target}};

    graph.addDependencyOf(&first, source);
    graph.copyDependenciesOf(source, target);

    // A new dependency of `source` has to reach `target` ...
    graph.addDependencyOf(&second, source);
    graph.copyDependenciesOf(source, target);
    graph.unblockConstraint(NotNull{&first});
    CHECK(graph.hasUnsolvedDependencies(target));

    // ... and so does one that was removed and then added back.
    graph.addDependencyOf(&first, source);
    graph.copyDependenciesOf(source, target);
    graph.unblockConstraint(NotNull{&second});
    CHECK(graph.hasUnsolvedDependencies(target));
}

TEST_SUITE_END();
