/**
 * @file   bench_ecs.cpp
 * @brief  Project Aegis — Phase 3 Benchmarks: OOP virtual dispatch vs ECS SparseSet View
 *
 * Scenario: 1,000,000 entities each having a 2-D position and velocity.
 *   Every benchmark "frame" applies:    position += velocity * dt
 *
 * BM_OOP_VirtualUpdate  — Classic OOP: std::vector of pointers to a base class.
 *                          update() is virtual → one vtable dereference per entity.
 *                          Heap-scattered objects → continuous cache misses.
 *
 * BM_ECS_ViewUpdate     — Aegis ECS: EntityManager + two SparseSets.
 *                          MakeView(positions, velocities) drives a linear sweep
 *                          over the dense component arrays.  No virtual calls.
 *                          CPU prefetcher sees a stride-1 access pattern.
 *
 * BM_ECS_DirectSweep    — Lower bound: direct pointer sweep over SparseSet::Data()
 *                          (bypasses even the View iterator — measures the hardware ceiling).
 */

#include <aegis/entity_manager.hpp>
#include <aegis/sparse_set.hpp>
#include <aegis/view.hpp>

#include <benchmark/benchmark.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Shared constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t    kEntityCount = 1'000'000u;
static constexpr uint32_t    kCapacity    = kEntityCount;
static constexpr float       kDt          = 0.016f;

// ─────────────────────────────────────────────────────────────────────────────
//  Component types (plain aggregates — no vtable, no heap)
// ─────────────────────────────────────────────────────────────────────────────

struct Position { float x, y; };
struct Velocity { float x, y; };

// ─────────────────────────────────────────────────────────────────────────────
//  OOP Baseline: virtual dispatch over heap-scattered objects
// ─────────────────────────────────────────────────────────────────────────────

struct IGameObject {
    float px, py, vx, vy;
    virtual ~IGameObject() = default;
    virtual void Update(float dt) noexcept = 0;
};

struct MovingObject final : IGameObject {
    void Update(float dt) noexcept override {
        px += vx * dt;
        py += vy * dt;
        vx = std::sin(px);
        vy = std::cos(py);
    }
};

static void BM_OOP_VirtualUpdate(benchmark::State& state)
{
    // Build heap-scattered array (shape: AoP — Array of Pointers)
    std::vector<std::unique_ptr<IGameObject>> objects;
    objects.reserve(kEntityCount);
    for (uint32_t i = 0; i < kEntityCount; ++i) {
        auto o = std::make_unique<MovingObject>();
        o->px = static_cast<float>(i) * 0.001f;
        o->py = static_cast<float>(i) * 0.002f;
        o->vx = 1.0f;
        o->vy = 0.5f;
        objects.push_back(std::move(o));
    }

    for (auto _ : state) {
        for (auto& obj : objects) {
            obj->Update(kDt);            // vtable lookup → indirect branch
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * kEntityCount);
    state.SetLabel("OOP | heap AoP | virtual dispatch");
}

// ─────────────────────────────────────────────────────────────────────────────
//  ECS: MakeView over two SparseSets
// ─────────────────────────────────────────────────────────────────────────────

// Global storage declared at file scope to avoid stack overflow from large
// SparseSet fixed arrays.  In a real engine these live in the World object.
static aegis::SparseSet<Position, kCapacity> g_positions;
static aegis::SparseSet<Velocity, kCapacity> g_velocities;

/// Populate the SparseSets once (simulates world initialisation).
static bool g_initialised = false;
static void EnsureECSWorld()
{
    if (g_initialised) return;
    aegis::EntityManager<kCapacity> mgr;
    for (uint32_t i = 0; i < kEntityCount; ++i) {
        aegis::Entity e = mgr.Create();
        const uint32_t idx = aegis::detail::GetIndex(e);
        g_positions.Emplace(idx, Position{static_cast<float>(i) * 0.001f,
                                          static_cast<float>(i) * 0.002f});
        g_velocities.Emplace(idx, Velocity{1.0f, 0.5f});
    }
    g_initialised = true;
}

/**
 * Benchmark: ECS View-driven update.
 *
 * MakeView internally selects the smallest of the two SparseSets as the
 * driving iterator.  With kEntityCount entities in both sets, both are equal
 * and the first is chosen.  The loop is a linear dense-array scan.
 */
static void BM_ECS_ViewUpdate(benchmark::State& state)
{
    EnsureECSWorld();
    auto view = aegis::MakeView(g_positions, g_velocities);

    for (auto _ : state) {
        for (aegis::Entity e : view) {
            auto& [pos, vel] = view.Get(e);
            pos.x += vel.x * kDt;
            pos.y += vel.y * kDt;
            vel.x = std::sin(pos.x);
            vel.y = std::cos(pos.y);
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * kEntityCount);
    state.SetLabel("ECS | SparseSet View | direct call");
}

/**
 * Benchmark: hardware ceiling.
 *
 * Directly indexes SparseSet::Data() — the absolute minimum overhead.
 * This tells us how close the View abstraction gets to the hardware limit.
 */
static void BM_ECS_DirectSweep(benchmark::State& state)
{
    EnsureECSWorld();

    Position* pos = g_positions.Data();
    Velocity* vel = g_velocities.Data();
    const std::size_t n = g_positions.Size();

    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            pos[i].x += vel[i].x * kDt;
            pos[i].y += vel[i].y * kDt;
            vel[i].x = std::sin(pos[i].x);
            vel[i].y = std::cos(pos[i].y);
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
    state.SetLabel("ECS | direct Data() sweep | hardware ceiling");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Registration
// ─────────────────────────────────────────────────────────────────────────────

BENCHMARK(BM_OOP_VirtualUpdate)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3);

BENCHMARK(BM_ECS_ViewUpdate)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3);

BENCHMARK(BM_ECS_DirectSweep)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3);

BENCHMARK_MAIN();
