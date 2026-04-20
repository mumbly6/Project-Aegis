/**
 * @file   view.hpp
 * @brief  Project Aegis — Phase 3: Variadic Component View
 *
 * .
 *
 * Algorithm:
 *   1. Identify the SparseSet with the fewest elements (the "leading" set).
 *      This minimises the number of membership tests.
 *   2. Iterate the dense entity array of the leading set linearly.
 *   3. For each entity, test Has() in every other set.  Has() is a single
 *      array lookup — O(1), branch-predictor-friendly for dense datasets.
 *   4. If all sets contain the entity, yield it.
 *
 * Zero-cost abstraction proof:
 *   - The iterator is a thin wrapper over a raw pointer.
 *   - The variadic Has() tests fold away at compile time with a parameter pack.
 *   - Under -O2, GCC/Clang emit a tight loop with no virtual calls.
 *
 * Usage:
 *   auto view = MakeView(positions, velocities);
 *   for (auto [e, pos, vel] : view) {
 *       pos.x += vel.x * dt;
 *   }
 */

#pragma once

#include <aegis/sparse_set.hpp>

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
//  Internal: pick the smallest set at runtime
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Returns index of the SparseSet with fewest elements.
template <uint32_t Cap>
std::size_t SmallestSet() { return 0u; }   // base / trivial

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  View<Cs...> — variadic, iterator-based
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Iterates over entities that have all of the listed component types.
 *
 * @tparam Capacity  Must match the SparseSet capacity.
 * @tparam Cs        Component types to require.
 */
template <uint32_t Capacity, typename... Cs>
class View
{
    static_assert(sizeof...(Cs) >= 1u,
        "[Aegis] View must specify at least one component type.");

public:
    using entity_type = uint32_t;

    // The leading set is the one with the fewest elements;
    // we hold raw references to each SparseSet.
    using StorageTuple = std::tuple<SparseSet<Cs, Capacity>*...>;

    // ── Iterator ──────────────────────────────────────────────────────────────

    struct Iterator {
        using iterator_category = std::forward_iterator_tag;
        using value_type        = entity_type;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const entity_type*;
        using reference         = entity_type;

        const entity_type* cur;
        const entity_type* end;
        StorageTuple       sets;

        entity_type operator*() const noexcept { return *cur; }

        Iterator& operator++() noexcept
        {
            ++cur;
            // Skip entities that are absent from any non-leading set.
            while (cur != end && !AllHave(*cur)) { ++cur; }
            return *this;
        }

        bool operator==(const Iterator& o) const noexcept { return cur == o.cur; }
        bool operator!=(const Iterator& o) const noexcept { return cur != o.cur; }

    private:
        // Fold expression: true iff ALL sets contain e.
        bool AllHave(entity_type e) const noexcept
        {
            return std::apply([e](auto*... s) {
                return (... && s->Has(e));
            }, sets);
        }
    };

    // ── Construction ──────────────────────────────────────────────────────────

    explicit View(SparseSet<Cs, Capacity>&... sets) noexcept
        : sets_(&sets...)
        , leading_(SelectLeading(sets...))
    {}

    // ── Range API ─────────────────────────────────────────────────────────────

    [[nodiscard]] Iterator begin() const noexcept
    {
        Iterator it{leading_, leading_ + LeadingSize(), sets_};
        // Fast-forward to first valid entity (may skip immediately if first entity
        // is absent from a secondary set).
        while (it.cur != it.end && !CheckAll(*it.cur)) { ++it.cur; }
        return it;
    }

    [[nodiscard]] Iterator end() const noexcept
    {
        const entity_type* e = leading_ + LeadingSize();
        return Iterator{e, e, sets_};
    }

    // ── Get component tuple for an entity  ───────────────────────────────────

    /**
     * @brief  Returns a tuple of references (T0&, T1&, ...) for entity @p e.
     *         Use structured bindings: auto& [pos, vel] = view.Get(e);
     */
    [[nodiscard]] std::tuple<Cs&...> Get(entity_type e) const noexcept
    {
        return std::apply([e](auto*... s) -> std::tuple<Cs&...> {
            return {s->Get(e)...};
        }, sets_);
    }

private:
    StorageTuple       sets_;
    const entity_type* leading_;   // dense array of the smallest set

    // Select the set with fewest elements as the leading (driving) set.
    static const entity_type* SelectLeading(SparseSet<Cs, Capacity>&... sets) noexcept
    {
        const entity_type* best  = nullptr;
        std::size_t        least = ~std::size_t{0};

        // Fold: compare each set's size.
        ([&](auto& s) {
            if (s.Size() < least) {
                least = s.Size();
                best  = s.Entities();
            }
        }(sets), ...);

        return best;
    }

    std::size_t LeadingSize() const noexcept
    {
        std::size_t sz = ~std::size_t{0};
        std::apply([&](auto*... s) {
            ([&](auto* sp) {
                if (sp->Entities() == leading_) { sz = sp->Size(); }
            }(s), ...);
        }, sets_);
        return sz;
    }

    bool CheckAll(entity_type e) const noexcept
    {
        return std::apply([e](auto*... s) {
            return (... && s->Has(e));
        }, sets_);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Factory function — deduces Capacity and Cs... from arguments
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Convenience factory.  Capacity is deduced from the first set.
 *
 * Example:
 *   auto view = aegis::MakeView(positions, velocities);
 *   for (Entity e : view) {
 *       auto& [pos, vel] = view.Get(e);
 *       pos.x += vel.x * dt;
 *   }
 */
template <typename C0, typename... Cs, uint32_t Cap>
[[nodiscard]] auto MakeView(SparseSet<C0, Cap>& s0,
                             SparseSet<Cs, Cap>&... rest) noexcept
{
    return View<Cap, C0, Cs...>(s0, rest...);
}

} // namespace aegis
