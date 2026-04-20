/**
 * @file   entity_manager.hpp
 * @brief  Project Aegis — Phase 3: Entity Lifecycle Manager
 *
 * Entities are plain uint32_t handles split into two fields:
 *
 *   Bits [31..8]  — Index (24 bits → up to 16,777,216 live entities)
 *   Bits [7..0]   — Generation (8 bits → detects stale handles after Destroy)
 *
 * The generation 
 *
 * Memory:
 *   No heap.  Entity slots are a fixed C-array.  A freelist (intrusive singly-
 *   linked list through the dead slots) provides O(1) Create and Destroy.
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <limits>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
//  Entity handle
// ─────────────────────────────────────────────────────────────────────────────

using Entity = uint32_t;

inline constexpr Entity kNullEntity = std::numeric_limits<Entity>::max();

namespace detail {

inline constexpr uint32_t kGenBits    = 8u;
inline constexpr uint32_t kGenMask    = (1u << kGenBits) - 1u;   // 0xFF
inline constexpr uint32_t kIndexShift = kGenBits;

[[nodiscard]] constexpr uint32_t GetIndex(Entity e) noexcept
{
    return e >> kIndexShift;
}

[[nodiscard]] constexpr uint8_t GetGeneration(Entity e) noexcept
{
    return static_cast<uint8_t>(e & kGenMask);
}

[[nodiscard]] constexpr Entity MakeEntity(uint32_t index, uint8_t gen) noexcept
{
    return (index << kIndexShift) | static_cast<uint32_t>(gen);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  EntityManager<MaxEntities>
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Manages Entity creation, destruction, and liveness.
 *
 * @tparam MaxEntities  Upper bound on simultaneously live entities.
 *                      Determines the size of the slot array.
 */
template <uint32_t MaxEntities = 65536u>
class EntityManager
{
    static_assert(MaxEntities > 0u && MaxEntities < (1u << 24u),
        "[Aegis] MaxEntities must be in (0, 2^24) to fit in 24-bit index field.");

public:
    using size_type = std::size_t;

    // ── Construction ──────────────────────────────────────────────────────────

    EntityManager() noexcept : free_head_(kNullEntity), alive_count_(0u)
    {
        // Build an intrusive freelist: slot[i].next = i+1
        for (uint32_t i = 0u; i < MaxEntities - 1u; ++i) {
            slots_[i].next_free = i + 1u;
            slots_[i].gen       = 0u;
        }
        slots_[MaxEntities - 1u].next_free = kNullFree;
        slots_[MaxEntities - 1u].gen       = 0u;
        free_head_ = 0u;
    }

    // ── Core API ──────────────────────────────────────────────────────────────

    /**
     * @brief  Allocate a new Entity handle.  O(1), no heap.
     * @return A valid Entity, or kNullEntity if the pool is exhausted.
     */
    [[nodiscard]] Entity Create() noexcept
    {
        if (free_head_ == kNullFree) [[unlikely]] {
            return kNullEntity;   // pool exhausted
        }
        const uint32_t idx = free_head_;
        free_head_         = slots_[idx].next_free;
        ++alive_count_;
        return detail::MakeEntity(idx, slots_[idx].gen);
    }

    /**
     * @brief  Destroy an Entity, incrementing its generation to invalidate stale handles.
     * @pre    IsAlive(e) must be true.
     */
    void Destroy(Entity e) noexcept
    {
        assert(IsAlive(e) && "[Aegis] EntityManager::Destroy() called on dead entity.");
        const uint32_t idx = detail::GetIndex(e);
        ++slots_[idx].gen;               // invalidate all existing handles to idx
        slots_[idx].next_free = free_head_;
        free_head_            = idx;
        --alive_count_;
    }

    /**
     * @brief  Returns true iff the handle was issued by Create() and not yet Destroyed.
     *
     * Checks the generation field — stale handles from before a Destroy() will
     * have a mismatched generation and return false immediately.
     */
    [[nodiscard]] bool IsAlive(Entity e) const noexcept
    {
        if (e == kNullEntity) return false;
        const uint32_t idx = detail::GetIndex(e);
        if (idx >= MaxEntities)         return false;
        return slots_[idx].gen == detail::GetGeneration(e);
    }

    // ── Diagnostics ───────────────────────────────────────────────────────────

    [[nodiscard]] size_type AliveCount()     const noexcept { return alive_count_; }
    [[nodiscard]] size_type Capacity()       const noexcept { return MaxEntities; }
    [[nodiscard]] bool      IsPoolExhausted() const noexcept { return free_head_ == kNullFree; }

private:
    static constexpr uint32_t kNullFree = std::numeric_limits<uint32_t>::max();

    // Each slot is 8 bytes — fits 8 entities per cache line.
    struct Slot {
        uint32_t next_free;  // freelist pointer when dead
        uint8_t  gen;        // generation counter (wraps at 255)
        uint8_t  _pad[3]{};
    };

    alignas(64) Slot slots_[MaxEntities];

    uint32_t  free_head_;
    size_type alive_count_;
};

} // namespace aegis
