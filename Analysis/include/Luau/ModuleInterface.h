// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/DenseHash.h"
#include "Luau/NotNull.h"
#include "Luau/TypeFwd.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Luau
{

struct BuiltinTypes;
struct GlobalTypes;
struct MagicFunction;
struct Module;
struct ModuleResolver;
struct SourceModule;
using ModulePtr = std::shared_ptr<Module>;

// A 128-bit digest of a module's public interface: its return pack, exported type bindings, declared globals and
// type function aliases, the shape of every type they reach and the names of everything outside the module they
// refer to. Source locations are left out, so an edit that only moves code does not change it.
struct InterfaceHash
{
    uint64_t low = 0;
    uint64_t high = 0;

    bool operator==(const InterfaceHash& rhs) const
    {
        return low == rhs.low && high == rhs.high;
    }

    bool operator!=(const InterfaceHash& rhs) const
    {
        return !(*this == rhs);
    }
};

struct TypeArena;

// The canonical numbering of a module's interface types, which is how other modules' serialized interfaces refer to
// them. It depends only on the interface's shape, so it is stable for as long as the interface hash is.
struct ModuleInterfaceIndex
{
    std::vector<TypeId> types;
    std::vector<TypePackId> packs;
    InterfaceHash hash;

    // Numbers in place (Type::interfaceNumber) the types and packs `arena` owns, the only ones another module's
    // interface refers to by number, so that no map from them is kept.
    void stamp(const TypeArena& arena) const;

    std::optional<uint32_t> numberOf(TypeId ty) const;
    std::optional<uint32_t> numberOf(TypePackId tp) const;
};

// A stable numbering of every type reachable from the builtin types and an environment's global scopes, and of
// every magic function attached to them. Two processes that load the same definitions number them identically;
// `digest` says whether they did.
struct GlobalTypeRegistry
{
    GlobalTypeRegistry(NotNull<BuiltinTypes> builtinTypes, const GlobalTypes& globals);

    std::optional<uint32_t> find(TypeId ty) const;
    std::optional<uint32_t> find(TypePackId tp) const;
    std::optional<uint32_t> findMagic(const MagicFunction* magic) const;

    TypeId type(uint32_t number) const;
    TypePackId pack(uint32_t number) const;
    std::shared_ptr<MagicFunction> magic(uint32_t number) const;

    size_t typeCount() const
    {
        return types.size();
    }

    size_t packCount() const
    {
        return packs.size();
    }

    // Changes whenever the shape of any numbered type does.
    InterfaceHash digest;

private:
    std::vector<TypeId> types;
    std::vector<TypePackId> packs;
    std::vector<std::shared_ptr<MagicFunction>> magics;
    DenseHashMap<TypeId, uint32_t> typeNumbers;
    DenseHashMap<TypePackId, uint32_t> packNumbers;
    DenseHashMap<const MagicFunction*, uint32_t> magicNumbers;
};

// Names for types the embedder synthesizes outside any module (one extern type per instance in a place, say). A name
// must identify the same type, with the same meaning, in every process that resolves it.
struct InterfaceHostTypes
{
    virtual std::optional<std::string> nameOf(TypeId ty) const = 0;
    virtual std::optional<TypeId> typeNamed(std::string_view name) const = 0;
    virtual std::optional<std::string> nameOfMagic(const MagicFunction* magic) const;
    virtual std::shared_ptr<MagicFunction> magicNamed(std::string_view name) const;

protected:
    ~InterfaceHostTypes() = default;
};

struct InterfaceSerializeContext
{
    NotNull<BuiltinTypes> builtinTypes;
    NotNull<const GlobalTypeRegistry> globals;
    const InterfaceHostTypes* host = nullptr;
};

struct SerializedInterface
{
    // Empty when the interface holds something that cannot be written; `failure` says what.
    std::string bytes;
    std::string failure;
    InterfaceHash hash;
    // Modules whose interface types this one refers to.
    std::vector<std::string> referencedModules;
};

// Numbers `module`'s interface, records the result in `module.interfaceIndex`, and writes it out. Every module a
// reference leaves for must already have its own index. The index and hash are recorded even when writing fails, so
// that a module with an interface nothing can store still has dependents that can be.
SerializedInterface serializeModuleInterface(Module& module, const InterfaceSerializeContext& context);

struct InterfaceDeserializeContext
{
    NotNull<BuiltinTypes> builtinTypes;
    NotNull<const GlobalTypeRegistry> globals;
    const InterfaceHostTypes* host = nullptr;
    // Resolves the modules a serialized interface refers to; each must have an `interfaceIndex`.
    NotNull<ModuleResolver> moduleResolver;
};

// Rebuilds the module `bytes` describes: its interface types in a fresh arena, exactly as `serializeModuleInterface`
// numbered them, with its `interfaceIndex` set. `source` is the module's own parse, which supplies the definitions of
// the user-defined type functions it declares; the module keeps that syntax tree only when its interface needs one of
// them, and has no `root` otherwise. Returns null, with `failure` set, when anything it refers to cannot be found as
// it was.
ModulePtr deserializeModuleInterface(
    std::string_view bytes,
    const SourceModule& source,
    const InterfaceDeserializeContext& context,
    std::string* failure
);

} // namespace Luau
