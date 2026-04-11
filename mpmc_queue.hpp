/**
 * @file   mpmc_queue.hpp
 * @brief  Project Aegis — Phase 2: Lock-Free MPMC Ring Buffer
 *
 * Algorithm: Dmitry Vyukov's bounded MPMC queue (2010).
 * Reference:  https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
 *
 * Mechanical sympathy guarantees:
 *   1. False-sharing prevention.
 *      `head_` and `tail_` are on distinct cache lines (alignas(64)).
 *      Each `Slot` is cache-line padded so that a producer writing to slot N
 *      does not evict the sequence counter in slot N+1 from another thread's L1.
 *
 *   2. Minimal fence cost.
 *      Only `acquire` and `release` fences are used.  There is zero
 *      `memory_order_seq_cst` (which would flush the store buffer and drain
 *      the load queue on x86 via MFENCE — very expensive).
 *
 *   3. No kernel involvement.
 *      No futex, no condvar, no mutex.  Threads spin on the CAS loop.
 *      For high-throughput task queues the spin is cheaper than park/unpark.
 *
 * Safety constraints:
 *   - Capacity MUST be a power of two (enforced by static_assert).
 *   - T must be move-assignable and default-constructible.
 *   - Not safe to use after destruction if threads are still running.
 *
 * Usage:
 *   aegis::MpmcQueue<Task, 1024> queue;
 *   // Producer thread
 *   while (!queue.TryEnqueue(std::move(task))) { /* spin or yield *\/ }
 *   // Consumer thread
 *   Task t;
 *   if (queue.TryDequeue(t)) { process(t); }
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>          // std::hardware_destructive_interference_size
#include <type_traits>
#include <utility>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
//  Cache line size constant (same helper used in arena_allocator.hpp)
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline constexpr std::size_t kCacheLine =
#if defined(__cpp_lib_hardware_interference_size)
    std::hardware_destructive_interference_size;
#else
    64u;
#endif

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  MpmcQueue<T, Capacity>
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Lock-Free Multi-Producer / Multi-Consumer bounded ring buffer.
 *
 * @tparam T         Element type.  Must be move-assignable + default-constructible.
 * @tparam Capacity  Ring buffer size.  MUST be a power of two.
 */
template <typename T, std::size_t Capacity>
class MpmcQueue
{
    static_assert(Capacity > 1u,
        "[Aegis] MpmcQueue Capacity must be greater than 1.");
    static_assert((Capacity & (Capacity - 1u)) == 0u,
        "[Aegis] MpmcQueue Capacity must be a power of two "
        "(enables fast modulo via bitwise AND instead of integer division).");
    static_assert(std::is_default_constructible_v<T>,
        "[Aegis] MpmcQueue element type T must be default-constructible.");
    static_assert(std::is_move_assignable_v<T>,
        "[Aegis] MpmcQueue element type T must be move-assignable.");

public:
    // ── Types ─────────────────────────────────────────────────────────────────

    using value_type = T;
    using size_type  = std::size_t;

    // ── Constants ─────────────────────────────────────────────────────────────

    static constexpr size_type kCapacity = Capacity;
    static constexpr size_type kMask     = Capacity - 1u;  // bitmask for fast mod

    // ── Construction ──────────────────────────────────────────────────────────

    MpmcQueue() noexcept
    {
        // Initialise each slot's sequence to its index.
        // A slot with sequence == index is "empty and ready for a producer."
        for (size_type i = 0u; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
        head_.store(0u, std::memory_order_relaxed);
        tail_.store(0u, std::memory_order_relaxed);
    }

    // Queues are not copyable or movable — they contain atomic members
    // and live references in flight from other threads.
    MpmcQueue(const MpmcQueue&)            = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;
    MpmcQueue(MpmcQueue&&)                 = delete;
    MpmcQueue& operator=(MpmcQueue&&)      = delete;

    ~MpmcQueue() = default;

    // ── Core API ──────────────────────────────────────────────────────────────

    /**
     * @brief  Non-blocking enqueue.  Returns false if the queue is full.
     *
     * Memory ordering anatomy:
     *   tail_.load(relaxed)           — read current tail; no fence needed yet.
     *   slot.sequence.load(acquire)   — synchronise with the consumer's
     *                                   release store that freed this slot.
     *   tail_.compare_exchange_weak(relaxed)  — claim the slot; the actual
     *                                           data write and the sequence
     *                                           release below are the fences.
     *   slot.data = move(item)        — write the payload.
     *   slot.sequence.store(release)  — publish to consumers; pairs with the
     *                                   consumer's acquire load in TryDequeue.
     */
    [[nodiscard]] bool TryEnqueue(T item) noexcept
    {
        size_type pos = tail_.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = slots_[pos & kMask];
            const size_type seq = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq)
                            - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                // Slot is free; try to claim it.
                if (tail_.compare_exchange_weak(pos, pos + 1u,
                                                std::memory_order_relaxed)) {
                    break;   // we own this slot
                }
                // Another producer raced us; re-read tail and retry.
            } else if (diff < 0) {
                // seq < pos → queue is full.
                return false;
            } else {
                // Another producer is mid-write on a recycled slot; re-read.
                pos = tail_.load(std::memory_order_relaxed);
            }
        }

        // We own the slot.  Write the payload then publish.
        slots_[pos & kMask].data = std::move(item);
        slots_[pos & kMask].sequence.store(pos + 1u, std::memory_order_release);
        return true;
    }

    /**
     * @brief  Non-blocking dequeue.  Returns false if the queue is empty.
     *
     * Memory ordering anatomy:
     *   head_.load(relaxed)           — read current head.
     *   slot.sequence.load(acquire)   — synchronise with the producer's
     *                                   release store that published this slot.
     *   head_.compare_exchange_weak(relaxed)  — claim the slot.
     *   item = move(slot.data)        — read the payload.
     *   slot.sequence.store(release)  — free the slot for producers;
     *                                   store = pos + Capacity (reuse cycle).
     */
    [[nodiscard]] bool TryDequeue(T& item) noexcept
    {
        size_type pos = head_.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = slots_[pos & kMask];
            const size_type seq = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq)
                            - static_cast<std::intptr_t>(pos + 1u);

            if (diff == 0) {
                // Slot has data; try to claim it.
                if (head_.compare_exchange_weak(pos, pos + 1u,
                                                std::memory_order_relaxed)) {
                    break;    // we own this slot
                }
                // Another consumer raced us; re-read head and retry.
            } else if (diff < 0) {
                // seq < pos+1 → queue is empty.
                return false;
            } else {
                // Another consumer moved head; re-read.
                pos = head_.load(std::memory_order_relaxed);
            }
        }

        // We own the slot.  Read payload then free the slot.
        item = std::move(slots_[pos & kMask].data);
        // Advance sequence by Capacity to signal "empty, ready for next cycle."
        slots_[pos & kMask].sequence.store(pos + Capacity, std::memory_order_release);
        return true;
    }

    // ── Diagnostics ───────────────────────────────────────────────────────────

    /**
     * @brief  Approximate size.  Not exact under concurrent access —
     *         safe for logging/metrics, not for correctness decisions.
     */
    [[nodiscard]] size_type SizeApprox() const noexcept
    {
        const size_type h = head_.load(std::memory_order_relaxed);
        const size_type t = tail_.load(std::memory_order_relaxed);
        return (t >= h) ? (t - h) : 0u;
    }

    [[nodiscard]] bool EmptyApprox() const noexcept { return SizeApprox() == 0u; }

private:
    // ── Slot ──────────────────────────────────────────────────────────────────

    /// Each slot occupies exactly one cache line.
    ///
    /// Why: if Slot were smaller (e.g., just atomic + int32_t = 12 bytes),
    /// multiple slots would share a cache line.  Thread A writing to slot 0's
    /// sequence would invalidate Thread B's copy of slot 1's sequence in L1 —
    /// classic false sharing.  The padding eliminates this entirely.
    struct alignas(detail::kCacheLine) Slot {
        std::atomic<size_type> sequence{0};
        T                      data{};

        // Pad to fill the rest of the cache line if T is small.
        // This ensures slot[N] and slot[N+1] never share a cache line.
        static constexpr size_type kPayload =
            sizeof(std::atomic<size_type>) + sizeof(T);
        static constexpr size_type kPadding =
            (kPayload < detail::kCacheLine)
                ? (detail::kCacheLine - kPayload)
                : 0u;

        [[maybe_unused]] char _pad[kPadding]{};
    };

    // ── Members ───────────────────────────────────────────────────────────────

    /// Ring buffer of padded slots.
    Slot slots_[Capacity];

    /// Write cursor — producers race to increment this.
    /// alignas(kCacheLine): sits on its own cache line, isolated from `slots_`
    /// and from `head_`.  A producer CAS on tail_ does NOT evict head_ from
    /// a consumer's L1 cache.
    alignas(detail::kCacheLine) std::atomic<size_type> head_{0};
    alignas(detail::kCacheLine) std::atomic<size_type> tail_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Convenience aliases
// ─────────────────────────────────────────────────────────────────────────────

/// 4096-slot task queue — large enough for burst bursts of 4K tasks per frame.
template <typename T>
using DefaultTaskQueue = MpmcQueue<T, 4096>;

} // namespace aegis
