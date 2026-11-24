// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Property tests (R7, R8, R15): many random worlds, each drawn from one seed.
 *
 * A world is a stream length, an exchange history, a book size, a reorder
 * window, a recovery policy, rates of loss, duplication and reordering, and a
 * few lost bursts, all picked at random from the world's seed. In every world
 * the handler must keep these properties:
 *
 * - safety: after every arrival its book is the exchange's book as of the
 *   handler's last applied seq: never wrong, at most behind;
 * - progress: its last applied seq never goes down;
 * - no event is ever rejected by the book;
 * - liveness: at the end, after the heartbeat, it has caught up exactly;
 * - determinism: the same world run twice gives the same result.
 *
 * The worlds are seeds FEED_SEED, FEED_SEED + 1, ... (default 1), and
 * there are FEED_SEEDS of them (default 200). A failure names its seed and
 * world; to replay it alone:
 *
 *     FEED_SEED=<seed> FEED_SEEDS=1 ./build/feed_tests --gtest_filter='Property.*'
 */

#include <cstdint>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support.hpp"

using namespace std;

namespace{

uint64_t from_environment(const char* name, uint64_t fallback){
  const char* text = getenv(name);
  return text == nullptr || *text == '\0' ? fallback : strtoull(text, nullptr, 10);
}

/** A world: everything one run depends on, drawn from one seed. */
struct World{
  uint64_t seed;
  uint64_t events;
  size_t history, orders, window;
  Recovery recovery;
  LinkConfig link;

  explicit World(uint64_t world_seed) : seed(world_seed){
    mt19937_64 random(world_seed);
    // Portable draws: mt19937_64 is the same everywhere; the distributions are not.
    const auto below = [&](uint64_t n){ return random() % n; };
    const auto uniform = [&](double hi){ return static_cast<double>(random() >> 11) * 0x1p-53 * hi; };
    events = 1 + below(4'000);
    history = 1 + below(2 * events);
    orders = 1 + below(300);
    window = below(9);
    recovery = below(2) == 0 ? Recovery::every_hole : Recovery::first_hole;
    link.loss = uniform(0.4);
    link.duplicate = uniform(0.2);
    link.reorder = uniform(0.3);
    for (uint64_t bursts = below(3); bursts > 0; --bursts){
      const uint64_t first = 1 + below(events), last = first + below(200);
      link.drop_ranges.push_back({first, last});
    }
    link.seed = world_seed ^ link_salt;
  }

  string describe() const{
    ostringstream text;
    text << "world " << seed << ": " << events << " events, history " << history << ", " << orders
         << " orders, window " << window << ", " << (recovery == Recovery::every_hole ? "every" : "first")
         << "-hole recovery, loss " << link.loss << ", dup " << link.duplicate << ", reorder " << link.reorder << ", "
         << link.drop_ranges.size() << " bursts\n  replay: FEED_SEED=" << seed
         << " FEED_SEEDS=1 ./build/feed_tests --gtest_filter='Property.*'";
    return text.str();
  }
};

/** What a world's run ends with, to compare two runs of it. */
struct Outcome{
  uint64_t fingerprint;
  HandlerStats stats;

  bool operator==(const Outcome& other) const{
    return fingerprint == other.fingerprint && stats.duplicates == other.stats.duplicates &&
           stats.gaps == other.stats.gaps && stats.retransmits == other.stats.retransmits &&
           stats.recovered == other.stats.recovered && stats.snapshots == other.stats.snapshots;
  }
};

/**
 * Runs one world, checking safety and progress after every arrival and
 * liveness at the end.
 *
 * @return the outcome, for the determinism check
 */
Outcome run(const World& world){
  Exchange exchange(world.seed, world.history, world.orders);
  LossyLink link(world.link);
  FeedHandler handler(exchange, world.window, world.recovery);
  vector<uint64_t> truth{0};  // the exchange's book fingerprint after each seq; an empty book is 0
  truth.reserve(world.events + 1);

  uint64_t applied = 0;
  const auto arrive = [&](const Event& event){
    handler.on_event(event);
    ASSERT_GE(handler.last_applied(), applied) << "the handler went backwards";
    applied = handler.last_applied();
    ASSERT_EQ(handler.book().fingerprint(), truth[applied]) << "wrong book at seq " << applied;
  };
  for (uint64_t sent = 0; sent < world.events; ++sent){
    const Event event = exchange.next();
    truth.push_back(exchange.book().fingerprint());
    for (const Event& arrived : link.send(event)){
      arrive(arrived);
      if (testing::Test::HasFatalFailure()){
        return {};
      }
    }
  }
  for (const Event& arrived : link.flush()){
    arrive(arrived);
  }
  handler.on_heartbeat(exchange.last_seq());
  expect_same_book(handler, exchange);
  return {handler.book().fingerprint(), handler.stats()};
}

/** @test T-PR-01 (R7, R8) Safety, progress, no rejection and liveness hold in every random world. */
TEST(Property, HandlerIsNeverWrongAndAlwaysCatchesUp){
  const uint64_t first = from_environment("FEED_SEED", 1), count = from_environment("FEED_SEEDS", 200);
  for (uint64_t seed = first; seed < first + count; ++seed){
    const World world(seed);
    SCOPED_TRACE(world.describe());
    run(world);
    if (HasFailure()){
      return;  // one failing world is enough to replay
    }
  }
}

/** @test T-PR-02 (R15) A world run twice gives the same book and the same counts of everything. */
TEST(Property, SameWorldSameOutcome){
  const uint64_t first = from_environment("FEED_SEED", 1);
  for (uint64_t seed = first; seed < first + 20; ++seed){
    const World world(seed);
    SCOPED_TRACE(world.describe());
    EXPECT_TRUE(run(world) == run(world));
  }
}

}  // namespace
