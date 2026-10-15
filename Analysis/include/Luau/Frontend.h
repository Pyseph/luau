// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Config.h"
#include "Luau/ConfigResolver.h"
#include "Luau/DenseHash.h"
#include "Luau/GlobalTypes.h"
#include "Luau/Module.h"
#include "Luau/ModuleResolver.h"
#include "Luau/RequireTracer.h"
#include "Luau/Scope.h"
#include "Luau/TypeCheckLimits.h"

#include <mutex>
#include <string>
#include <vector>
#include <optional>

namespace Luau
{

class AstStat;
class ParseError;
struct Frontend;
struct TypeError;
struct LintWarning;
struct GlobalTypes;
struct TypeChecker;
struct FileResolver;
struct ModuleResolver;
struct ParseResult;
struct HotComment;
struct BuildQueueItem;
struct BuildQueueModuleInfo;
struct BuildQueueWorkState;
struct FrontendCancellationToken;

struct LoadDefinitionFileResult
{
    bool success;
    ParseResult parseResult;
    SourceModule sourceModule;
    ModulePtr module;
};

std::optional<Mode> parseMode(const std::vector<HotComment>& hotcomments);

struct ModuleSCC
{
    std::vector<ModuleName> members;
    std::shared_ptr<TypeArena> sharedArena;
};
using ModuleSCCPtr = std::shared_ptr<ModuleSCC>;

struct SourceNode
{
    bool hasDirtySourceModule() const
    {
        return dirtySourceModule;
    }

    bool hasDirtyModule(bool forAutocomplete) const
    {
        return forAutocomplete ? dirtyModuleForAutocomplete : dirtyModule;
    }

    bool hasInvalidModuleDependency(bool forAutocomplete) const
    {
        return forAutocomplete ? invalidModuleDependencyForAutocomplete : invalidModuleDependency;
    }

    void setInvalidModuleDependency(bool value, bool forAutocomplete)
    {
        if (forAutocomplete)
            invalidModuleDependencyForAutocomplete = value;
        else
            invalidModuleDependency = value;
    }

    ModuleName name;
    std::string humanReadableName;
    std::weak_ptr<ModuleSCC> scc;
    DenseHashSet<ModuleName> requireSet;

    std::vector<std::pair<ModuleName, Location>> requireLocations;
    DenseHashSet<ModuleName> dependents;

    bool dirtySourceModule = true;
    bool dirtyModule = true;
    bool dirtyModuleForAutocomplete = true;

    bool invalidModuleDependency = true;
    bool invalidModuleDependencyForAutocomplete = true;

    double autocompleteLimitsMult = 1.0;
};

// Lets an embedder reuse modules checked earlier, in another process even, instead of checking them again. Every
// method runs on the thread that checks the module and may run on several threads at once.
struct ModuleCheckCache
{
    virtual ~ModuleCheckCache() = default;

    // Before an acyclic module checked without full type graphs is checked. A module returned here is recorded as the
    // result of the check, which does not run. `requireCycles` are the cycles through this module's requires, whose
    // targets the check treats as `any`: they depend on other modules' requires, so a result depends on them too.
    virtual ModulePtr load(const SourceNode& node, const SourceModule& source, Mode mode, const std::vector<RequireCycle>& requireCycles) = 0;

    // From any thread, as soon as `source` is parsed and its requires traced: whether `load` is likely to answer for it,
    // in which case its syntax tree is given back until a check needs it. A wrong guess costs a second parse.
    virtual bool expectsLoad(const SourceModule& source, const SourceCode& code)
    {
        return false;
    }

    // Installed on the checking thread for as long as `source` is checked and linted; may be null.
    virtual ExternTypeObserver* observerFor(const SourceModule& source)
    {
        return nullptr;
    }

    // After `source` was checked, linted and, without full type graphs, stripped of its internal types.
    virtual void checked(
        const SourceNode& node,
        const SourceModule& source,
        const ModulePtr& module,
        const std::vector<RequireCycle>& requireCycles,
        ExternTypeObserver* observer
    ) = 0;

    // Instead of `checked`, when checking `source` threw.
    virtual void abandoned(ExternTypeObserver* observer) {}
};

struct FrontendOptions
{
    // When true, we retain full type information about every term in the AST.
    // Setting this to false cuts back on RAM and is a good idea for batch
    // jobs where the type graph is not deeply inspected after typechecking
    // is complete.
    bool retainFullTypeGraphs = false;

    // Run typechecking only in mode required for autocomplete (strict mode in
    // order to get more precise type information)
    bool forAutocomplete = false;

    bool runLintChecks = false;

    // If not empty, randomly shuffle the constraint set before attempting to
    // solve.  Use this value to seed the random number generator.
    std::optional<unsigned> randomizeConstraintResolutionSeed;

    std::optional<LintOptions> enabledLintWarnings;

    std::shared_ptr<FrontendCancellationToken> cancellationToken;

    // Time limit for typechecking a single module
    std::optional<double> moduleTimeLimitSec;

    // When true, some internal complexity limits will be scaled down for modules that miss the limit set by moduleTimeLimitSec
    bool applyInternalLimitScaling = false;

    // An optional callback which is called for every *dirty* module was checked
    // If multi-threaded typechecking is used, this callback might be called
    // from multiple threads and has to be thread-safe
    std::function<void(const SourceModule& sourceModule, const Luau::Module& module)> customModuleCheck;

    bool collectTypeAllocationStats = false;

    // Consulted for every module this check builds; see ModuleCheckCache.
    std::shared_ptr<ModuleCheckCache> moduleCache;

    // Without full type graphs, a checked module's syntax tree is given back once the check is done and its source
    // marked dirty, so that a later check of the module reads and parses it afresh. A module whose own user-defined
    // type functions its interface refers to keeps its tree. With a parallel build, every module parsed for the check
    // also keeps only its text until its turn comes, so that a closure's trees are never all alive at once.
    bool releaseSyntaxAfterCheck = false;

    // Called on the checking thread once a module without full type graphs is checked, linted and given its structural
    // errors, while the types its errors mention are still alive. Returning true says the embedder kept what it needs
    // of them: the module's errors are then dropped rather than copied, with every type they mention, into its
    // interface arena.
    std::function<bool(const SourceModule&, const ModulePtr&)> renderErrors;
};

struct CheckResult
{
    std::vector<TypeError> errors;

    LintResult lintResult;

    std::vector<ModuleName> timeoutHits;
};

struct FrontendModuleResolver : ModuleResolver
{
    FrontendModuleResolver(Frontend* frontend);

    const ModulePtr getModule(const ModuleName& moduleName) const override;
    bool moduleExists(const ModuleName& moduleName) const override;
    std::optional<ModuleInfo> resolveModuleInfo(const ModuleName& currentModuleName, const AstExpr& pathExpr) override;
    std::string getHumanReadableModuleName(const ModuleName& moduleName) const override;

    bool setModule(const ModuleName& moduleName, ModulePtr module);
    void eraseModule(const ModuleName& moduleName);
    void clearModules();


private:
    Frontend* frontend;

    mutable std::mutex moduleMutex;
    std::unordered_map<ModuleName, ModulePtr> modules;
};

struct Frontend
{
    struct Stats
    {
        size_t files = 0;
        size_t lines = 0;

        size_t filesStrict = 0;
        size_t filesNonstrict = 0;

        size_t typesAllocated = 0;
        size_t typePacksAllocated = 0;

        size_t boolSingletonsMinted = 0;
        size_t strSingletonsMinted = 0;
        size_t uniqueStrSingletonsMinted = 0;

        double timeRead = 0;
        double timeParse = 0;
        double timeCheck = 0;
        double timeLint = 0;

        size_t dynamicConstraintsCreated = 0;
    };
    Frontend(SolverMode mode, FileResolver* fileResolver, ConfigResolver* configResolver, FrontendOptions options = {});
    Frontend(FileResolver* fileResolver, ConfigResolver* configResolver, const FrontendOptions& options = {});

    void setLuauSolverMode(SolverMode mode);
    SolverMode getLuauSolverMode() const;
    // The default value assuming there is no workspace setup yet
    std::atomic<SolverMode> useNewLuauSolver;
    // Parse module graph and prepare SourceNode/SourceModule data, including required dependencies without running typechecking
    void parse(const ModuleName& name);
    void parseModules(const std::vector<ModuleName>& name);

    // Parse and typecheck module graph
    CheckResult check(const ModuleName& name, std::optional<FrontendOptions> optionOverride = {}); // new shininess

    bool allModuleDependenciesValid(const ModuleName& name, bool forAutocomplete = false) const;

    bool isDirty(const ModuleName& name, bool forAutocomplete = false) const;
    void markDirty(const ModuleName& name, std::vector<ModuleName>* markedDirty = nullptr);

    void traverseDependents(const ModuleName& name, std::function<bool(SourceNode&)> processSubtree);

    /** Borrow a pointer into the SourceModule cache.
     *
     * Returns nullptr if we don't have it.  This could mean that the script
     * doesn't exist, or simply that its contents have changed since the previous
     * check, in which case we do not have its AST.
     *
     * IMPORTANT: this pointer is only valid until the next call to markDirty.  Do not retain it.
     */
    SourceModule* getSourceModule(const ModuleName& name);
    const SourceModule* getSourceModule(const ModuleName& name) const;

    void clearStats();
    void clear();
    void clearModules(const std::vector<ModuleName>& names);
    void clearBuiltinEnvironments();

    ScopePtr addEnvironment(const std::string& environmentName);
    ScopePtr getEnvironmentScope(const std::string& environmentName) const;

    void registerBuiltinDefinition(const std::string& name, std::function<void(Frontend&, GlobalTypes&, ScopePtr)>);
    void applyBuiltinDefinitionToEnvironment(const std::string& environmentName, const std::string& definitionName);

    LoadDefinitionFileResult loadDefinitionFile(
        GlobalTypes& globals,
        ScopePtr targetScope,
        std::string_view source,
        const std::string& packageName,
        bool captureComments,
        bool typeCheckForAutocomplete = false
    );

    // Copies a successfully loaded definitions file into another set of globals, as loading it there would: the check
    // does not depend on the globals it lands in, so the second set needs the copy and not a second check.
    void persistDefinitionFile(const LoadDefinitionFileResult& loaded, GlobalTypes& globals, ScopePtr targetScope, const std::string& packageName);

    // Batch module checking. Queue modules and check them together, retrieve results with 'getCheckResult'
    // If provided, 'executeTasks' function is allowed to call any item in 'tasks' on any thread and return without waiting for them to complete
    void queueModuleCheck(const std::vector<ModuleName>& names);
    void queueModuleCheck(const ModuleName& name);
    std::vector<ModuleName> checkQueuedModules(
        std::optional<FrontendOptions> optionOverride = {},
        std::function<void(std::vector<std::function<void()>> tasks)> executeTasks = {},
        std::function<bool(size_t done, size_t total)> progress = {}
    );

    std::optional<CheckResult> getCheckResult(const ModuleName& name, bool accumulateNested, bool forAutocomplete = false);
    std::vector<ModuleName> getRequiredScripts(const ModuleName& name, const TypeCheckLimits& limits);

    TypeId parseType(
        NotNull<Allocator> allocator,
        NotNull<AstNameTable> nameTable,
        NotNull<InternalErrorReporter> iceHandler,
        TypeCheckLimits limits,
        NotNull<TypeArena> arena,
        std::string_view source
    );

private:
    ModulePtr check(
        const SourceModule& sourceModule,
        Mode mode,
        std::vector<RequireCycle> requireCycles,
        std::optional<ScopePtr> environmentScope,
        bool forAutocomplete,
        bool recordJsonLog,
        Frontend::Stats& stats,
        TypeCheckLimits typeCheckLimits
    );

    std::pair<SourceNode*, SourceModule*> getSourceNode(const ModuleName& name, const TypeCheckLimits& limits);
    SourceModule parse(const ModuleName& name, std::string_view src, const ParseOptions& parseOptions);

    // A module read, parsed and traced but not yet recorded: the half of getSourceNode any thread may do, given resolvers
    // that answer reads from any thread.
    struct PreparedSource
    {
        ModuleName name;
        std::optional<SourceCode> source;
        std::optional<std::string> environmentName;
        SourceModule module;
        RequireTraceResult trace;
        double timeRead = 0;
        double timeParse = 0;
        size_t lines = 0;
    };

    // With `deferTo`, a module it expects to load keeps its text instead of its syntax tree (SourceModule::deferredSyntax);
    // with `deferAll`, every module does, and is parsed again right before it is checked.
    PreparedSource prepareSource(
        const ModuleName& name,
        const ParseOptions& parseOptions,
        const TypeCheckLimits& limits,
        ModuleCheckCache* deferTo = nullptr,
        bool deferAll = false
    );
    std::pair<SourceNode*, SourceModule*> installSource(PreparedSource&& prepared);

    // Parses a module whose syntax was deferred and traces its requires again; their list is unchanged, being the same
    // text's. Safe on a checking thread: it writes only this module's source and its existing require trace entry.
    void restoreSyntax(SourceModule& sourceModule, const TypeCheckLimits& limits);

    // Gives a checked or loaded module's syntax tree back until it is checked again (see releaseSyntaxAfterCheck). Safe
    // on a checking thread, for the same reason as restoreSyntax.
    void releaseUntilChecked(SourceNode& sourceNode, SourceModule& sourceModule);

    // Reads, parses and traces every module parseGraph would reach from `roots` and have to (re)parse, a frontier at a
    // time on `executeTasks`, so that the walk itself finds them all parsed.
    void prefetchSources(
        const std::vector<ModuleName>& roots,
        const TypeCheckLimits& limits,
        bool forAutocomplete,
        const std::function<void(std::vector<std::function<void()>> tasks)>& executeTasks,
        ModuleCheckCache* deferTo = nullptr,
        bool deferAll = false
    );

    bool parseGraph(
        std::vector<ModuleName>& buildQueue,
        const ModuleName& root,
        const TypeCheckLimits& limits,
        bool forAutocomplete,
        std::function<bool(const ModuleName&)> canSkip = {}
    );

    void addBuildQueueItems(
        std::vector<BuildQueueItem>& items,
        std::vector<ModuleName>& buildQueue,
        bool cycleDetected,
        DenseHashSet<Luau::ModuleName>& seen,
        const FrontendOptions& frontendOptions
    );
    void computeSCCs(const std::vector<ModuleName>& buildQueue);
    void checkSCCBuildQueueItem(BuildQueueItem& item);
    void checkBuildQueueItem(BuildQueueItem& item);
    void addStructuralErrors(Module& module, const BuildQueueModuleInfo& moduleInfo, Mode mode);
    void checkBuildQueueItems(std::vector<BuildQueueItem>& items);
    void recordItemResult(const BuildQueueItem& item);
    void performQueueItemTask(std::shared_ptr<BuildQueueWorkState> state, size_t itemPos);
    void sendQueueItemTasks(std::shared_ptr<BuildQueueWorkState> state, const std::vector<size_t>& items);
    void sendQueueCycleItemTask(std::shared_ptr<BuildQueueWorkState> state);

    static LintResult classifyLints(const std::vector<LintWarning>& warnings, const Config& config);

    ScopePtr getModuleEnvironment(const SourceModule& module, const Config& config, bool forAutocomplete) const;
    std::unordered_map<std::string, ScopePtr> environments;
    std::unordered_map<std::string, std::function<void(Frontend&, GlobalTypes&, ScopePtr)>> builtinDefinitions;

    BuiltinTypes builtinTypes_;

public:
    const NotNull<BuiltinTypes> builtinTypes;

    FileResolver* fileResolver;

    FrontendModuleResolver moduleResolver;
    FrontendModuleResolver moduleResolverForAutocomplete;

    GlobalTypes globals;
    GlobalTypes globalsForAutocomplete;

    ConfigResolver* configResolver;
    FrontendOptions options;
    InternalErrorReporter iceHandler;
    std::function<void(const ModuleName& name, const ScopePtr& scope, bool forAutocomplete)> prepareModuleScope;
    std::function<void(const ModuleName& name, std::string log)> writeJsonLog = {};

    std::unordered_map<ModuleName, std::shared_ptr<SourceNode>> sourceNodes;
    std::unordered_map<ModuleName, std::shared_ptr<SourceModule>> sourceModules;
    std::unordered_map<ModuleName, RequireTraceResult> requireTrace;
    DenseHashMap<ModuleName, ModuleSCCPtr> sccs;

    Stats stats = {};

    std::vector<ModuleName> moduleQueue;
};

ModulePtr check(
    const SourceModule& sourceModule,
    Mode mode,
    const std::vector<RequireCycle>& requireCycles,
    NotNull<BuiltinTypes> builtinTypes,
    NotNull<InternalErrorReporter> iceHandler,
    NotNull<ModuleResolver> moduleResolver,
    NotNull<FileResolver> fileResolver,
    const ScopePtr& parentScope,
    const ScopePtr& typeFunctionScope,
    std::function<void(const ModuleName&, const ScopePtr&)> prepareModuleScope,
    FrontendOptions options,
    TypeCheckLimits limits,
    bool recordJsonLog,
    Frontend::Stats& stats,
    std::function<void(const ModuleName&, std::string)> writeJsonLog
);

} // namespace Luau
