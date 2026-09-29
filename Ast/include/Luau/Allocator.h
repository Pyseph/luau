// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Ast.h"
#include "Luau/Location.h"
#include "Luau/DenseHash.h"
#include "Luau/Common.h"

#include <vector>

namespace Luau
{

class Allocator
{
public:
    Allocator();
    Allocator(Allocator&&);

    Allocator& operator=(Allocator&&) = delete;

    ~Allocator();

    void* allocate(size_t size);

    template<typename T, typename... Args>
    T* alloc(Args&&... args)
    {
        static_assert(std::is_trivially_destructible<T>::value, "Objects allocated with this allocator will never have their destructors run!");

        T* t = static_cast<T*>(allocate(sizeof(T)));
        new (t) T(std::forward<Args>(args)...);
        return t;
    }

private:
    // Allocators that round requests up to size classes can serve a page of exactly this size without waste.
    static constexpr size_t kPageSize = 8192;

    struct Page
    {
        Page* next;

        alignas(8) char data[kPageSize - 8];
    };

    static_assert(sizeof(Page) == kPageSize, "Page header and data should add up to exactly kPageSize");

    Page* root;
    size_t offset;
};

} // namespace Luau
