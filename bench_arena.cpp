/**
 * @file   bench_arena.
 * @brief  Project Aegis — Phase 1 Benchmarks
 *
 * Proves the cache-locality and zero-allocation advantages of Data-Oriented
 * Design (DOD) over traditional Object-Oriented (OOP) patterns.
 *
 * Two scenarios, each updating 1,000,000 2-D positions every benchmark frame:
 *
 *   BM_OOP_HeapAoS  — std::vector<std::unique_ptr<Entity>>
 *                     Array-of-Pointers → each dereference is a potential
 *                     cache miss.  Heap allocations scatter objects in memory.
 *
 *   BM_DOD_ArenaSOA — Two contiguous float arrays (x[], y[]) bumped from an
 *                     ArenaAllocator.  CPU prefetcher loads data linearly;
 *                     zero heap calls after arena setup.
 */

#include <aegis/arena_allocator.hpp>

#include <benchmark/benchmark.h>

#include <cmath>       // std::sin, std::cos
#include <cstdint>
#include <memory>      // std::make_unique
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr std::size_t kEntityCount = 1'000'000;

// ─────────────────────────────────────────────────────────────────────────────
//  OOP Baseline: Array of Pointers (heap-allocated, scattered)
// ─────────────────────────────────────────────────────────────────────────────

/// Classic OOP entity.  Virtual Update() forces an indirect call + vtable
/// lookup, destroying prefetcher predictions.
struct OopEntity {
    float x, y;
    float vx, vy;

    virtual ~OopEntity() = default;

    virtual void Update(float dt) noexcept
    {
        x += vx * dt;
        y += vy * dt;
        // A tiny trig op gives the CPU actual work, preventing dead-code
        // elimination while still being representatively "cheap" physics.
        vx = std::sin(static_cast<float>(x));
        vy = std::cos(static_cast<float>(y));
    }
};

/**
 * Benchmark: OOP heap Array-of-Structures.
 *
 * Every iteration:
 *   1. Allocates kEntityCount objects on the heap (setup, amortised).
 *   2. Loops over a vector of unique_ptr — each dereference follows a
 *      pointer to a (potentially non-adjacent) heap block → cache miss.
 */
static void BM_OOP_HeapAoS(benchmark::State& state)
{
    // ── Setup (outside timed region) ─────────────────────────────────────────
    std::vector<std::unique_ptr<OopEntity>> entities;
    entities.reserve(kEntityCount);

    for (std::size_t i = 0; i < kEntityCount; ++i) {
        auto e  = std::make_unique<OopEntity>();
        e->x    = static_cast<float>(i) * 0.001f;
        e->y    = static_cast<float>(i) * 0.002f;
        e->vx   = 1.0f;
        e->vy   = 0.5f;
        entities.push_back(std::move(e));
    }

    constexpr float kDt = 0.016f;  // ~60 Hz

    // ── Timed region ─────────────────────────────────────────────────────────
    for (auto _ : state) {
        for (auto& entity : entities) {
            entity->Update(kDt);    // vtable dispatch → indirect branch
        }
        // Prevent compiler from eliminating the entire loop.
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(kEntityCount));
    state.SetLabel("AoS | heap | virtual dispatch");
}

// ─────────────────────────────────────────────────────────────────────────────
//  DOD Optimised: Structure of Arrays via ArenaAllocator (zero-heap hot path)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Benchmark: DOD Structure-of-Arrays backed by ArenaAllocator.
 *
 * Layout in memory:
 *   [x0 x1 x2 … x999999] [y0 y1 y2 … y999999]
 *                         ↑ contiguous, cache-resident
 *
 * Every iteration:
 *   1. Bumps the arena pointer N times total (O(1) per bump, no syscall).
 *   2. Streams linearly through the x[] then y[] arrays.
 *      The hardware prefetcher recognises the stride and pre-fetches L2→L1
 *      before the CPU needs it — eliminating the cache miss penalty.
 *   3. Resets the arena in O(1) at end of each "frame".
 */
static void BM_DOD_ArenaSOA(benchmark::State& state)
{
    // 4 float arrays × 1M × 4 bytes = 16 MiB. Keep within MediumArena (64 MiB).
    aegis::MediumArena arena;

    constexpr float kDt = 0.016f;

    // ── Timed region ─────────────────────────────────────────────────────────
    for (auto _ : state) {
        // Allocate four tightly-packed SoA streams from the arena.
        // In a real ECS these live permanently; here we re-allocate each
        // iteration to demonstrate the O(1) bump cost explicitly.
        float* xs = arena.Allocate<float>(kEntityCount);
        float* ys = arena.Allocate<float>(kEntityCount);
        float* vxs = arena.Allocate<float>(kEntityCount);
        float* vys = arena.Allocate<float>(kEntityCount);

        if (!xs || !ys || !vxs || !vys) {
            state.SkipWithError("ArenaAllocator OOM — increase arena capacity.");
            return;
        }

        // Initialise (in a real engine this is done once, not per frame).
        for (std::size_t i = 0; i < kEntityCount; ++i) {
            xs[i]  = static_cast<float>(i) * 0.001f;
            ys[i]  = static_cast<float>(i) * 0.002f;
            vxs[i] = 1.0f;
            vys[i] = 0.5f;
        }

        // ── Hot path: linear sweep, no pointer chasing ────────────────────
        //
        // A modern x86-64 CPU can prefetch and potentially auto-vectorise
        // (AVX2: 8 floats per instruction) this loop because:
        //   • xs, ys, vxs, vys are all contiguous.
        //   • No aliasing across streams (separate pointers, same arena).
        //   • No virtual dispatch or indirect calls.
        for (std::size_t i = 0; i < kEntityCount; ++i) {
            xs[i]  += vxs[i] * kDt;
            ys[i]  += vys[i] * k
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(kEntityCount));
    state.SetLabel("SoA | arena | direct call");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Bonus: Arena Allocate+Reset cost in isolation
//  Confirms that Reset() is genuinely O(1) and carries no hidden cost.
// ─────────────────────────────────────────────────────────────────────────────
static void BM_Arena_AllocReset(benchmark::State& state)
{
    aegis::MediumArena arena;

    for (auto _ : state) {
        float* p = arena.Allocate<float>(kEntityCount);
        benchmark::DoNotOptimize(p);
        arena.Reset();
    }

    state.SetLabel("Arena bump-alloc + Reset cost");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Registration
// ─────────────────────────────────────────────────────────────────────────────

BENCHMARK(BM_OOP_HeapAoS)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3);

BENCHMARK(BM_DOD_ArenaSOA)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3);

BENCHMARK(BM_Arena_AllocReset)
    ->Unit(benchmark::kMicrosecond)
    ->Repetitions(3);

BENCHMARK_MAIN();
