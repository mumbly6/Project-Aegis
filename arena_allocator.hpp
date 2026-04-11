/**
 * @file   arena_allocator.hpp
 * @brief  Project Aegis — Phase 1: Thread-Local Linear Arena Allocator
 *
 * Design goals (CppCon narrative):
 *   • Zero heap allocations in the hot path  — Allocate() is a pointer bump.
 *   • Cache-line aligned backing buffer      — alignas(64) prevents false sharing.
 *   • O(1) free                             — Reset() moves one pointer.
 *   • No stdlib containers inside logic      — raw pointers + std::byte only.
 *   • Full C++20: concepts, [[nodiscard]],   std::hardware_destructive_interference_size.
 *
 * Usage:
 *   aegis::ArenaAllocator<64 * 1024 * 1024> arena;   // 64 MiB stack arena
 *   float* xs = arena.Allocate<float>(1'000'000);    // bump pointer, no syscall
 *   arena.Reset();                                    // O(1) — frame reuse
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>          // std::hardware_destructive_interference_size
#include <type_traits>
#include <utility>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Round @p value up to the next multiple of @p alignment.
/// alignment MUST be a power of two (verified by caller static_assert).
[[nodiscard]] constexpr std::size_t
AlignUp(std::size_t value, std::size_t alignment) noexcept
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/// True iff @p x is a power of two (and non-zero).
[[nodiscard]] constexpr bool IsPowerOfTwo(std::size_t x) noexcept
{
    return x != 0u && (x & (x - 1u)) == 0u;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  ArenaAllocator<Capacity>
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  A linear (bump-pointer) arena allocator backed by a fixed-size buffer.
 *
 * @tparam Capacity  Size of the backing buffer in bytes.
 *                   Must be > 0 and ideally a multiple of 64 (cache line size).
 *
 * Thread safety: NOT thread-safe by design.
 *               Intended use: one arena per worker thread (thread-local storage).
 *               Each thread owns its arena exclusively — no synchronisation needed.
 */
template <std::size_t Capacity>
class ArenaAllocator
{
    static_assert(Capacity > 0,
        "[Aegis] ArenaAllocator capacity must be greater than zero.");
    static_assert(Capacity % 64 == 0,
        "[Aegis] ArenaAllocator capacity should be a multiple of 64 bytes "
        "(cache line size) to avoid partial line waste.");

public:
    // ── Types ─────────────────────────────────────────────────────────────────

    using size_type = std::size_t;

    // ── Constants ─────────────────────────────────────────────────────────────

    /// Cache line size — used externally to place arenas without false sharing.
    static constexpr size_type kCacheLineSize =
#if defined(__cpp_lib_hardware_interference_size)
        std::hardware_destructive_interference_size;
#else
        64u;   // conservative fallback (x86-64, ARM64)
#endif

    static constexpr size_type kCapacity = Capacity;

    // ── Construction ──────────────────────────────────────────────────────────

    ArenaAllocator() noexcept : offset_(0u) {}

    // Arenas are not copyable — owning a unique memory region.
    ArenaAllocator(const ArenaAllocator&)            = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    // Move is allowed (e.g. initial placement in thread-local storage).
    ArenaAllocator(ArenaAllocator&&) noexcept            = default;
    ArenaAllocator& operator=(ArenaAllocator&&) noexcept = default;

    ~ArenaAllocator() = default;

    // ── Core API ──────────────────────────────────────────────────────────────

    /**
     * @brief  Allocate storage for @p count objects of type T.
     *
     * This is the hot-path function.  The compiler inlines it to:
     *     ptr = buffer_ + offset_;   offset_ += sizeof(T)*count;
     * — a single addition and a bounds check.  Zero heap, zero syscall.
     *
     * @return  Pointer to aligned, uninitialized storage.
     *          Returns nullptr if there is insufficient capacity.
     *
     * @note  Objects are NOT constructed.  Use placement-new if needed.
     *        The returned storage is valid until Reset() is called.
     */
    template <typename T>
    [[nodiscard]] T* Allocate(size_type count = 1u) noexcept
    {
        static_assert(!std::is_void_v<T>,
            "[Aegis] Allocate<void>() is ill-formed; use AllocateRaw().");
        static_assert(alignof(T) <= Capacity,
            "[Aegis] Requested alignment exceeds arena capacity.");

        return static_cast<T*>(AllocateRaw(sizeof(T) * count, alignof(T)));
    }

    /**
     * @brief  Low-level raw allocation with explicit alignment control.
     *
     * @param bytes      Number of bytes to allocate.
     * @param alignment  Required alignment.  Must be a power of two.
     * @return           Aligned pointer into the arena, or nullptr on OOM.
     */
    [[nodiscard]] void* AllocateRaw(size_type bytes,
                                    size_type alignment = alignof(std::max_align_t)) noexcept
    {
        assert(detail::IsPowerOfTwo(alignment) &&
               "[Aegis] alignment must be a power of two.");

        const size_type aligned_offset = detail::AlignUp(offset_, alignment);
        const size_type new_offset     = aligned_offset + bytes;

        if (new_offset > Capacity) [[unlikely]] {
            // Arena is full.  In production, this surfaces immediately in
            // benchmark runs — making OOM a hard, visible error.
            return nullptr;
        }

        offset_ = new_offset;
        return static_cast<void*>(buffer_ + aligned_offset);
    }

    /**
     * @brief  Reset the arena to its initial state.
     *
     * O(1) — moves the offset pointer back to zero.
     * Does NOT zero the backing memory (intentional: we rely on overwrite).
     * Safe to call every simulation frame for frame-scoped allocations.
     */
    void Reset() noexcept
    {
        offset_ = 0u;
    }

    // ── Diagnostics ───────────────────────────────────────────────────────────

    /// Bytes consumed since last Reset().
    [[nodiscard]] size_type Used() const noexcept { return offset_; }

    /// Bytes remaining before the arena is full.
    [[nodiscard]] size_type Remaining() const noexcept
    {
        return Capacity - offset_;
    }

    /// Fraction [0.0, 1.0] of the arena that is currently occupied.
    [[nodiscard]] double Utilization() const noexcept
    {
        return static_cast<double>(offset_) / static_cast<double>(Capacity);
    }

private:
    // ── Storage ───────────────────────────────────────────────────────────────

    /// Backing store.
    ///
    /// alignas(64): aligns the buffer to a cache line boundary so that:
    ///   1. The first Allocate() call always returns a cache-line-aligned pointer.
    ///   2. If multiple ArenaAllocators live in the same struct/array, each
    ///      starts on its own line, preventing false sharing between threads.
    alignas(64) std::byte buffer_[Capacity];

    /// Current write cursor.  Stored separately from buffer_ to avoid
    /// polluting the buffer's cache line during non-allocation reads.
    size_type offset_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Convenience aliases used by later phases
// ─────────────────────────────────────────────────────────────────────────────

/// 64 MiB arena — typical per-thread budget for 1M-entity simulations.
using MediumArena = ArenaAllocator<64u * 1024u * 1024u>;

/// 256 MiB arena — used by the ECS component pools in Phase 3.
using LargeArena  = ArenaAllocator<256u * 1024u * 1024u>;

} // namespace aegis
