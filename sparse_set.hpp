/**
 * @file   sparse_set.hpp
 * @brief  Project Aegis — Phase 3: Cache-Friendly Sparse Set
 *
 * A SparseSet maps arbitrary uint32_t keys (Entity IDs) to a dense contiguous
 * array of values.  This is the canonical data structure for ECS component stores
 * because it provides:
 *
 *   • O(1) Has / Get / Emplace / Remove  (amortised, no hash, no tree)
 *   • Contiguous dense array             — cache-friendly linear iteration
 *   • Stable iteration 
 *
 * Memory layout:
 *   sparse_[entity_id]  → index into dense_   (uint32_t, page-allocated)
 *   dense_[index]       → entity_id           (uint32_t[])
 *   values_[index]      → component T         (T[])
 *
 * Index relationship invariant:
 *   sparse_[dense_[i]] == i      for all valid i in [0, size_)
 *   dense_[sparse_[e]] == e      for all live entities e
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility 

// ─────────────────────────────────────────────────────────────────────────────
//  SparseSet<T, PageSize>
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Maps uint32_t entity IDs → contiguous array of T.
 *
 * @tparam T         Component value type (must be movable).
 * @tparam Capacity  Maximum number of distinct entity IDs that can exist.
 *                   This bounds the sparse array size.
 */
template <typename T, uint32_t Capacity = 65536u>
class SparseSet
{
    static_assert(Capacity > 0u, "[Aegis] SparseSet Capacity must be > 0.");

public:
    using entity_type = uint32_t;
    using index_type  = uint32_t;
    using size_type   = std::size_t;

    // ── Construction ──────────────────────────────────────────────────────────

    SparseSet() noexcept : size_(0u)
    {
        // Fill every sparse slot with the null sentinel.
        for (uint32_t i = 0u; i < Capacity; ++i) {
            sparse_[i] = kNullIndex;
        }
    }

    SparseSet(const SparseSet&)            = delete;
    SparseSet& operator=(const SparseSet&) = delete;
    SparseSet(SparseSet&&)                 = delete;
    SparseSet& operator=(SparseSet&&)      = delete;

    // ── Queries ───────────────────────────────────────────────────────────────

    [[nodiscard]] bool Has(entity_type e) const noexcept
    {
        assert(e < Capacity && "[Aegis] entity ID out of sparse range.");
        return sparse_[e] != kNullIndex;
    }

    [[nodiscard]] size_type Size() const noexcept { return size_; }
    [[nodiscard]] bool      Empty() const noexcept { return size_ == 0u; }

    // ── Accessors ─────────────────────────────────────────────────────────────

    /**
     * @brief  Return a reference to the component for entity @p e.
     * @pre    Has(e) must be true.
     */
    [[nodiscard]] T& Get(entity_type e) noexcept
    {
        assert(Has(e) && "[Aegis] SparseSet::Get() called for entity without component.");
        return values_[sparse_[e]];
    }

    [[nodiscard]] const T& Get(entity_type e) const noexcept
    {
        assert(Has(e) && "[Aegis] SparseSet::Get() called for entity without component.");
        return values_[sparse_[e]];
    }

    /// Direct access to the dense component array — used by View<> for SIMD/linear sweeps.
    [[nodiscard]] T*       Data() noexcept       { return values_; }
    [[nodiscard]] const T* Data() const noexcept { return values_; }

    /// Dense entity ID array — parallel to Data().
    [[nodiscard]] entity_type*       Entities() noexcept       { return dense_; }
    [[nodiscard]] const entity_type* Entities() const noexcept { return dense_; }

    // ── Mutation ──────────────────────────────────────────────────────────────

    /**
     * @brief  Add a component for entity @p e.
     *
     * If @p e already has a component, this overwrites it (idempotent insert).
     * Hot path: two array writes + one bounds check.  No heap allocation.
     *
     * @return Reference to the newly inserted (or overwritten) component.
     */
    template <typename... Args>
    T& Emplace(entity_type e, Args&&... args) noexcept
    {
        assert(e < Capacity && "[Aegis] entity ID out of sparse range.");
        assert(size_ < Capacity && "[Aegis] SparseSet is full.");

        if (Has(e)) {
            // Overwrite existing
            values_[sparse_[e]] = T{std::forward<Args>(args)...};
            return values_[sparse_[e]];
        }

        // Append to the dense arrays
        const index_type idx = static_cast<index_type>(size_);
        sparse_[e]  = idx;
        dense_[idx] = e;
        new (&values_[idx]) T{std::forward<Args>(args)...};
        ++size_;
        return values_[idx];
    }

    /**
     * @brief  Remove the component for entity @p e.
     *
     * Uses the swap-and-pop idiom to keep the dense array contiguous.
     * After removal, the last element in values_[] moves into the vacated slot.
     * O(1), no memory freed.
     *
     * @pre    Has(e) must be true.
     */
    void Remove(entity_type e) noexcept
    {
        assert(Has(e) && "[Aegis] SparseSet::Remove() called for entity without component.");

        const index_type  idx      = sparse_[e];
        const index_type  last_idx = static_cast<index_type>(size_ - 1u);
        const entity_type last_e   = dense_[last_idx];

        // Move last element into the vacated slot.
        values_[idx] = std::move(values_[last_idx]);
        dense_[idx]  = last_e;

        // Update the sparse pointer for the moved entity.
        sparse_[last_e] = idx;

        // Mark e as absent.
        sparse_[e] = kNullIndex;
        --size_;
    }

    /// Remove all entries in O(N) — resets sparse sentinels.
    void Clear() noexcept
    {
        for (size_t i = 0u; i < size_; ++i) {
            sparse_[dense_[i]] = kNullIndex;
        }
        size_ = 0u;
    }

    // ── Iteration ─────────────────────────────────────────────────────────────

    /// Range-for over the dense entity list.
    [[nodiscard]] entity_type* begin() noexcept { return dense_; }
    [[nodiscard]] entity_type* end()   noexcept { return dense_ + size_; }
    [[nodiscard]] const entity_type* begin() const noexcept { return dense_; }
    [[nodiscard]] const entity_type* end()   const noexcept { return dense_ + size_; }

private:
    // ── Storage ───────────────────────────────────────────────────────────────

    // sparse_ is the "index-of" lookup: indexed by entity ID.
    // Aligned to 64 bytes so the first cache line fetch covers entity IDs 0–15.
    alignas(64) index_type sparse_[Capacity];

    // dense_ and values_ are parallel arrays, tightly packed.
    alignas(64) entity_type dense_[Capacity];
    alignas(64) T           values_[Capacity];

    size_type size_;
};

} // namespace aegis
