// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/ModuleInterface.h"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Frontend.h"
#include "Luau/ToString.h"

#include "Fixture.h"

#include "doctest.h"

#include <algorithm>
#include <map>
#include <set>

LUAU_FASTFLAG(DebugLuauForceOldSolver)
LUAU_FASTFLAG(LuauPrefetchSourcesInParallel)
LUAU_FASTFLAG(LuauDeferSyntaxOfCachedModules)

using namespace Luau;

namespace
{

struct InterfaceFixture : BuiltinsFixture
{
    std::unique_ptr<GlobalTypeRegistry> registry;

    GlobalTypeRegistry& globals()
    {
        if (!registry)
            registry = std::make_unique<GlobalTypeRegistry>(getFrontend().builtinTypes, getFrontend().globals);
        return *registry;
    }

    InterfaceSerializeContext writeContext()
    {
        return InterfaceSerializeContext{getFrontend().builtinTypes, NotNull{&globals()}};
    }

    InterfaceDeserializeContext readContext()
    {
        return InterfaceDeserializeContext{getFrontend().builtinTypes, NotNull{&globals()}, nullptr, NotNull{&getFrontend().moduleResolver}};
    }

    static std::string describe(const CheckResult& result)
    {
        std::vector<std::string> lines;
        for (const TypeError& error : result.errors)
            lines.push_back(error.moduleName + ":" + toString(error.location) + ": " + toString(error));
        std::sort(lines.begin(), lines.end());
        std::string out;
        for (const std::string& line : lines)
            out += line + "\n";
        return out;
    }

    // Checks `consumer` normally, then replaces every module in `dependencies` (topologically ordered) by its
    // round-tripped interface and checks `consumer` again: both checks must report the same errors.
    void roundTrip(const std::vector<ModuleName>& dependencies, const ModuleName& consumer)
    {
        Frontend& frontend = getFrontend();
        CheckResult before = frontend.check(consumer);
        std::string expected = describe(before);

        std::vector<std::string> blobs;
        std::vector<InterfaceHash> hashes;
        for (const ModuleName& name : dependencies)
        {
            ModulePtr module = frontend.moduleResolver.getModule(name);
            REQUIRE(module);
            SerializedInterface written = serializeModuleInterface(*module, writeContext());
            INFO(name);
            REQUIRE_MESSAGE(written.failure.empty(), written.failure);
            blobs.push_back(written.bytes);
            hashes.push_back(written.hash);
        }

        for (size_t i = 0; i < dependencies.size(); i++)
        {
            SourceModule* source = frontend.getSourceModule(dependencies[i]);
            REQUIRE(source);
            std::string failure;
            ModulePtr loaded = deserializeModuleInterface(blobs[i], *source, readContext(), &failure);
            INFO(dependencies[i]);
            REQUIRE_MESSAGE(loaded, failure);
            CHECK(loaded->interfaceIndex->hash == hashes[i]);

            SerializedInterface again = serializeModuleInterface(*loaded, writeContext());
            CHECK(again.failure.empty());
            CHECK(again.bytes == blobs[i]);
            CHECK(again.hash == hashes[i]);

            frontend.moduleResolver.setModule(dependencies[i], loaded);
        }

        frontend.markDirty(consumer);
        CheckResult after = frontend.check(consumer);
        CHECK_EQ(expected, describe(after));
        // Every consumer below makes mistakes on purpose, so an identical answer is not two empty ones.
        CHECK(!after.errors.empty());
    }
};

} // namespace

TEST_SUITE_BEGIN("ModuleInterface");

TEST_CASE_FIXTURE(InterfaceFixture, "tables_functions_and_generics_survive_a_round_trip")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    fileResolver.source["game/A"] = R"(
        --!strict
        export type Point = { x: number, y: number }
        export type Box<T> = { value: T, map: <U>(self: Box<T>, (T) -> U) -> Box<U> }
        export type Tree = { left: Tree?, right: Tree?, value: string }
        export type Pack<T...> = (T...) -> (T...)

        local M = {}
        M.origin = { x = 0, y = 0 } :: Point
        M.kind = "a" :: "a" | "b"

        function M.add(a: Point, b: Point): Point
            return { x = a.x + b.x, y = a.y + b.y }
        end

        function M.first<T>(list: { T }): T?
            return list[1]
        end

        function M.many(...: number): ...string
            return tostring((...))
        end

        M.over = (nil :: any) :: ((number) -> string) & ((string) -> number)
        M.lookup = {} :: { [string]: boolean }
        M.readonly = {} :: { read name: string }

        return M
    )";

    fileResolver.source["game/B"] = R"(
        --!strict
        local A = require(game.A)
        type Point = A.Point

        local p: Point = A.add(A.origin, { x = 1, y = 2 })
        local q: Point = A.add(p, { x = "nope", y = 2 })
        local n: number = A.first({ "s" })
        local s: string = A.over(1)
        local t: boolean = A.lookup.k
        local k: "a" = A.kind
        local tree: A.Tree = { value = 1 }
        local box: A.Box<number> = (nil :: any)
        local r: string = box.value
        local m = A.many(1, 2, 3)
        A.readonly.name = "x"
    )";

    roundTrip({"game/A"}, "game/B");
}

TEST_CASE_FIXTURE(InterfaceFixture, "references_into_other_modules_resolve_through_their_interfaces")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    fileResolver.source["game/A"] = R"(
        --!strict
        export type Item = { id: number, tags: { string } }
        local A = {}
        function A.make(id: number): Item
            return { id = id, tags = {} }
        end
        return A
    )";

    fileResolver.source["game/B"] = R"(
        --!strict
        local A = require(game.A)
        export type Item = A.Item
        export type Bag = { items: { A.Item }, owner: string }
        local B = {}
        B.make = A.make
        function B.bag(owner: string): Bag
            return { items = { A.make(1) }, owner = owner }
        end
        return B
    )";

    fileResolver.source["game/C"] = R"(
        --!strict
        local B = require(game.B)
        local bag = B.bag("me")
        local id: string = bag.items[1].id
        local item: B.Item = B.make(true)
    )";

    roundTrip({"game/A", "game/B"}, "game/C");
}

TEST_CASE_FIXTURE(InterfaceFixture, "classes_built_from_metatables_survive_a_round_trip")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    fileResolver.source["game/A"] = R"(
        --!strict
        local Account = {}
        Account.__index = Account

        export type Account = typeof(setmetatable({} :: { balance: number }, Account))

        function Account.new(balance: number): Account
            return setmetatable({ balance = balance }, Account)
        end

        function Account.deposit(self: Account, amount: number)
            self.balance += amount
        end

        return Account
    )";

    fileResolver.source["game/B"] = R"(
        --!strict
        local Account = require(game.A)
        local a = Account.new(10)
        a:deposit("ten")
        local b: string = a.balance
    )";

    roundTrip({"game/A"}, "game/B");
}

TEST_CASE_FIXTURE(InterfaceFixture, "user_defined_type_functions_are_rebound_to_their_definitions")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    fileResolver.source["game/A"] = R"(
        --!strict
        type function Optionalize(t)
            local result = types.newtable()
            for key, value in t:properties() do
                result:setproperty(key, types.unionof(value.read, types.singleton(nil)))
            end
            return result
        end

        type Create = <T>(T) -> Optionalize<T>
        local create = (nil :: any) :: Create
        return create
    )";

    fileResolver.source["game/B"] = R"(
        --!strict
        local create = require(game.A)
        local v = create({ a = 1, b = "s" })
        local n: number = v.a
    )";

    roundTrip({"game/A"}, "game/B");
}

TEST_CASE_FIXTURE(InterfaceFixture, "the_hash_ignores_where_code_moved")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    auto hashOf = [&](const std::string& source)
    {
        fileResolver.source["game/A"] = source;
        getFrontend().markDirty("game/A");
        getFrontend().check("game/A");
        ModulePtr module = getFrontend().moduleResolver.getModule("game/A");
        REQUIRE(module);
        SerializedInterface written = serializeModuleInterface(*module, writeContext());
        REQUIRE(written.failure.empty());
        return written.hash;
    };

    InterfaceHash first = hashOf(R"(
        --!strict
        local M = {}
        function M.f(x: number): string return tostring(x) end
        return M
    )");

    InterfaceHash moved = hashOf(R"(
        --!strict


        local M = {}
        -- a comment that moves everything down
        function M.f(x: number): string
            return tostring(x)
        end
        return M
    )");

    InterfaceHash changed = hashOf(R"(
        --!strict
        local M = {}
        function M.f(x: number): number return x end
        return M
    )");

    CHECK(first == moved);
    CHECK(first != changed);
}

TEST_CASE_FIXTURE(InterfaceFixture, "a_missing_dependency_fails_the_load")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    fileResolver.source["game/A"] = R"(
        --!strict
        export type T = { x: number }
        return { make = function(): T return { x = 1 } end }
    )";
    fileResolver.source["game/B"] = R"(
        --!strict
        local A = require(game.A)
        return { make = A.make }
    )";

    getFrontend().check("game/B");
    ModulePtr a = getFrontend().moduleResolver.getModule("game/A");
    ModulePtr b = getFrontend().moduleResolver.getModule("game/B");
    REQUIRE(serializeModuleInterface(*a, writeContext()).failure.empty());
    SerializedInterface written = serializeModuleInterface(*b, writeContext());
    REQUIRE(written.failure.empty());
    CHECK(written.referencedModules == std::vector<std::string>{"game/A"});

    getFrontend().moduleResolver.eraseModule("game/A");
    std::string failure;
    ModulePtr loaded = deserializeModuleInterface(written.bytes, *getFrontend().getSourceModule("game/B"), readContext(), &failure);
    CHECK(!loaded);
    CHECK(!failure.empty());
}

namespace
{

struct MapCache : ModuleCheckCache
{
    InterfaceFixture& fixture;
    std::map<ModuleName, std::string> blobs;
    std::vector<ModuleName> loaded;
    std::vector<ModuleName> checkedModules;

    explicit MapCache(InterfaceFixture& fixture)
        : fixture(fixture)
    {
    }

    ModulePtr load(const SourceNode&, const SourceModule& source, Mode, const std::vector<RequireCycle>&) override
    {
        auto it = blobs.find(source.name);
        if (it == blobs.end())
            return nullptr;
        std::string failure;
        ModulePtr module = deserializeModuleInterface(it->second, source, fixture.readContext(), &failure);
        if (module)
            loaded.push_back(source.name);
        return module;
    }

    void checked(const SourceNode&, const SourceModule& source, const ModulePtr& module, const std::vector<RequireCycle>&, ExternTypeObserver*)
        override
    {
        checkedModules.push_back(source.name);
        SerializedInterface written = serializeModuleInterface(*module, fixture.writeContext());
        if (written.failure.empty())
            blobs[source.name] = written.bytes;
    }
};

} // namespace

TEST_CASE_FIXTURE(InterfaceFixture, "a_frontend_rebuilds_a_closure_from_the_module_cache")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    fileResolver.source["game/A"] = R"(
        --!strict
        export type T = { x: number }
        return { make = function(): T return { x = 1 } end }
    )";
    fileResolver.source["game/B"] = R"(
        --!strict
        local A = require(game.A)
        return { make = A.make, twice = function(t: A.T): number return t.x * 2 end }
    )";
    fileResolver.source["game/C"] = R"(
        --!strict
        local B = require(game.B)
        local s: string = B.twice(B.make())
    )";

    auto cache = std::make_shared<MapCache>(*this);
    FrontendOptions options;
    options.retainFullTypeGraphs = false;
    options.moduleCache = cache;

    CheckResult cold = getFrontend().check("game/C", options);
    CHECK(cache->checkedModules == std::vector<ModuleName>{"game/A", "game/B", "game/C"});
    CHECK(cache->loaded.empty());

    getFrontend().clear();
    CheckResult warm = getFrontend().check("game/C", options);
    CHECK(cache->loaded == std::vector<ModuleName>{"game/A", "game/B", "game/C"});
    // A loaded module carries no diagnostics of its own: they belong to whoever stored it.
    CHECK(warm.errors.empty());

    // A loaded module gives its syntax tree back; checking it again reads and parses it afresh.
    REQUIRE(getFrontend().getSourceModule("game/A"));
    CHECK(getFrontend().getSourceModule("game/A")->root == nullptr);
    getFrontend().markDirty("game/A");
    cache->blobs.erase("game/A");
    cache->blobs.erase("game/C");
    CheckResult reparsed = getFrontend().check("game/C", options);
    CHECK(getFrontend().getSourceModule("game/A")->root != nullptr);
    CHECK_EQ(describe(cold), describe(reparsed));

    // The consumer's own diagnostics come from a real check, so ask for it the way an editor would.
    getFrontend().clear();
    cache->blobs.erase("game/C");
    CheckResult rechecked = getFrontend().check("game/C", options);
    CHECK_EQ(describe(cold), describe(rechecked));
    CHECK(!rechecked.errors.empty());
}

namespace
{

struct DeferringCache : MapCache
{
    std::set<ModuleName> expected;
    std::vector<ModuleName> deferred;

    using MapCache::MapCache;

    bool expectsLoad(const SourceModule& source, const SourceCode&) override
    {
        if (!expected.count(source.name))
            return false;
        deferred.push_back(source.name);
        return true;
    }
};

} // namespace

TEST_CASE_FIXTURE(InterfaceFixture, "a_deferred_module_is_parsed_again_when_the_cache_misses")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    ScopedFastFlag sffs[] = {{FFlag::LuauPrefetchSourcesInParallel, true}, {FFlag::LuauDeferSyntaxOfCachedModules, true}};

    fileResolver.source["game/A"] = R"(
        --!strict
        export type T = { x: number }
        return { make = function(): T return { x = 1 } end }
    )";
    fileResolver.source["game/B"] = R"(
        --!strict
        local A = require(game.A)
        local n: string = 1
        return { make = A.make, twice = function(t: A.T): number return t.x * 2 end }
    )";
    fileResolver.source["game/C"] = R"(
        --!strict
        local B = require(game.B)
        local s: string = B.twice(B.make())
    )";

    auto cache = std::make_shared<DeferringCache>(*this);
    FrontendOptions options;
    options.retainFullTypeGraphs = false;
    options.moduleCache = cache;
    auto inline_ = [](std::vector<std::function<void()>> tasks)
    {
        for (std::function<void()>& task : tasks)
            task();
    };

    Frontend& frontend = getFrontend();
    frontend.queueModuleCheck("game/C");
    frontend.checkQueuedModules(options, inline_);
    std::optional<CheckResult> cold = frontend.getCheckResult("game/C", true);
    REQUIRE(cold);
    CHECK(cache->deferred.empty());
    CHECK(cache->checkedModules == std::vector<ModuleName>{"game/A", "game/B", "game/C"});

    // All three look familiar, but only A still has a record: B and C are parsed again and checked.
    frontend.clear();
    cache->expected = {"game/A", "game/B", "game/C"};
    cache->blobs.erase("game/B");
    cache->blobs.erase("game/C");
    cache->checkedModules.clear();
    frontend.queueModuleCheck("game/C");
    frontend.checkQueuedModules(options, inline_);
    std::optional<CheckResult> warm = frontend.getCheckResult("game/C", true);
    REQUIRE(warm);

    CHECK(cache->deferred.size() == 3);
    CHECK(cache->loaded == std::vector<ModuleName>{"game/A"});
    CHECK(cache->checkedModules == std::vector<ModuleName>{"game/B", "game/C"});
    CHECK_EQ(describe(*cold), describe(*warm));
    CHECK(!warm->errors.empty());

    SourceModule* a = frontend.getSourceModule("game/A");
    SourceModule* b = frontend.getSourceModule("game/B");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->root == nullptr);
    CHECK(a->deferredSyntax == nullptr);
    CHECK(a->topLevelReturn == std::optional<bool>{true});
    CHECK(b->root != nullptr);
    CHECK(b->deferredSyntax == nullptr);
}

TEST_CASE_FIXTURE(InterfaceFixture, "a_module_defining_type_functions_keeps_its_tree_for_the_load")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    ScopedFastFlag sffs[] = {{FFlag::LuauPrefetchSourcesInParallel, true}, {FFlag::LuauDeferSyntaxOfCachedModules, true}};

    fileResolver.source["game/F"] = R"(
        --!strict
        type function Wrap(t)
            return types.newtable({ value = t })
        end
        export type Boxed = Wrap<number>
        return {}
    )";

    auto cache = std::make_shared<DeferringCache>(*this);
    FrontendOptions options;
    options.retainFullTypeGraphs = false;
    options.moduleCache = cache;
    auto inline_ = [](std::vector<std::function<void()>> tasks)
    {
        for (std::function<void()>& task : tasks)
            task();
    };

    Frontend& frontend = getFrontend();
    frontend.queueModuleCheck("game/F");
    frontend.checkQueuedModules(options, inline_);
    REQUIRE(cache->blobs.count("game/F"));

    frontend.clear();
    cache->expected = {"game/F"};
    frontend.queueModuleCheck("game/F");
    frontend.checkQueuedModules(options, inline_);

    CHECK(cache->deferred.empty());
    CHECK(cache->loaded == std::vector<ModuleName>{"game/F"});
}

namespace
{

struct RecordingObserver : ExternTypeObserver
{
    std::vector<const void*> whole;
    std::vector<std::pair<const void*, Name>> members;

    void observe(const void* key) override
    {
        whole.push_back(key);
    }

    void observeMember(const void* key, const Name& name) override
    {
        members.emplace_back(key, name);
    }
};

} // namespace

TEST_CASE_FIXTURE(InterfaceFixture, "a_lookup_on_an_observed_extern_type_reports_the_member_it_asked_for")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    loadDefinition(R"(
        declare extern type Folder with
            Size: number
            Label: string
        end
        declare folder: Folder
    )");

    TypeId folder = getGlobalBinding(getFrontend().globals, "folder");
    ExternType* external = getMutable<ExternType>(follow(folder));
    REQUIRE(external);
    static const int key = 0;
    unfreeze(getFrontend().globals.globalTypes);
    external->props.observationKey = &key;
    freeze(getFrontend().globals.globalTypes);

    RecordingObserver observer;
    ExternTypeObserver* previous = setExternTypeObserver(&observer);
    check(R"(
        local size: number = folder.Size
    )");
    const size_t wholeAfterPresent = observer.whole.size();
    check(R"(
        local missing = folder.Missing
    )");
    setExternTypeObserver(previous);
    CHECK_EQ(wholeAfterPresent, 0);
    unfreeze(getFrontend().globals.globalTypes);
    external->props.observationKey = nullptr;
    freeze(getFrontend().globals.globalTypes);

    // Reporting the missing member may read every member (to suggest one), which only widens what is observed.
    auto asked = [&](const char* name)
    {
        return std::find(observer.members.begin(), observer.members.end(), std::pair<const void*, Name>{&key, name}) != observer.members.end();
    };
    CHECK(asked("Size"));
    CHECK(asked("Missing"));
    CHECK(!asked("Label"));
}

TEST_CASE_FIXTURE(InterfaceFixture, "the_global_registry_is_deterministic")
{
    if (FFlag::DebugLuauForceOldSolver)
        return;

    GlobalTypeRegistry first{getFrontend().builtinTypes, getFrontend().globals};
    GlobalTypeRegistry second{getFrontend().builtinTypes, getFrontend().globals};
    CHECK(first.digest == second.digest);
    CHECK(first.typeCount() == second.typeCount());
    CHECK(first.typeCount() > 100);
}

TEST_SUITE_END();
