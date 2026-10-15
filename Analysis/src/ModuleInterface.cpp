// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/ModuleInterface.h"

#include "Luau/BuiltinTypeFunctions.h"
#include "Luau/GlobalTypes.h"
#include "Luau/Module.h"
#include "Luau/ModuleResolver.h"
#include "Luau/Scope.h"
#include "Luau/Type.h"
#include "Luau/TypeArena.h"
#include "Luau/TypeFunction.h"
#include "Luau/TypePack.h"
#include "Luau/UserDefinedTypeFunction.h"

#include <algorithm>
#include <cstring>

namespace Luau
{

std::optional<std::string> InterfaceHostTypes::nameOfMagic(const MagicFunction*) const
{
    return std::nullopt;
}

std::shared_ptr<MagicFunction> InterfaceHostTypes::magicNamed(std::string_view) const
{
    return nullptr;
}

namespace
{

constexpr uint32_t kInterfaceMagic = 0x31494d4c; // "LMI1"
constexpr uint32_t kInterfaceVersion = 1;

// Two independent 64-bit lanes over the same bytes; collisions only cost a wasted reuse check upstream of this.
struct Hasher
{
    uint64_t a = 0x9E3779B97F4A7C15ull;
    uint64_t b = 0xD1B54A32D192ED03ull;
    uint64_t length = 0;

    static uint64_t mix(uint64_t h, uint64_t v, uint64_t k)
    {
        h ^= v * k;
        h = (h << 31) | (h >> 33);
        return h * 0xFF51AFD7ED558CCDull + 0x52DCE729ull;
    }

    void bytes(const void* data, size_t size)
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        length += size;
        while (size >= 8)
        {
            uint64_t v;
            memcpy(&v, p, 8);
            a = mix(a, v, 0x87C37B91114253D5ull);
            b = mix(b, v, 0x4CF5AD432745937Full);
            p += 8;
            size -= 8;
        }
        if (size > 0)
        {
            uint64_t v = 0;
            memcpy(&v, p, size);
            a = mix(a, v ^ (uint64_t(size) << 56), 0x87C37B91114253D5ull);
            b = mix(b, v ^ (uint64_t(size) << 56), 0x4CF5AD432745937Full);
        }
    }

    static uint64_t finalize(uint64_t h)
    {
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    }

    InterfaceHash finish() const
    {
        return InterfaceHash{finalize(a ^ length), finalize(b + length)};
    }
};

struct ByteWriter
{
    std::string out;

    void u8(uint8_t v)
    {
        out.push_back(char(v));
    }

    void varint(uint64_t v)
    {
        while (v >= 0x80)
        {
            out.push_back(char((v & 0x7f) | 0x80));
            v >>= 7;
        }
        out.push_back(char(v));
    }

    void raw(std::string_view v)
    {
        out.append(v);
    }

    void u32(uint32_t v)
    {
        char bytes[4];
        memcpy(bytes, &v, 4);
        out.append(bytes, 4);
    }
};

struct ByteReader
{
    const unsigned char* p = nullptr;
    const unsigned char* end = nullptr;
    bool ok = true;

    ByteReader() = default;
    explicit ByteReader(std::string_view data)
        : p(reinterpret_cast<const unsigned char*>(data.data()))
        , end(reinterpret_cast<const unsigned char*>(data.data()) + data.size())
    {
    }

    uint8_t u8()
    {
        if (p >= end)
        {
            ok = false;
            return 0;
        }
        return *p++;
    }

    uint64_t varint()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7)
        {
            if (p >= end)
            {
                ok = false;
                return 0;
            }
            uint8_t byte = *p++;
            v |= uint64_t(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0)
                return v;
        }
        ok = false;
        return 0;
    }

    uint32_t u32()
    {
        if (end - p < 4)
        {
            ok = false;
            return 0;
        }
        uint32_t v;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    }

    std::string_view raw(size_t size)
    {
        if (size_t(end - p) < size)
        {
            ok = false;
            return {};
        }
        std::string_view v(reinterpret_cast<const char*>(p), size);
        p += size;
        return v;
    }
};

enum TypeTag : uint8_t
{
    TBound = 1,
    TError,
    TGeneric,
    TPrimitive,
    TBooleanSingleton,
    TStringSingleton,
    TFunction,
    TTable,
    TMetatable,
    TAny,
    TUnknown,
    TNever,
    TNoRefine,
    TUnion,
    TIntersection,
    TNegation,
    TTypeFunctionInstance,
    TExtern,
    TLazy,
    TOpaque, // free, blocked and pending types: only ever numbered in the global registry
};

enum PackTag : uint8_t
{
    PBound = 1,
    PError,
    PGeneric,
    PPack,
    PVariadic,
    PTypeFunctionInstance,
    POpaque,
};

enum RefKind : uint8_t
{
    RefLocal = 0,
    RefBuiltin = 1,
    RefGlobal = 2,
    RefHost = 3,
    RefModule = 4,
};

std::vector<TypeId> builtinTypeList(const BuiltinTypes& b)
{
    return {
        b.nilType,      b.numberType, b.integerType,        b.stringType,         b.booleanType, b.threadType,     b.bufferType,
        b.functionType, b.externType, b.objectType,         b.classType,          b.tableType,   b.emptyTableType, b.trueType,
        b.falseType,    b.anyType,    b.unknownType,        b.neverType,          b.errorType,   b.noRefineType,   b.falsyType,
        b.truthyType,   b.notNilType, b.optionalNumberType, b.optionalStringType,
    };
}

std::vector<TypePackId> builtinPackList(const BuiltinTypes& b)
{
    return {
        b.emptyTypePack,
        b.anyTypePack,
        b.unknownTypePack,
        b.neverTypePack,
        b.uninhabitableTypePack,
        b.errorTypePack,
    };
}

std::vector<const TypeFunction*> builtinTypeFunctionList(const BuiltinTypeFunctions& f)
{
    return {
        &f.userFunc,     &f.notFunc,   &f.lenFunc,    &f.unmFunc,          &f.addFunc,          &f.subFunc,       &f.mulFunc,
        &f.divFunc,      &f.idivFunc,  &f.powFunc,    &f.modFunc,          &f.concatFunc,       &f.andFunc,       &f.orFunc,
        &f.ltFunc,       &f.leFunc,    &f.refineFunc, &f.singletonFunc,    &f.unionFunc,        &f.intersectFunc, &f.keyofFunc,
        &f.rawkeyofFunc, &f.indexFunc, &f.rawgetFunc, &f.setmetatableFunc, &f.getmetatableFunc, &f.objectofFunc,  &f.weakoptionalFunc,
    };
}

template<typename Map>
std::vector<typename Map::const_pointer> sortedByName(const Map& map)
{
    std::vector<typename Map::const_pointer> entries;
    entries.reserve(map.size());
    for (const auto& entry : map)
        entries.push_back(&entry);
    std::sort(
        entries.begin(),
        entries.end(),
        [](auto lhs, auto rhs)
        {
            return lhs->first < rhs->first;
        }
    );
    return entries;
}

// Identifies a user-defined type function's definition across a reparse: its name, and how many functions of that
// name precede it in the module.
struct TypeFunctionDefinitions
{
    std::vector<AstStatTypeFunction*> all;

    explicit TypeFunctionDefinitions(AstStatBlock* root)
    {
        struct Collector : AstVisitor
        {
            std::vector<AstStatTypeFunction*>* out;
            bool visit(AstStatTypeFunction* node) override
            {
                out->push_back(node);
                return true;
            }
        };
        if (!root)
            return;
        Collector collector;
        collector.out = &all;
        root->visit(&collector);
    }

    std::optional<uint32_t> ordinalOf(const AstStatTypeFunction* definition) const
    {
        uint32_t ordinal = 0;
        for (AstStatTypeFunction* candidate : all)
        {
            if (candidate == definition)
                return ordinal;
            if (candidate->name == definition->name)
                ordinal++;
        }
        return std::nullopt;
    }

    AstStatTypeFunction* find(std::string_view name, uint32_t ordinal) const
    {
        for (AstStatTypeFunction* candidate : all)
        {
            if (candidate->name.value && name == candidate->name.value)
            {
                if (ordinal == 0)
                    return candidate;
                ordinal--;
            }
        }
        return nullptr;
    }
};

// Walks one type or pack's fields in a fixed order. Numbering, hashing and writing all go through these two
// functions, so a reference is numbered in exactly the order it is written.
template<typename E>
void encodeLocation(E& e, const Location& location)
{
    e.side(location.begin.line);
    e.side(location.begin.column);
    e.side(location.end.line);
    e.side(location.end.column);
}

template<typename E>
void encodeOptionalString(E& e, const std::optional<std::string>& value)
{
    e.u8(value ? 1 : 0);
    if (value)
        e.str(*value);
}

template<typename E>
void encodeTags(E& e, const Tags& tags)
{
    e.varint(tags.size());
    for (const std::string& tag : tags)
        e.str(tag);
}

template<typename E>
void encodeProperty(E& e, const Property& prop)
{
    e.u8(prop.readTy ? 1 : 0);
    if (prop.readTy)
        e.type(*prop.readTy);
    e.u8(prop.writeTy ? 1 : 0);
    if (prop.writeTy)
        e.type(*prop.writeTy);
    e.u8(prop.deprecated ? 1 : 0);
    e.str(prop.deprecatedSuggestion ? std::string_view{*prop.deprecatedSuggestion} : std::string_view{});
    e.side(prop.location ? 1 : 0);
    if (prop.location)
        encodeLocation(e, *prop.location);
    e.side(prop.typeLocation ? 1 : 0);
    if (prop.typeLocation)
        encodeLocation(e, *prop.typeLocation);
    encodeTags(e, prop.tags);
    encodeOptionalString(e, prop.documentationSymbol);
}

template<typename E>
void encodeProps(E& e, const TableType::Props& props)
{
    e.varint(props.size());
    for (const auto& [name, prop] : props)
    {
        e.str(name);
        encodeProperty(e, prop);
    }
}

template<typename E>
void encodeIndexer(E& e, const std::optional<TableIndexer>& indexer)
{
    e.u8(indexer ? 1 : 0);
    if (indexer)
    {
        e.type(indexer->indexType);
        e.type(indexer->indexResultType);
        e.u8(indexer->isReadOnly ? 1 : 0);
    }
}

template<typename E>
void encodeTypeFun(E& e, const TypeFun& tf)
{
    e.varint(tf.typeParams.size());
    for (const GenericTypeDefinition& param : tf.typeParams)
    {
        e.type(param.ty);
        e.u8(param.defaultValue ? 1 : 0);
        if (param.defaultValue)
            e.type(*param.defaultValue);
    }
    e.varint(tf.typePackParams.size());
    for (const GenericTypePackDefinition& param : tf.typePackParams)
    {
        e.pack(param.tp);
        e.u8(param.defaultValue ? 1 : 0);
        if (param.defaultValue)
            e.pack(*param.defaultValue);
    }
    e.type(tf.type);
    e.side(tf.definitionLocation ? 1 : 0);
    if (tf.definitionLocation)
        encodeLocation(e, *tf.definitionLocation);
}

template<typename E>
void encodeGenericName(E& e, const Name& name, bool explicitName)
{
    e.u8(explicitName ? 1 : 0);
    // A generated name embeds a process-wide counter, so it is kept out of the interface hash.
    if (explicitName)
        e.str(name);
    else
        e.sideStr(name);
}

template<typename E>
void encodeType(E& e, TypeId ty)
{
    const Type& t = *ty;
    e.side(t.documentationSymbol ? 1 : 0);
    if (t.documentationSymbol)
        e.sideStr(*t.documentationSymbol);

    if (auto bound = get_if<BoundType>(&t.ty))
    {
        e.u8(TBound);
        e.type(bound->boundTo);
    }
    else if (auto error = get_if<ErrorType>(&t.ty))
    {
        e.u8(TError);
        e.u8(error->synthetic ? 1 : 0);
        if (error->synthetic)
            e.type(*error->synthetic);
    }
    else if (auto generic = get_if<GenericType>(&t.ty))
    {
        e.u8(TGeneric);
        encodeGenericName(e, generic->name, generic->explicitName);
        e.u8(uint8_t(generic->polarity));
        e.varint(generic->level.level);
        e.varint(generic->level.subLevel);
        e.scope(generic->scope);
    }
    else if (auto primitive = get_if<PrimitiveType>(&t.ty))
    {
        e.u8(TPrimitive);
        e.u8(uint8_t(primitive->type));
        e.u8(primitive->metatable ? 1 : 0);
        if (primitive->metatable)
            e.type(*primitive->metatable);
    }
    else if (auto singleton = get_if<SingletonType>(&t.ty))
    {
        if (auto boolean = get<BooleanSingleton>(singleton))
        {
            e.u8(TBooleanSingleton);
            e.u8(boolean->value ? 1 : 0);
        }
        else if (auto string = get<StringSingleton>(singleton))
        {
            e.u8(TStringSingleton);
            e.str(string->value);
        }
        else
            e.fail("unknown singleton");
    }
    else if (auto function = get_if<FunctionType>(&t.ty))
    {
        e.u8(TFunction);
        e.u8(function->definition ? 1 : 0);
        if (function->definition)
        {
            const FunctionDefinition& definition = *function->definition;
            encodeOptionalString(e, definition.definitionModuleName);
            encodeLocation(e, definition.definitionLocation);
            e.side(definition.varargLocation ? 1 : 0);
            if (definition.varargLocation)
                encodeLocation(e, *definition.varargLocation);
            encodeLocation(e, definition.originalNameLocation);
        }
        e.varint(function->generics.size());
        for (TypeId generic : function->generics)
            e.type(generic);
        e.varint(function->genericPacks.size());
        for (TypePackId generic : function->genericPacks)
            e.pack(generic);
        e.varint(function->argNames.size());
        for (const std::optional<FunctionArgument>& arg : function->argNames)
        {
            e.u8(arg ? 1 : 0);
            if (arg)
            {
                e.str(arg->name);
                encodeLocation(e, arg->location);
            }
        }
        encodeTags(e, function->tags);
        e.varint(function->level.level);
        e.varint(function->level.subLevel);
        e.pack(function->argTypes);
        e.pack(function->retTypes);
        e.magic(function->magic.get());
        e.u8(function->hasSelf ? 1 : 0);
        e.u8(function->hasNoFreeOrGenericTypes ? 1 : 0);
        e.u8(function->isCheckedFunction ? 1 : 0);
        e.u8(function->isDeprecatedFunction ? 1 : 0);
        e.u8(function->deprecatedInfo ? 1 : 0);
        if (function->deprecatedInfo)
        {
            e.u8(function->deprecatedInfo->deprecated ? 1 : 0);
            encodeOptionalString(e, function->deprecatedInfo->use);
            encodeOptionalString(e, function->deprecatedInfo->reason);
        }
    }
    else if (auto table = get_if<TableType>(&t.ty))
    {
        e.u8(TTable);
        encodeProps(e, table->props);
        encodeIndexer(e, table->indexer);
        e.u8(uint8_t(table->state));
        e.varint(table->level.level);
        e.varint(table->level.subLevel);
        e.scope(table->scope);
        encodeOptionalString(e, table->name);
        encodeOptionalString(e, table->syntheticName);
        e.varint(table->instantiatedTypeParams.size());
        for (TypeId param : table->instantiatedTypeParams)
            e.type(param);
        e.varint(table->instantiatedTypePackParams.size());
        for (TypePackId param : table->instantiatedTypePackParams)
            e.pack(param);
        e.str(table->definitionModuleName.str());
        encodeLocation(e, table->definitionLocation);
        e.u8(table->boundTo ? 1 : 0);
        if (table->boundTo)
            e.type(*table->boundTo);
        encodeTags(e, table->tags);
        e.varint(table->remainingProps);
    }
    else if (auto metatable = get_if<MetatableType>(&t.ty))
    {
        e.u8(TMetatable);
        e.type(metatable->table);
        e.type(metatable->metatable);
        encodeOptionalString(e, metatable->syntheticName);
    }
    else if (get_if<AnyType>(&t.ty))
        e.u8(TAny);
    else if (get_if<UnknownType>(&t.ty))
        e.u8(TUnknown);
    else if (get_if<NeverType>(&t.ty))
        e.u8(TNever);
    else if (get_if<NoRefineType>(&t.ty))
        e.u8(TNoRefine);
    else if (auto u = get_if<UnionType>(&t.ty))
    {
        e.u8(TUnion);
        e.varint(u->options.size());
        for (TypeId option : u->options)
            e.type(option);
    }
    else if (auto i = get_if<IntersectionType>(&t.ty))
    {
        e.u8(TIntersection);
        e.varint(i->parts.size());
        for (TypeId part : i->parts)
            e.type(part);
    }
    else if (auto negation = get_if<NegationType>(&t.ty))
    {
        e.u8(TNegation);
        e.type(negation->ty);
    }
    else if (auto tfi = get_if<TypeFunctionInstanceType>(&t.ty))
    {
        e.u8(TTypeFunctionInstance);
        e.typeFunction(tfi->function.get());
        e.varint(tfi->typeArguments.size());
        for (TypeId arg : tfi->typeArguments)
            e.type(arg);
        e.varint(tfi->packArguments.size());
        for (TypePackId arg : tfi->packArguments)
            e.pack(arg);
        e.u8(uint8_t(tfi->state));
        e.userFunction(*tfi);
    }
    else if (auto external = get_if<ExternType>(&t.ty))
    {
        e.u8(TExtern);
        e.externType(*external);
        e.str(external->name);
        encodeProps(e, external->props.observe());
        e.u8(external->parent ? 1 : 0);
        if (external->parent)
            e.type(*external->parent);
        e.u8(external->metatable ? 1 : 0);
        if (external->metatable)
            e.type(*external->metatable);
        encodeTags(e, external->tags);
        e.str(external->definitionModuleName);
        e.u8(external->isOpen ? 1 : 0);
        encodeIndexer(e, external->indexer);
    }
    else if (get_if<LazyType>(&t.ty))
    {
        e.u8(TLazy);
        e.opaque("lazy type");
    }
    else
    {
        e.u8(TOpaque);
        e.opaque("unsolved type");
    }
}

template<typename E>
void encodePack(E& e, TypePackId tp)
{
    const TypePackVar& p = *tp;
    if (auto bound = get_if<BoundTypePack>(&p.ty))
    {
        e.u8(PBound);
        e.pack(bound->boundTo);
    }
    else if (auto error = get_if<ErrorTypePack>(&p.ty))
    {
        e.u8(PError);
        e.u8(error->synthetic ? 1 : 0);
        if (error->synthetic)
            e.pack(*error->synthetic);
    }
    else if (auto generic = get_if<GenericTypePack>(&p.ty))
    {
        e.u8(PGeneric);
        encodeGenericName(e, generic->name, generic->explicitName);
        e.u8(uint8_t(generic->polarity));
        e.varint(generic->level.level);
        e.varint(generic->level.subLevel);
        e.scope(generic->scope);
    }
    else if (auto pack = get_if<TypePack>(&p.ty))
    {
        e.u8(PPack);
        e.varint(pack->head.size());
        for (TypeId ty : pack->head)
            e.type(ty);
        e.u8(pack->tail ? 1 : 0);
        if (pack->tail)
            e.pack(*pack->tail);
    }
    else if (auto variadic = get_if<VariadicTypePack>(&p.ty))
    {
        e.u8(PVariadic);
        e.type(variadic->ty);
        e.u8(variadic->hidden ? 1 : 0);
    }
    else if (auto tfi = get_if<TypeFunctionInstanceTypePack>(&p.ty))
    {
        e.u8(PTypeFunctionInstance);
        e.str(tfi->function->name);
        e.opaque("type pack function instance");
        e.varint(tfi->typeArguments.size());
        for (TypeId arg : tfi->typeArguments)
            e.type(arg);
        e.varint(tfi->packArguments.size());
        for (TypePackId arg : tfi->packArguments)
            e.pack(arg);
    }
    else
    {
        e.u8(POpaque);
        e.opaque("unsolved type pack");
    }
}

// Collects the references one node makes, in encoding order; everything else is ignored.
struct ChildCollector
{
    std::vector<TypeId> types;
    std::vector<TypePackId> packs;
    std::vector<const MagicFunction*> magics;

    void u8(uint8_t) {}
    void varint(uint64_t) {}
    void str(std::string_view) {}
    void side(uint64_t) {}
    void sideStr(std::string_view) {}
    void scope(const Scope*) {}
    void fail(const char*) {}
    void opaque(const char*) {}
    void externType(const ExternType&) {}
    void typeFunction(const TypeFunction*) {}
    void userFunction(const TypeFunctionInstanceType&) {}

    void type(TypeId ty)
    {
        types.push_back(ty);
    }

    void pack(TypePackId tp)
    {
        packs.push_back(tp);
    }

    void magic(const MagicFunction* m)
    {
        if (m)
            magics.push_back(m);
    }
};

// Shared by the global registry and module writer: a string table, a hashed main stream and an unhashed side stream.
struct StreamWriter
{
    ByteWriter main;
    ByteWriter sideStream;
    std::vector<std::string> strings;
    DenseHashMap<std::string, uint32_t> stringNumbers;
    std::string failure;

    void u8(uint8_t v)
    {
        main.u8(v);
    }

    void varint(uint64_t v)
    {
        main.varint(v);
    }

    uint32_t intern(std::string_view s)
    {
        std::string key(s);
        if (key.empty())
            return 0;
        if (const uint32_t* found = stringNumbers.find(key))
            return *found;
        strings.push_back(key);
        uint32_t number = uint32_t(strings.size());
        stringNumbers[key] = number;
        return number;
    }

    void str(std::string_view s)
    {
        main.varint(intern(s));
    }

    void side(uint64_t v)
    {
        sideStream.varint(v);
    }

    void sideStr(std::string_view s)
    {
        sideStream.varint(s.size());
        sideStream.raw(s);
    }

    void fail(const char* reason)
    {
        if (failure.empty())
            failure = reason;
    }

    void writeStrings(ByteWriter& out) const
    {
        out.varint(strings.size());
        for (const std::string& s : strings)
        {
            out.varint(s.size());
            out.raw(s);
        }
    }

    InterfaceHash hash(const std::vector<uint8_t>& prefix = {}) const
    {
        ByteWriter table;
        writeStrings(table);
        Hasher hasher;
        if (!prefix.empty())
            hasher.bytes(prefix.data(), prefix.size());
        hasher.bytes(table.out.data(), table.out.size());
        hasher.bytes(main.out.data(), main.out.size());
        return hasher.finish();
    }
};

const TypeFunction* builtinTypeFunctionNamed(const BuiltinTypes& builtinTypes, std::string_view name)
{
    for (const TypeFunction* function : builtinTypeFunctionList(*builtinTypes.typeFunctions))
    {
        if (function->name == name)
            return function;
    }
    return nullptr;
}

} // namespace

// The registry numbers every type reachable from its roots, wherever it lives; the digest hashes all of them.
struct RegistryEncoder : StreamWriter
{
    GlobalTypeRegistry& registry;
    DenseHashMap<TypeId, uint32_t>& typeNumbers;
    DenseHashMap<TypePackId, uint32_t>& packNumbers;
    DenseHashMap<const MagicFunction*, uint32_t>& magicNumbers;

    RegistryEncoder(
        GlobalTypeRegistry& registry,
        DenseHashMap<TypeId, uint32_t>& typeNumbers,
        DenseHashMap<TypePackId, uint32_t>& packNumbers,
        DenseHashMap<const MagicFunction*, uint32_t>& magicNumbers
    )
        : registry(registry)
        , typeNumbers(typeNumbers)
        , packNumbers(packNumbers)
        , magicNumbers(magicNumbers)
    {
    }

    void type(TypeId ty)
    {
        main.varint(typeNumbers[ty]);
    }

    void pack(TypePackId tp)
    {
        main.varint(packNumbers[tp]);
    }

    void magic(const MagicFunction* m)
    {
        main.varint(m ? magicNumbers[m] + 1 : 0);
    }

    void scope(const Scope*) {}
    void opaque(const char*) {}
    void externType(const ExternType&) {}

    void typeFunction(const TypeFunction* function)
    {
        str(function->name);
    }

    void userFunction(const TypeFunctionInstanceType& tfi)
    {
        u8(tfi.userFuncName ? 1 : 0);
        if (tfi.userFuncName && tfi.userFuncName->value)
            str(tfi.userFuncName->value);
    }
};

void ModuleInterfaceIndex::stamp(const TypeArena& arena) const
{
    for (size_t i = 0; i < types.size(); i++)
    {
        if (types[i]->owningArena == &arena)
            types[i]->interfaceNumber = uint32_t(i);
    }
    for (size_t i = 0; i < packs.size(); i++)
    {
        if (packs[i]->owningArena == &arena)
            packs[i]->interfaceNumber = uint32_t(i);
    }
}

std::optional<uint32_t> ModuleInterfaceIndex::numberOf(TypeId ty) const
{
    if (ty->interfaceNumber < types.size() && types[ty->interfaceNumber] == ty)
        return ty->interfaceNumber;
    return std::nullopt;
}

std::optional<uint32_t> ModuleInterfaceIndex::numberOf(TypePackId tp) const
{
    if (tp->interfaceNumber < packs.size() && packs[tp->interfaceNumber] == tp)
        return tp->interfaceNumber;
    return std::nullopt;
}

GlobalTypeRegistry::GlobalTypeRegistry(NotNull<BuiltinTypes> builtinTypes, const GlobalTypes& globals)
{
    std::vector<TypeId> pendingTypes;
    std::vector<TypePackId> pendingPacks;

    for (TypeId ty : builtinTypeList(*builtinTypes))
        pendingTypes.push_back(ty);
    for (TypePackId tp : builtinPackList(*builtinTypes))
        pendingPacks.push_back(tp);

    auto addTypeFun = [&](const TypeFun& tf)
    {
        ChildCollector children;
        encodeTypeFun(children, tf);
        pendingTypes.insert(pendingTypes.end(), children.types.begin(), children.types.end());
        pendingPacks.insert(pendingPacks.end(), children.packs.begin(), children.packs.end());
    };

    auto addScope = [&](const ScopePtr& scope)
    {
        if (!scope)
            return;

        std::vector<std::pair<std::string, TypeId>> bindings;
        for (const auto& [symbol, binding] : scope->bindings)
        {
            if (symbol.global.value)
                bindings.emplace_back(symbol.global.value, binding.typeId);
        }
        std::sort(
            bindings.begin(),
            bindings.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.first < rhs.first;
            }
        );
        for (const auto& [_, ty] : bindings)
            pendingTypes.push_back(ty);

        for (auto entry : sortedByName(scope->exportedTypeBindings))
            addTypeFun(entry->second);
        for (auto entry : sortedByName(scope->privateTypeBindings))
            addTypeFun(entry->second);
    };

    addScope(globals.globalScope);
    addScope(globals.globalTypeFunctionScope);

    // Iterative depth-first numbering, children in encoding order. Roots are visited in the order they were listed.
    struct Item
    {
        TypeId ty = nullptr;
        TypePackId tp = nullptr;
    };
    std::vector<Item> stack;
    for (auto it = pendingPacks.rbegin(); it != pendingPacks.rend(); ++it)
        stack.push_back({nullptr, *it});
    for (auto it = pendingTypes.rbegin(); it != pendingTypes.rend(); ++it)
        stack.push_back({*it, nullptr});

    while (!stack.empty())
    {
        Item item = stack.back();
        stack.pop_back();

        ChildCollector children;
        if (item.ty)
        {
            if (typeNumbers.contains(item.ty))
                continue;
            typeNumbers[item.ty] = uint32_t(types.size());
            types.push_back(item.ty);
            encodeType(children, item.ty);
        }
        else
        {
            if (packNumbers.contains(item.tp))
                continue;
            packNumbers[item.tp] = uint32_t(packs.size());
            packs.push_back(item.tp);
            encodePack(children, item.tp);
        }

        for (const MagicFunction* m : children.magics)
        {
            if (!magicNumbers.contains(m))
            {
                magicNumbers[m] = uint32_t(magics.size());
                magics.push_back(nullptr);
            }
        }

        // Children go on the stack in reverse, so they come off in encoding order: packs after types, as encoded
        // fields interleave them only by kind within a node.
        for (auto it = children.packs.rbegin(); it != children.packs.rend(); ++it)
            stack.push_back({nullptr, *it});
        for (auto it = children.types.rbegin(); it != children.types.rend(); ++it)
            stack.push_back({*it, nullptr});
    }

    // Magic functions are owned by the functions that carry them; keep each alive through the registry.
    for (TypeId ty : types)
    {
        if (const FunctionType* function = get_if<FunctionType>(&ty->ty); function && function->magic)
        {
            if (const uint32_t* number = magicNumbers.find(function->magic.get()))
                magics[*number] = function->magic;
        }
    }

    RegistryEncoder encoder{*this, typeNumbers, packNumbers, magicNumbers};
    for (TypeId ty : types)
        encodeType(encoder, ty);
    for (TypePackId tp : packs)
        encodePack(encoder, tp);
    digest = encoder.hash();
}

std::optional<uint32_t> GlobalTypeRegistry::find(TypeId ty) const
{
    if (const uint32_t* number = typeNumbers.find(ty))
        return *number;
    return std::nullopt;
}

std::optional<uint32_t> GlobalTypeRegistry::find(TypePackId tp) const
{
    if (const uint32_t* number = packNumbers.find(tp))
        return *number;
    return std::nullopt;
}

std::optional<uint32_t> GlobalTypeRegistry::findMagic(const MagicFunction* magic) const
{
    if (const uint32_t* number = magicNumbers.find(magic))
        return *number;
    return std::nullopt;
}

TypeId GlobalTypeRegistry::type(uint32_t number) const
{
    return number < types.size() ? types[number] : nullptr;
}

TypePackId GlobalTypeRegistry::pack(uint32_t number) const
{
    return number < packs.size() ? packs[number] : nullptr;
}

std::shared_ptr<MagicFunction> GlobalTypeRegistry::magic(uint32_t number) const
{
    return number < magics.size() ? magics[number] : nullptr;
}

namespace
{

enum class Placement
{
    Local,
    Builtin,
    Global,
    Host,
    Module,
};

struct ModuleEncoder : StreamWriter
{
    Module& module;
    const InterfaceSerializeContext& context;

    DenseHashMap<TypeId, uint32_t> builtinTypes;
    DenseHashMap<TypePackId, uint32_t> builtinPacks;
    DenseHashMap<TypeId, uint32_t> localTypes;
    DenseHashMap<TypePackId, uint32_t> localPacks;
    std::vector<TypeId> typeOrder;
    std::vector<TypePackId> packOrder;
    std::vector<std::string> referencedModules;
    DenseHashMap<const Module*, std::shared_ptr<TypeFunctionDefinitions>> definitions;
    std::vector<const TypeFunction*> typeFunctions;

    ModuleEncoder(Module& module, const InterfaceSerializeContext& context)
        : module(module)
        , context(context)
    {
        std::vector<TypeId> types = builtinTypeList(*context.builtinTypes);
        for (size_t i = 0; i < types.size(); i++)
            builtinTypes[types[i]] = uint32_t(i);
        std::vector<TypePackId> packs = builtinPackList(*context.builtinTypes);
        for (size_t i = 0; i < packs.size(); i++)
            builtinPacks[packs[i]] = uint32_t(i);
        typeFunctions = builtinTypeFunctionList(*context.builtinTypes->typeFunctions);
    }

    // Types not owned by any module interface but by a module (another module's shared type function results, say)
    // are copied in; an arena with no module is global and must be named.
    bool inlined(TypeArena* arena, bool persistent) const
    {
        return !persistent && arena && arena->owningModule && arena != &arena->owningModule->interfaceTypes;
    }

    Placement place(TypeId ty) const
    {
        if (builtinTypes.contains(ty))
            return Placement::Builtin;
        if (ty->owningArena == &module.interfaceTypes || inlined(ty->owningArena, ty->persistent))
            return Placement::Local;
        if (context.host && context.host->nameOf(ty))
            return Placement::Host;
        if (context.globals->find(ty))
            return Placement::Global;
        return Placement::Module;
    }

    Placement place(TypePackId tp) const
    {
        if (builtinPacks.contains(tp))
            return Placement::Builtin;
        if (tp->owningArena == &module.interfaceTypes || inlined(tp->owningArena, tp->persistent))
            return Placement::Local;
        if (context.globals->find(tp))
            return Placement::Global;
        return Placement::Module;
    }

    void moduleRef(TypeArena* arena, const void* id, bool isType)
    {
        Module* owner = arena ? arena->owningModule : nullptr;
        if (!owner || arena != &owner->interfaceTypes || !owner->interfaceIndex)
        {
            fail("refers to a type no module interface owns");
            main.varint(RefModule);
            main.varint(0);
            return;
        }

        const std::optional<uint32_t> number =
            isType ? owner->interfaceIndex->numberOf(static_cast<TypeId>(id)) : owner->interfaceIndex->numberOf(static_cast<TypePackId>(id));
        if (!number)
        {
            fail("refers to a type outside another module's interface");
            main.varint(RefModule);
            main.varint(0);
            return;
        }

        if (std::find(referencedModules.begin(), referencedModules.end(), owner->name) == referencedModules.end())
            referencedModules.push_back(owner->name);
        main.varint((uint64_t(intern(owner->name)) << 3) | RefModule);
        main.varint(*number);
    }

    void type(TypeId ty)
    {
        switch (place(ty))
        {
        case Placement::Builtin:
            main.varint((uint64_t(builtinTypes[ty]) << 3) | RefBuiltin);
            return;
        case Placement::Local:
            main.varint((uint64_t(localTypes[ty]) << 3) | RefLocal);
            return;
        case Placement::Host:
            main.varint((uint64_t(intern(*context.host->nameOf(ty))) << 3) | RefHost);
            return;
        case Placement::Global:
            main.varint((uint64_t(*context.globals->find(ty)) << 3) | RefGlobal);
            return;
        case Placement::Module:
            moduleRef(ty->owningArena, ty, true);
            return;
        }
    }

    void pack(TypePackId tp)
    {
        switch (place(tp))
        {
        case Placement::Builtin:
            main.varint((uint64_t(builtinPacks[tp]) << 3) | RefBuiltin);
            return;
        case Placement::Local:
            main.varint((uint64_t(localPacks[tp]) << 3) | RefLocal);
            return;
        case Placement::Global:
            main.varint((uint64_t(*context.globals->find(tp)) << 3) | RefGlobal);
            return;
        case Placement::Host:
        case Placement::Module:
            moduleRef(tp->owningArena, tp, false);
            return;
        }
    }

    void magic(const MagicFunction* m)
    {
        if (!m)
        {
            main.varint(0);
            return;
        }
        if (std::optional<uint32_t> number = context.globals->findMagic(m))
        {
            main.varint((uint64_t(*number) << 2) | 1);
            return;
        }
        if (context.host)
        {
            if (std::optional<std::string> name = context.host->nameOfMagic(m))
            {
                main.varint((uint64_t(intern(*name)) << 2) | 2);
                return;
            }
        }
        fail("carries a magic function nothing names");
        main.varint(0);
    }

    void scope(const Scope* scope)
    {
        if (scope)
            fail("holds a scope");
    }

    void opaque(const char* what)
    {
        fail(what);
    }

    void externType(const ExternType&)
    {
        fail("owns an extern type");
    }

    void typeFunction(const TypeFunction* function)
    {
        auto it = std::find(typeFunctions.begin(), typeFunctions.end(), function);
        if (it == typeFunctions.end())
        {
            fail("instantiates a type function that is not built in");
            main.varint(0);
            return;
        }
        main.varint(uint64_t(it - typeFunctions.begin()));
    }

    std::shared_ptr<TypeFunctionDefinitions> definitionsOf(Module* owner)
    {
        if (auto found = definitions.find(owner))
            return *found;
        auto result = std::make_shared<TypeFunctionDefinitions>(owner->root);
        definitions[owner] = result;
        return result;
    }

    void userFunction(const TypeFunctionInstanceType& tfi)
    {
        const UserDefinedFunctionData& data = tfi.userFuncData;
        u8(data.definition ? 1 : 0);
        if (!data.definition)
            return;

        ModulePtr owner = data.owner.lock();
        if (!owner)
        {
            fail("instantiates a user-defined type function whose module is gone");
            return;
        }
        std::shared_ptr<TypeFunctionDefinitions> defs = definitionsOf(owner.get());

        auto writeDefinition = [&](const AstStatTypeFunction* definition)
        {
            std::optional<uint32_t> ordinal = defs->ordinalOf(definition);
            if (!ordinal || !definition->name.value)
            {
                fail("instantiates a user-defined type function outside its module's AST");
                str("");
                varint(0);
                return;
            }
            str(definition->name.value);
            varint(*ordinal);
        };

        str(owner->name);
        writeDefinition(data.definition);

        std::vector<std::pair<std::string, std::pair<AstStatTypeFunction*, size_t>>> functions;
        for (const auto& [name, entry] : data.environmentFunction)
            functions.emplace_back(name, entry);
        std::sort(
            functions.begin(),
            functions.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.first < rhs.first;
            }
        );
        varint(functions.size());
        for (const auto& [name, entry] : functions)
        {
            str(name);
            writeDefinition(entry.first);
            varint(entry.second);
        }

        std::vector<std::pair<std::string, std::pair<TypeFun*, size_t>>> aliases;
        for (const auto& [name, entry] : data.environmentAlias)
            aliases.emplace_back(name, entry);
        std::sort(
            aliases.begin(),
            aliases.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.first < rhs.first;
            }
        );
        varint(aliases.size());
        for (const auto& [name, entry] : aliases)
        {
            str(name);
            size_t ordinal = 0;
            for (; ordinal < owner->typeFunctionAliases.size(); ordinal++)
            {
                if (owner->typeFunctionAliases[ordinal].get() == entry.first)
                    break;
            }
            if (ordinal == owner->typeFunctionAliases.size())
                fail("instantiates a type function whose alias its module does not hold");
            varint(ordinal);
            varint(entry.second);
        }
    }

    // Depth-first from the roots over local nodes only, children in encoding order.
    void number(const std::vector<TypeId>& rootTypes, const std::vector<TypePackId>& rootPacks)
    {
        struct Item
        {
            TypeId ty = nullptr;
            TypePackId tp = nullptr;
        };
        std::vector<Item> stack;
        for (auto it = rootPacks.rbegin(); it != rootPacks.rend(); ++it)
            stack.push_back({nullptr, *it});
        for (auto it = rootTypes.rbegin(); it != rootTypes.rend(); ++it)
            stack.push_back({*it, nullptr});

        while (!stack.empty())
        {
            Item item = stack.back();
            stack.pop_back();

            ChildCollector children;
            if (item.ty)
            {
                if (localTypes.contains(item.ty) || place(item.ty) != Placement::Local)
                    continue;
                localTypes[item.ty] = uint32_t(typeOrder.size());
                typeOrder.push_back(item.ty);
                encodeType(children, item.ty);
            }
            else
            {
                if (localPacks.contains(item.tp) || place(item.tp) != Placement::Local)
                    continue;
                localPacks[item.tp] = uint32_t(packOrder.size());
                packOrder.push_back(item.tp);
                encodePack(children, item.tp);
            }

            for (auto it = children.packs.rbegin(); it != children.packs.rend(); ++it)
                stack.push_back({nullptr, *it});
            for (auto it = children.types.rbegin(); it != children.types.rend(); ++it)
                stack.push_back({*it, nullptr});
        }
    }
};

struct InterfaceRoots
{
    std::vector<const std::pair<const Name, TypeFun>*> exported;
    std::vector<const std::pair<const Name, TypeId>*> globals;
};

InterfaceRoots rootsOf(const Module& module)
{
    InterfaceRoots roots;
    roots.exported = sortedByName(module.exportedTypeBindings);
    roots.globals = sortedByName(module.declaredGlobals);
    return roots;
}

template<typename E>
void encodeRoots(E& e, const Module& module, const InterfaceRoots& roots)
{
    e.u8(module.returnType ? 1 : 0);
    if (module.returnType)
        e.pack(module.returnType);
    e.varint(roots.exported.size());
    for (auto entry : roots.exported)
    {
        e.str(entry->first);
        encodeTypeFun(e, entry->second);
    }
    e.varint(roots.globals.size());
    for (auto entry : roots.globals)
    {
        e.str(entry->first);
        e.type(entry->second);
    }
    e.varint(module.typeFunctionAliases.size());
    for (const std::unique_ptr<TypeFun>& alias : module.typeFunctionAliases)
        encodeTypeFun(e, *alias);
}

} // namespace

SerializedInterface serializeModuleInterface(Module& module, const InterfaceSerializeContext& context)
{
    SerializedInterface result;
    ModuleEncoder encoder{module, context};
    InterfaceRoots roots = rootsOf(module);

    ChildCollector rootChildren;
    encodeRoots(rootChildren, module, roots);
    encoder.number(rootChildren.types, rootChildren.packs);

    encoder.varint(encoder.typeOrder.size());
    encoder.varint(encoder.packOrder.size());
    encoder.varint(module.typeFunctionAliases.size());
    for (TypeId ty : encoder.typeOrder)
        encodeType(encoder, ty);
    for (TypePackId tp : encoder.packOrder)
        encodePack(encoder, tp);
    encodeRoots(encoder, module, roots);

    std::vector<uint8_t> prefix{uint8_t(module.type)};
    result.hash = encoder.hash(prefix);

    auto index = std::make_shared<ModuleInterfaceIndex>();
    index->types = std::move(encoder.typeOrder);
    index->packs = std::move(encoder.packOrder);
    index->hash = result.hash;
    // A checked module's interface arena is frozen; the numbers live in padding nothing else reads.
    unfreeze(module.interfaceTypes);
    index->stamp(module.interfaceTypes);
    freeze(module.interfaceTypes);
    module.interfaceIndex = std::move(index);

    if (!encoder.failure.empty())
    {
        result.failure = std::move(encoder.failure);
        return result;
    }

    ByteWriter out;
    out.u32(kInterfaceMagic);
    out.u32(kInterfaceVersion);
    out.u8(uint8_t(module.type));
    out.u8(uint8_t(module.mode));
    out.u8(module.typeFunctionResults ? 1 : 0);
    encoder.writeStrings(out);
    out.varint(encoder.main.out.size());
    out.raw(encoder.main.out);
    out.varint(encoder.sideStream.out.size());
    out.raw(encoder.sideStream.out);

    result.bytes = std::move(out.out);
    result.referencedModules = std::move(encoder.referencedModules);
    return result;
}

namespace
{

struct ModuleDecoder
{
    const InterfaceDeserializeContext& context;
    Module& module;
    ModulePtr self;
    std::vector<std::string> strings;
    ByteReader main;
    ByteReader sideStream;
    std::vector<TypeId> types;
    std::vector<TypePackId> packs;
    std::vector<TypeId> builtinTypes;
    std::vector<TypePackId> builtinPacks;
    std::vector<const TypeFunction*> typeFunctions;
    DenseHashMap<std::string, ModulePtr> modules;
    DenseHashMap<const Module*, std::shared_ptr<TypeFunctionDefinitions>> definitions;
    std::string failure;
    // Whether a type function instance was bound to one of this module's own definitions, which
    // is the one reason a rebuilt module keeps its syntax tree.
    bool boundOwnDefinitions = false;

    ModuleDecoder(const InterfaceDeserializeContext& context, Module& module)
        : context(context)
        , module(module)
        , builtinTypes(builtinTypeList(*context.builtinTypes))
        , builtinPacks(builtinPackList(*context.builtinTypes))
        , typeFunctions(builtinTypeFunctionList(*context.builtinTypes->typeFunctions))
    {
    }

    void fail(const std::string& reason)
    {
        if (failure.empty())
            failure = reason;
        main.ok = false;
    }

    bool ok() const
    {
        return main.ok && sideStream.ok && failure.empty();
    }

    uint8_t u8()
    {
        return main.u8();
    }

    bool flag()
    {
        return main.u8() != 0;
    }

    uint64_t varint()
    {
        return main.varint();
    }

    const std::string& str()
    {
        static const std::string empty;
        uint64_t number = main.varint();
        if (number == 0)
            return empty;
        if (number > strings.size())
        {
            fail("string out of range");
            return empty;
        }
        return strings[number - 1];
    }

    std::optional<std::string> optionalString()
    {
        if (!flag())
            return std::nullopt;
        return str();
    }

    uint64_t side()
    {
        return sideStream.varint();
    }

    std::string sideStr()
    {
        uint64_t size = sideStream.varint();
        return std::string(sideStream.raw(size_t(size)));
    }

    Location location()
    {
        Location l;
        l.begin.line = unsigned(side());
        l.begin.column = unsigned(side());
        l.end.line = unsigned(side());
        l.end.column = unsigned(side());
        return l;
    }

    ModulePtr moduleNamed(const std::string& name)
    {
        if (ModulePtr* found = modules.find(name))
            return *found;
        ModulePtr found = context.moduleResolver->getModule(name);
        if (found && !found->interfaceIndex)
            found = nullptr;
        modules[name] = found;
        return found;
    }

    TypeId type()
    {
        uint64_t v = main.varint();
        uint64_t n = v >> 3;
        switch (v & 7)
        {
        case RefLocal:
            if (n < types.size())
                return types[n];
            break;
        case RefBuiltin:
            if (n < builtinTypes.size())
                return builtinTypes[n];
            break;
        case RefGlobal:
            if (TypeId ty = context.globals->type(uint32_t(n)))
                return ty;
            break;
        case RefHost:
            if (context.host && n > 0 && n <= strings.size())
            {
                if (std::optional<TypeId> ty = context.host->typeNamed(strings[n - 1]))
                    return *ty;
                fail("no host type named " + strings[n - 1]);
                return context.builtinTypes->errorType;
            }
            break;
        case RefModule:
        {
            uint64_t number = main.varint();
            if (n > 0 && n <= strings.size())
            {
                ModulePtr owner = moduleNamed(strings[n - 1]);
                if (owner && number < owner->interfaceIndex->types.size())
                    return owner->interfaceIndex->types[number];
                fail("no interface for " + strings[n - 1]);
                return context.builtinTypes->errorType;
            }
            break;
        }
        default:
            break;
        }
        fail("bad type reference");
        return context.builtinTypes->errorType;
    }

    TypePackId pack()
    {
        uint64_t v = main.varint();
        uint64_t n = v >> 3;
        switch (v & 7)
        {
        case RefLocal:
            if (n < packs.size())
                return packs[n];
            break;
        case RefBuiltin:
            if (n < builtinPacks.size())
                return builtinPacks[n];
            break;
        case RefGlobal:
            if (TypePackId tp = context.globals->pack(uint32_t(n)))
                return tp;
            break;
        case RefModule:
        {
            uint64_t number = main.varint();
            if (n > 0 && n <= strings.size())
            {
                ModulePtr owner = moduleNamed(strings[n - 1]);
                if (owner && number < owner->interfaceIndex->packs.size())
                    return owner->interfaceIndex->packs[number];
                fail("no interface for " + strings[n - 1]);
                return context.builtinTypes->errorTypePack;
            }
            break;
        }
        default:
            break;
        }
        fail("bad pack reference");
        return context.builtinTypes->errorTypePack;
    }

    std::shared_ptr<MagicFunction> magic()
    {
        uint64_t v = main.varint();
        if (v == 0)
            return nullptr;
        uint64_t n = v >> 2;
        if ((v & 3) == 1)
        {
            if (auto m = context.globals->magic(uint32_t(n)))
                return m;
        }
        else if ((v & 3) == 2 && context.host && n > 0 && n <= strings.size())
        {
            if (auto m = context.host->magicNamed(strings[n - 1]))
                return m;
        }
        fail("unknown magic function");
        return nullptr;
    }

    Tags tags()
    {
        Tags result;
        uint64_t count = varint();
        for (uint64_t i = 0; i < count && main.ok; i++)
            result.push_back(str());
        return result;
    }

    Property property()
    {
        Property prop;
        if (flag())
            prop.readTy = type();
        if (flag())
            prop.writeTy = type();
        prop.deprecated = flag();
        if (std::string suggestion = str(); !suggestion.empty())
            prop.deprecatedSuggestion = std::move(suggestion);
        if (side())
            prop.location = location();
        if (side())
            prop.typeLocation = location();
        prop.tags = tags();
        prop.documentationSymbol = optionalString();
        return prop;
    }

    TableType::Props props()
    {
        TableType::Props result;
        uint64_t count = varint();
        for (uint64_t i = 0; i < count && main.ok; i++)
        {
            std::string name = str();
            result.emplace(std::move(name), property());
        }
        return result;
    }

    std::optional<TableIndexer> indexer()
    {
        if (!flag())
            return std::nullopt;
        TypeId indexType = type();
        TypeId resultType = type();
        bool readOnly = flag();
        return TableIndexer{indexType, resultType, readOnly};
    }

    TypeFun typeFun()
    {
        TypeFun tf;
        uint64_t count = varint();
        for (uint64_t i = 0; i < count && main.ok; i++)
        {
            GenericTypeDefinition param{type(), std::nullopt};
            if (flag())
                param.defaultValue = type();
            tf.typeParams.push_back(param);
        }
        count = varint();
        for (uint64_t i = 0; i < count && main.ok; i++)
        {
            GenericTypePackDefinition param{pack(), std::nullopt};
            if (flag())
                param.defaultValue = pack();
            tf.typePackParams.push_back(param);
        }
        tf.type = type();
        if (side())
            tf.definitionLocation = location();
        return tf;
    }

    std::pair<Name, bool> genericName()
    {
        bool explicitName = flag();
        if (explicitName)
            return {str(), true};
        return {sideStr(), false};
    }

    std::shared_ptr<TypeFunctionDefinitions> definitionsOf(const Module* owner)
    {
        if (auto found = definitions.find(owner))
            return *found;
        auto result = std::make_shared<TypeFunctionDefinitions>(owner->root);
        definitions[owner] = result;
        return result;
    }

    bool userFunction(std::optional<AstName>& userFuncName, UserDefinedFunctionData& data)
    {
        if (!flag())
            return true;

        const std::string& ownerName = str();
        ModulePtr owner = ownerName == module.name ? self : moduleNamed(ownerName);
        if (owner == self)
            boundOwnDefinitions = true;
        if (!owner)
        {
            fail("no module " + ownerName + " for a user-defined type function");
            return false;
        }
        std::shared_ptr<TypeFunctionDefinitions> defs = definitionsOf(owner.get());

        auto readDefinition = [&]() -> AstStatTypeFunction*
        {
            std::string name = str();
            uint64_t ordinal = varint();
            AstStatTypeFunction* definition = defs->find(name, uint32_t(ordinal));
            if (!definition)
                fail("no type function " + name + " in " + ownerName);
            return definition;
        };

        data.owner = owner;
        data.definition = readDefinition();
        if (data.definition)
            userFuncName = data.definition->name;

        uint64_t count = varint();
        for (uint64_t i = 0; i < count && main.ok; i++)
        {
            std::string name = str();
            AstStatTypeFunction* definition = readDefinition();
            size_t level = size_t(varint());
            if (definition)
                data.environmentFunction[name] = std::make_pair(definition, level);
        }

        count = varint();
        for (uint64_t i = 0; i < count && main.ok; i++)
        {
            std::string name = str();
            uint64_t ordinal = varint();
            size_t level = size_t(varint());
            if (ordinal >= owner->typeFunctionAliases.size())
            {
                fail("type function alias out of range");
                return false;
            }
            data.environmentAlias[name] = std::make_pair(owner->typeFunctionAliases[ordinal].get(), level);
        }
        return main.ok;
    }

    void decodeType(TypeId target)
    {
        std::optional<std::string> documentationSymbol;
        if (side())
            documentationSymbol = sideStr();

        Type& t = *asMutable(target);
        t.documentationSymbol = std::move(documentationSymbol);

        uint8_t tag = u8();
        switch (tag)
        {
        case TBound:
            t.ty.emplace<BoundType>(type());
            return;
        case TError:
            if (flag())
                t.ty.emplace<ErrorType>(type());
            else
                t.ty.emplace<ErrorType>();
            return;
        case TGeneric:
        {
            auto [name, explicitName] = genericName();
            GenericType generic{nullptr, std::move(name), Polarity(u8())};
            generic.explicitName = explicitName;
            generic.level.level = int(varint());
            generic.level.subLevel = int(varint());
            t.ty.emplace<GenericType>(std::move(generic));
            return;
        }
        case TPrimitive:
        {
            PrimitiveType primitive{PrimitiveType::Type(u8())};
            if (flag())
                primitive.metatable = type();
            t.ty.emplace<PrimitiveType>(primitive);
            return;
        }
        case TBooleanSingleton:
            t.ty.emplace<SingletonType>(BooleanSingleton{flag()});
            return;
        case TStringSingleton:
            t.ty.emplace<SingletonType>(StringSingleton{str()});
            return;
        case TFunction:
        {
            std::optional<FunctionDefinition> definition;
            if (flag())
            {
                FunctionDefinition d;
                d.definitionModuleName = optionalString();
                d.definitionLocation = location();
                if (side())
                    d.varargLocation = location();
                d.originalNameLocation = location();
                definition = std::move(d);
            }
            std::vector<TypeId> generics;
            uint64_t count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                generics.push_back(type());
            std::vector<TypePackId> genericPacks;
            count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                genericPacks.push_back(pack());
            std::vector<std::optional<FunctionArgument>> argNames;
            count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
            {
                if (flag())
                {
                    FunctionArgument arg;
                    arg.name = str();
                    arg.location = location();
                    argNames.push_back(std::move(arg));
                }
                else
                    argNames.push_back(std::nullopt);
            }
            Tags functionTags = tags();
            TypeLevel level;
            level.level = int(varint());
            level.subLevel = int(varint());
            TypePackId argTypes = pack();
            TypePackId retTypes = pack();
            FunctionType function{level, std::move(generics), std::move(genericPacks), argTypes, retTypes, std::move(definition)};
            function.argNames = std::move(argNames);
            function.tags = std::move(functionTags);
            function.magic = magic();
            function.hasSelf = flag();
            function.hasNoFreeOrGenericTypes = flag();
            function.isCheckedFunction = flag();
            function.isDeprecatedFunction = flag();
            if (flag())
            {
                auto info = std::make_shared<AstAttr::DeprecatedInfo>();
                info->deprecated = flag();
                info->use = optionalString();
                info->reason = optionalString();
                function.deprecatedInfo = std::move(info);
            }
            t.ty.emplace<FunctionType>(std::move(function));
            return;
        }
        case TTable:
        {
            TableType table;
            table.props = props();
            table.indexer = indexer();
            table.state = TableState(u8());
            table.level.level = int(varint());
            table.level.subLevel = int(varint());
            table.name = optionalString();
            table.syntheticName = optionalString();
            uint64_t count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                table.instantiatedTypeParams.push_back(type());
            count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                table.instantiatedTypePackParams.push_back(pack());
            table.definitionModuleName = str();
            table.definitionLocation = location();
            if (flag())
                table.boundTo = type();
            table.tags = tags();
            table.remainingProps = size_t(varint());
            t.ty.emplace<TableType>(std::move(table));
            return;
        }
        case TMetatable:
        {
            MetatableType metatable;
            metatable.table = type();
            metatable.metatable = type();
            metatable.syntheticName = optionalString();
            t.ty.emplace<MetatableType>(std::move(metatable));
            return;
        }
        case TAny:
            t.ty.emplace<AnyType>();
            return;
        case TUnknown:
            t.ty.emplace<UnknownType>();
            return;
        case TNever:
            t.ty.emplace<NeverType>();
            return;
        case TNoRefine:
            t.ty.emplace<NoRefineType>();
            return;
        case TUnion:
        case TIntersection:
        {
            std::vector<TypeId> parts;
            uint64_t count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                parts.push_back(type());
            if (tag == TUnion)
                t.ty.emplace<UnionType>(UnionType{std::move(parts)});
            else
                t.ty.emplace<IntersectionType>(IntersectionType{std::move(parts)});
            return;
        }
        case TNegation:
            t.ty.emplace<NegationType>(NegationType{type()});
            return;
        case TTypeFunctionInstance:
        {
            uint64_t functionNumber = varint();
            if (functionNumber >= typeFunctions.size())
            {
                fail("unknown type function");
                return;
            }
            std::vector<TypeId> typeArguments;
            uint64_t count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                typeArguments.push_back(type());
            std::vector<TypePackId> packArguments;
            count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                packArguments.push_back(pack());
            TypeFunctionInstanceState state = TypeFunctionInstanceState(u8());
            std::optional<AstName> userFuncName;
            UserDefinedFunctionData data;
            if (!userFunction(userFuncName, data))
                return;
            TypeFunctionInstanceType instance{
                NotNull{typeFunctions[functionNumber]}, std::move(typeArguments), std::move(packArguments), userFuncName, std::move(data)
            };
            instance.state = state;
            t.ty.emplace<TypeFunctionInstanceType>(std::move(instance));
            return;
        }
        default:
            fail("unsupported type tag " + std::to_string(tag));
            return;
        }
    }

    void decodePack(TypePackId target)
    {
        TypePackVar& p = *asMutable(target);
        uint8_t tag = u8();
        switch (tag)
        {
        case PBound:
            p.ty.emplace<BoundTypePack>(pack());
            return;
        case PError:
            if (flag())
                p.ty.emplace<ErrorTypePack>(pack());
            else
                p.ty.emplace<ErrorTypePack>();
            return;
        case PGeneric:
        {
            auto [name, explicitName] = genericName();
            GenericTypePack generic{nullptr, std::move(name), Polarity(u8())};
            generic.explicitName = explicitName;
            generic.level.level = int(varint());
            generic.level.subLevel = int(varint());
            p.ty.emplace<GenericTypePack>(std::move(generic));
            return;
        }
        case PPack:
        {
            TypePack result;
            uint64_t count = varint();
            for (uint64_t i = 0; i < count && main.ok; i++)
                result.head.push_back(type());
            if (flag())
                result.tail = pack();
            p.ty.emplace<TypePack>(std::move(result));
            return;
        }
        case PVariadic:
        {
            VariadicTypePack variadic;
            variadic.ty = type();
            variadic.hidden = flag();
            p.ty.emplace<VariadicTypePack>(variadic);
            return;
        }
        default:
            fail("unsupported pack tag " + std::to_string(tag));
            return;
        }
    }
};

} // namespace

ModulePtr deserializeModuleInterface(
    std::string_view bytes,
    const SourceModule& source,
    const InterfaceDeserializeContext& context,
    std::string* failure
)
{
    auto setFailure = [&](const std::string& reason) -> ModulePtr
    {
        if (failure)
            *failure = reason;
        return nullptr;
    };

    ByteReader header{bytes};
    if (header.u32() != kInterfaceMagic || header.u32() != kInterfaceVersion || !header.ok)
        return setFailure("not a module interface of this version");

    ModulePtr module = std::make_shared<Module>(std::make_shared<TypeArena>());
    module->checkedInNewSolver = true;
    module->name = source.name;
    module->humanReadableName = source.humanReadableName;
    module->type = SourceCode::Type(header.u8());
    module->mode = Mode(header.u8());
    bool hasTypeFunctionResults = header.u8() != 0;
    module->internalTypes->owningModule = module.get();
    module->interfaceTypes.owningModule = module.get();
    module->allocator = source.allocator;
    module->names = source.names;
    module->root = source.root;
    module->constraintGenerationDidNotComplete = false;
    if (hasTypeFunctionResults)
    {
        module->typeFunctionResults = std::make_shared<UserDefinedTypeFunctionResults>();
        module->typeFunctionResults->arena.owningModule = module.get();
    }

    ModuleDecoder decoder{context, *module};
    decoder.self = module;

    uint64_t stringCount = header.varint();
    for (uint64_t i = 0; i < stringCount && header.ok; i++)
    {
        uint64_t size = header.varint();
        decoder.strings.emplace_back(header.raw(size_t(size)));
    }
    uint64_t mainSize = header.varint();
    decoder.main = ByteReader{header.raw(size_t(mainSize))};
    uint64_t sideSize = header.varint();
    decoder.sideStream = ByteReader{header.raw(size_t(sideSize))};
    if (!header.ok)
        return setFailure("truncated module interface");

    std::string_view mainBytes(reinterpret_cast<const char*>(decoder.main.p), size_t(decoder.main.end - decoder.main.p));

    uint64_t typeCount = decoder.varint();
    uint64_t packCount = decoder.varint();
    uint64_t aliasCount = decoder.varint();
    if (!decoder.ok() || typeCount > bytes.size() || packCount > bytes.size() || aliasCount > bytes.size())
        return setFailure("bad module interface counts");

    // Type function instances can name this module's aliases before they are read, so they exist from the start.
    for (uint64_t i = 0; i < aliasCount; i++)
        module->typeFunctionAliases.push_back(std::make_unique<TypeFun>());

    decoder.types.reserve(size_t(typeCount));
    for (uint64_t i = 0; i < typeCount; i++)
        decoder.types.push_back(module->interfaceTypes.addType(AnyType{}));
    decoder.packs.reserve(size_t(packCount));
    for (uint64_t i = 0; i < packCount; i++)
        decoder.packs.push_back(module->interfaceTypes.addTypePack(TypePack{}));

    for (TypeId ty : decoder.types)
    {
        decoder.decodeType(ty);
        if (!decoder.ok())
            return setFailure(decoder.failure.empty() ? "malformed type" : decoder.failure);
    }
    for (TypePackId tp : decoder.packs)
    {
        decoder.decodePack(tp);
        if (!decoder.ok())
            return setFailure(decoder.failure.empty() ? "malformed pack" : decoder.failure);
    }

    if (decoder.flag())
        module->returnType = decoder.pack();
    uint64_t count = decoder.varint();
    for (uint64_t i = 0; i < count && decoder.ok(); i++)
    {
        std::string name = decoder.str();
        module->exportedTypeBindings[name] = decoder.typeFun();
    }
    count = decoder.varint();
    for (uint64_t i = 0; i < count && decoder.ok(); i++)
    {
        std::string name = decoder.str();
        module->declaredGlobals[name] = decoder.type();
    }
    count = decoder.varint();
    if (count != aliasCount)
        return setFailure("type function alias count changed");
    for (uint64_t i = 0; i < count && decoder.ok(); i++)
        *module->typeFunctionAliases[i] = decoder.typeFun();

    if (!decoder.ok())
        return setFailure(decoder.failure.empty() ? "malformed roots" : decoder.failure);

    if (!decoder.boundOwnDefinitions)
    {
        module->allocator.reset();
        module->names.reset();
        module->root = nullptr;
    }

    // Hash exactly what the writer hashed: the type byte, the string table and the main stream.
    ByteWriter table;
    table.varint(decoder.strings.size());
    for (const std::string& s : decoder.strings)
    {
        table.varint(s.size());
        table.raw(s);
    }
    Hasher hasher;
    uint8_t typeByte = uint8_t(module->type);
    hasher.bytes(&typeByte, 1);
    hasher.bytes(table.out.data(), table.out.size());
    hasher.bytes(mainBytes.data(), mainBytes.size());

    auto index = std::make_shared<ModuleInterfaceIndex>();
    index->types = decoder.types;
    index->packs = decoder.packs;
    index->hash = hasher.finish();
    index->stamp(module->interfaceTypes);
    module->interfaceIndex = std::move(index);

    freeze(module->interfaceTypes);
    return module;
}

} // namespace Luau
