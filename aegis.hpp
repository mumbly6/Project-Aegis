/**
 * @file   aegis.hpp
 * @brief  Project Aegis — Umbrella Include
 *
 * Single header that pulls in all Aegis subsystems.
 * Include this in application code and benchmarks that need everything.
 *
 *   #include <aegis/aegis.hpp>
 */

#pragma once

// Phase 1 — Memory Foundation
#include <aegis/arena_allocator.hpp>

// Phase 2 — Concurrency Core
#include <aegis/mpmc_queue.hpp>

// Phase 3 — Data-Oriented ECS
#include <aegis/entity_manager.hpp>
#include <aegis/sparse_set.hpp>
#include <aegis/view.hpp>
