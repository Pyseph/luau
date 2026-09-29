// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Common.h"

#include <vector>
#include <memory>

namespace Luau
{

void* pagedAllocate(size_t size);
void pagedDeallocate(void* ptr, size_t size);
void pagedFreeze(void* ptr, size_t size);
void pagedUnfreeze(void* ptr, size_t size);

template<typename T>
class TypedAllocator
{
public:
    TypedAllocator() = default;

    // For allocators that often hold only a few values: blocks start at a single page and double up to the usual size.
    explicit TypedAllocator(bool growBlocks)
        : growBlocks(growBlocks)
    {
    }

    TypedAllocator(const TypedAllocator&) = delete;
    TypedAllocator& operator=(const TypedAllocator&) = delete;

    TypedAllocator(TypedAllocator&&) = default;
    TypedAllocator& operator=(TypedAllocator&&) = default;

    ~TypedAllocator()
    {
        if (frozen)
            unfreeze();
        free();
    }

    template<typename... Args>
    T* allocate(Args&&... args)
    {
        LUAU_ASSERT(!frozen);

        if (currentBlockSize >= currentBlockCapacity)
        {
            LUAU_ASSERT(currentBlockSize == currentBlockCapacity);
            appendBlock();
        }

        T* block = stuff.back();
        T* res = block + currentBlockSize;
        new (res) T(std::forward<Args>(args)...);
        ++currentBlockSize;
        return res;
    }

    bool contains(const T* ptr) const
    {
        for (size_t i = 0; i < stuff.size(); ++i)
            if (ptr >= stuff[i] && ptr < stuff[i] + blockCapacity(i))
                return true;

        return false;
    }

    bool empty() const
    {
        return stuff.empty();
    }

    size_t size() const
    {
        return previousBlocksSize + currentBlockSize;
    }

    void clear()
    {
        if (frozen)
            unfreeze();
        free();
    }

    void freeze()
    {
        for (size_t i = 0; i < stuff.size(); ++i)
            pagedFreeze(stuff[i], blockSizeBytes(i));
        frozen = true;
    }

    void unfreeze()
    {
        for (size_t i = 0; i < stuff.size(); ++i)
            pagedUnfreeze(stuff[i], blockSizeBytes(i));
        frozen = false;
    }

    bool isFrozen()
    {
        return frozen;
    }

private:
    void free()
    {
        LUAU_ASSERT(!frozen);

        for (size_t i = 0; i < stuff.size(); ++i)
        {
            size_t blockSize = (i + 1 == stuff.size()) ? currentBlockSize : blockCapacity(i);

            for (size_t j = 0; j < blockSize; ++j)
                stuff[i][j].~T();

            pagedDeallocate(stuff[i], blockSizeBytes(i));
        }

        stuff.clear();
        currentBlockSize = 0;
        currentBlockCapacity = 0;
        previousBlocksSize = 0;
    }

    void appendBlock()
    {
        size_t index = stuff.size();
        void* block = pagedAllocate(blockSizeBytes(index));
        if (!block)
            throw std::bad_alloc();

        stuff.emplace_back(static_cast<T*>(block));
        previousBlocksSize += currentBlockSize;
        currentBlockSize = 0;
        currentBlockCapacity = blockCapacity(index);
    }

    size_t blockSizeBytes(size_t index) const
    {
        if (!growBlocks || index >= kGrowingBlocks)
            return kBlockSizeBytes;

        return kFirstBlockSizeBytes << index;
    }

    size_t blockCapacity(size_t index) const
    {
        return blockSizeBytes(index) / sizeof(T);
    }

    bool frozen = false;
    bool growBlocks = false;
    std::vector<T*> stuff;
    size_t currentBlockSize = 0;
    size_t currentBlockCapacity = 0;
    size_t previousBlocksSize = 0;

    static constexpr size_t kFirstBlockSizeBytes = 4096;
    static constexpr size_t kBlockSizeBytes = 32768;
    static constexpr size_t kGrowingBlocks = 3;

    static_assert((kFirstBlockSizeBytes << kGrowingBlocks) == kBlockSizeBytes);
    static_assert(sizeof(T) <= kFirstBlockSizeBytes);
};

} // namespace Luau
