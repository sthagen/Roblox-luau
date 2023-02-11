// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Type.h"
#include "Luau/TypeArena.h"
#include "Luau/TypePack.h"
#include "Luau/Variant.h"

namespace Luau
{

namespace detail
{
template<typename T>
struct ReductionContext
{
    T type = nullptr;
    bool irreducible = false;
};
} // namespace detail

struct TypeReductionOptions
{
    /// If it's desirable for type reduction to allocate into a different arena than the TypeReduction instance you have, you will need
    /// to create a temporary TypeReduction in that case, and set [`TypeReductionOptions::allowTypeReductionsFromOtherArenas`] to true.
    /// This is because TypeReduction caches the reduced type.
    bool allowTypeReductionsFromOtherArenas = false;
};

struct TypeReduction
{
    explicit TypeReduction(
        NotNull<TypeArena> arena, NotNull<BuiltinTypes> builtinTypes, NotNull<InternalErrorReporter> handle, const TypeReductionOptions& opts = {});

    TypeReduction(const TypeReduction&) = delete;
    TypeReduction& operator=(const TypeReduction&) = delete;

    TypeReduction(TypeReduction&&) = default;
    TypeReduction& operator=(TypeReduction&&) = default;

    std::optional<TypeId> reduce(TypeId ty);
    std::optional<TypePackId> reduce(TypePackId tp);
    std::optional<TypeFun> reduce(const TypeFun& fun);

    /// Creating a child TypeReduction will allow the parent TypeReduction to share its memoization with the child TypeReductions.
    /// This is safe as long as the parent's TypeArena continues to outlive both TypeReduction memoization.
    TypeReduction fork(NotNull<TypeArena> arena, const TypeReductionOptions& opts = {}) const;

private:
    const TypeReduction* parent = nullptr;

    NotNull<TypeArena> arena;
    NotNull<BuiltinTypes> builtinTypes;
    NotNull<struct InternalErrorReporter> handle;
    TypeReductionOptions options;

    DenseHashMap<TypeId, detail::ReductionContext<TypeId>> memoizedTypes{nullptr};
    DenseHashMap<TypePackId, detail::ReductionContext<TypePackId>> memoizedTypePacks{nullptr};

    // Computes an *estimated length* of the cartesian product of the given type.
    size_t cartesianProductSize(TypeId ty) const;

    bool hasExceededCartesianProductLimit(TypeId ty) const;
    bool hasExceededCartesianProductLimit(TypePackId tp) const;

    std::optional<TypeId> memoizedof(TypeId ty) const;
    std::optional<TypePackId> memoizedof(TypePackId tp) const;
};

} // namespace Luau
