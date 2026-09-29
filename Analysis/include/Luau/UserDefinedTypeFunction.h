// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/DenseHash.h"
#include "Luau/TypeArena.h"
#include "Luau/TypeFunction.h"
#include "Luau/TypeFwd.h"

#include <mutex>
#include <vector>

namespace Luau
{

TypeFunctionReductionResult<TypeId> userDefinedTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
);

// Results of the user-defined type functions a module defines, for arguments that last as long as the module does.
// Every module that evaluates those functions reuses them, so they live in an arena of their own and are only accessed
// under the mutex.
struct UserDefinedTypeFunctionResults
{
    struct Call
    {
        const AstStatTypeFunction* definition = nullptr;
        std::vector<TypeId> arguments;

        bool operator==(const Call& rhs) const;
    };

    struct HashCall
    {
        size_t operator()(const Call& call) const;
    };

    std::mutex mutex;
    TypeArena arena;
    DenseHashMap<Call, TypeId, HashCall> results;
};

}