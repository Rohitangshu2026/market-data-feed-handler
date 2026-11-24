// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Tests for the Lamport clock and the handoff between two handler instances
 * with skewed clocks (R9).
 */

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lamport/lamport.hpp"

using namespace std;

namespace{

/** @test T-LC-01 (R9) Lamport's two rules: a local event adds one; a receive jumps to max(own, stamp) + 1. */
TEST(Lamport, ClockFollowsLamportsRules){
  LamportClock clock;
  EXPECT_EQ(clock.tick(), 1u);
  EXPECT_EQ(clock.receive(10), 11u);  // max(1, 10) + 1
  EXPECT_EQ(clock.receive(3), 12u);   // max(11, 3) + 1
  EXPECT_EQ(clock.tick(), 13u);
}

/** @test T-LC-02 (R9) Merging by Lamport time orders by (stamp, instance); merging by physical time orders by the clocks; ties keep log order. */
TEST(Lamport, MergesOrderByTheirTimestamps){
  // Instance 0's clock runs 10 us ahead; instance 1 took over at seq 3.
  const vector<LogEntry> entries = {{0, 1, 1, 110}, {0, 2, 2, 120}, {1, 3, 4, 105}, {1, 4, 5, 115}};
  const vector<LogEntry> physical = merge_by_physical_time(entries);
  const vector<uint64_t> physical_seqs = {physical[0].seq, physical[1].seq, physical[2].seq, physical[3].seq};
  EXPECT_EQ(physical_seqs, (vector<uint64_t>{3, 1, 4, 2}));
  EXPECT_EQ(count_order_violations(physical), 2u);  // 1 after 3, and 2 after 4

  const vector<LogEntry> lamport = merge_by_lamport_time(entries);
  EXPECT_EQ(count_order_violations(lamport), 0u);

  const vector<LogEntry> tied = merge_by_lamport_time({{1, 5, 7, 0}, {0, 6, 7, 0}});
  EXPECT_EQ(tied[0].instance, 0);  // equal stamps: the lower instance id first
  const vector<LogEntry> same_clock = merge_by_physical_time({{0, 9, 0, 50}, {0, 8, 0, 50}});
  EXPECT_EQ(same_clock[0].seq, 9u);  // fully tied: log order
}

/** @test T-LC-03 (R9) Across handoff latencies of 1-10 ms and skews of 0-25 ms, Lamport time never misorders an update, and physical time misorders one per ms of skew beyond the latency. */
TEST(Lamport, OnlyPhysicalTimeMisordersWhenSkewExceedsTheHandoff){
  for (const int64_t latency_ms : {1, 2, 5, 10}){
    for (int64_t skew_ms = 0; skew_ms <= 25; ++skew_ms){
      const LamportDemoResult result = run_lamport_demo(skew_ms * 1'000, latency_ms * 1'000);
      SCOPED_TRACE("latency " + to_string(latency_ms) + " ms, skew " + to_string(skew_ms) + " ms");
      EXPECT_GT(result.updates, 0u);
      EXPECT_EQ(result.lamport_violations, 0u);
      EXPECT_EQ(result.physical_violations, static_cast<size_t>(max<int64_t>(0, skew_ms - latency_ms)));
    }
  }
}

/** @test T-LC-04 (R9) The handoff needs two events, one per instance, and a message cannot arrive before it is sent. */
TEST(Lamport, HandoffRejectsImpossibleSettings){
  EXPECT_THROW(run_lamport_demo(0, 5'000, 1), invalid_argument);
  EXPECT_THROW(run_lamport_demo(0, -1), invalid_argument);
  EXPECT_NO_THROW(run_lamport_demo(0, 0, 2));
}

/** @test T-LC-05 (R9, R15) The handoff gives the same counts for the same seed, whatever the seed. */
TEST(Lamport, HandoffIsReproducible){
  for (uint64_t seed = 1; seed <= 5; ++seed){
    const LamportDemoResult a = run_lamport_demo(8'000, 5'000, 300, seed), b = run_lamport_demo(8'000, 5'000, 300, seed);
    EXPECT_EQ(a.updates, b.updates);
    EXPECT_EQ(a.physical_violations, b.physical_violations);
    EXPECT_EQ(a.lamport_violations, 0u);
  }
}

}  // namespace
