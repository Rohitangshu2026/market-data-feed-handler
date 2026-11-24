// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Tests for the feed handler (R1-R8), in two groups.
 *
 * Structural tests hand the handler events in an exact order, so that each
 * branch of on_event(), recover(), resync() and on_heartbeat() runs and its
 * effect is checked: the fast path, both kinds of duplicate, a late event
 * closing a gap, the reorder window, both recovery policies, the fall back
 * to a snapshot, and the heartbeat with and without a gap open.
 *
 * Functional tests drive whole streams through the lossy link, one per
 * requirement, and end by comparing the handler's book with the exchange's.
 */

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support.hpp"

using namespace std;

namespace{

/** Hands the handler the published events with these seqs, in this order. */
void deliver(FeedHandler& handler, const vector<Event>& events, initializer_list<uint64_t> seqs){
  for (const uint64_t seq : seqs){
    handler.on_event(events.at(seq - 1));
  }
}

void deliver_range(FeedHandler& handler, const vector<Event>& events, uint64_t first, uint64_t last){
  for (uint64_t seq = first; seq <= last; ++seq){
    handler.on_event(events.at(seq - 1));
  }
}

// ---- structural: each path through the handler ----

/** @test T-FH-01 (R1) An event in sequence with nothing waiting is applied at once, and nothing is buffered. */
TEST(FeedHandlerPaths, InOrderEventIsAppliedAtOnce){
  Exchange exchange(1, 100);
  const vector<Event> events = publish(exchange, 10);
  FeedHandler handler(exchange);
  for (uint64_t seq = 1; seq <= 10; ++seq){
    handler.on_event(events[seq - 1]);
    ASSERT_EQ(handler.last_applied(), seq);
    ASSERT_EQ(handler.buffered(), 0u);
  }
  expect_same_book(handler, exchange);
  EXPECT_EQ(handler.stats().gaps, 0u);
}

/** @test T-FH-02 (R2) A copy of an event already applied is dropped and counted. */
TEST(FeedHandlerPaths, CopyOfAnAppliedEventIsDropped){
  Exchange exchange(1, 100);
  const vector<Event> events = publish(exchange, 5);
  FeedHandler handler(exchange);
  deliver(handler, events, {1, 2, 2, 3, 1, 4, 5, 5});
  expect_same_book(handler, exchange);
  EXPECT_EQ(handler.stats().duplicates, 3u);
}

/** @test T-FH-03 (R2, R3) A copy of an event still waiting in the buffer is dropped; the original is applied once. */
TEST(FeedHandlerPaths, CopyOfABufferedEventIsDropped){
  Exchange exchange(1, 100);
  const vector<Event> events = publish(exchange, 5);
  FeedHandler handler(exchange, 3);
  deliver(handler, events, {1, 3, 3});
  EXPECT_EQ(handler.buffered(), 1u);
  EXPECT_EQ(handler.stats().duplicates, 1u);
  deliver(handler, events, {2, 4, 5});
  expect_same_book(handler, exchange);
}

/** @test T-FH-04 (R3) An early event waits; the late one closes the gap and both are applied in order, with no recovery. */
TEST(FeedHandlerPaths, LateEventClosesTheGap){
  Exchange exchange(1, 100);
  const vector<Event> events = publish(exchange, 6);
  FeedHandler handler(exchange, 3);
  deliver(handler, events, {1, 2, 4, 5});
  EXPECT_EQ(handler.last_applied(), 2u);
  EXPECT_EQ(handler.buffered(), 2u);
  deliver(handler, events, {3});
  EXPECT_EQ(handler.last_applied(), 5u);
  EXPECT_EQ(handler.buffered(), 0u);
  deliver(handler, events, {6});
  expect_same_book(handler, exchange);
  EXPECT_EQ(handler.stats().gaps, 1u);  // counted once, however many events wait behind it
  EXPECT_EQ(handler.stats().retransmits, 0u);
}

/** @test T-FH-05 (R3, R4) The reorder window W is exact: a gap survives the arrival that opens it and W-1 more, and the next arrival recovers it. Checked for W from 0 to 8. */
TEST(FeedHandlerPaths, ReorderWindowGivesAGapExactlyWArrivals){
  for (const size_t window : {0u, 1u, 2u, 3u, 5u, 8u}){
    SCOPED_TRACE("window " + to_string(window));
    Exchange exchange(1, 100);
    const vector<Event> events = publish(exchange, 30);
    FeedHandler handler(exchange, window);
    deliver_range(handler, events, 1, 5);  // seq 6 never arrives
    deliver_range(handler, events, 7, 6 + window);
    EXPECT_EQ(handler.stats().retransmits, 0u);
    EXPECT_EQ(handler.buffered(), window);
    deliver(handler, events, {7 + window});
    EXPECT_EQ(handler.stats().retransmits, 1u);
    EXPECT_EQ(handler.stats().recovered, 1u);
    EXPECT_EQ(handler.last_applied(), 7 + window);
    EXPECT_EQ(handler.buffered(), 0u);
  }
}

/** @test T-FH-06 (R4, R8) Every-hole recovery fetches every hole behind the window at once; first-hole recovery fetches only the oldest, and each later hole waits out a window of its own. Both end with the right book. */
TEST(FeedHandlerPaths, RecoveryPoliciesDifferOnlyInWhenLaterHolesAreFetched){
  for (const Recovery recovery : {Recovery::every_hole, Recovery::first_hole}){
    const bool every = recovery == Recovery::every_hole;
    SCOPED_TRACE(every ? "every hole" : "first hole");
    Exchange exchange(1, 100);
    const vector<Event> events = publish(exchange, 20);
    FeedHandler handler(exchange, 3, recovery);
    deliver(handler, events, {1, 2, 4, 6, 8, 9});  // holes at 3, 5 and 7; the 4th arrival recovers
    EXPECT_EQ(handler.last_applied(), every ? 9u : 4u);
    EXPECT_EQ(handler.buffered(), every ? 0u : 3u);
    EXPECT_EQ(handler.stats().retransmits, every ? 3u : 1u);
    deliver_range(handler, events, 10, 20);
    expect_same_book(handler, exchange);
    EXPECT_EQ(handler.stats().retransmits, 3u);
    EXPECT_EQ(handler.stats().recovered, 3u);
  }
}

/** @test T-FH-07 (R5) When the exchange can no longer retransmit the missing events, the handler takes a snapshot and drops the buffered events it covers. */
TEST(FeedHandlerPaths, RefusedRetransmissionFallsBackToASnapshot){
  Exchange exchange(1, 5);  // keeps seqs 16-20 only
  const vector<Event> events = publish(exchange, 20);
  FeedHandler handler(exchange, 0);
  deliver(handler, events, {1, 2, 3, 11});  // 4-10 are gone from the history
  EXPECT_EQ(handler.stats().snapshots, 1u);
  EXPECT_EQ(handler.stats().retransmits, 0u);
  EXPECT_EQ(handler.last_applied(), 20u);
  EXPECT_EQ(handler.buffered(), 0u);
  deliver(handler, events, {12, 20});  // now old: dropped as duplicates
  EXPECT_EQ(handler.stats().duplicates, 2u);
  expect_same_book(handler, exchange);
}

/** @test T-FH-08 (R2, R4) The original of an event already recovered arrives after all, and is dropped. */
TEST(FeedHandlerPaths, LateOriginalOfARecoveredEventIsDropped){
  Exchange exchange(1, 100);
  const vector<Event> events = publish(exchange, 10);
  FeedHandler handler(exchange, 3);
  deliver(handler, events, {1, 3, 4, 5, 6});  // 6 is the 4th arrival: 2 is retransmitted
  EXPECT_EQ(handler.stats().retransmits, 1u);
  deliver(handler, events, {2, 7, 8, 9, 10});
  EXPECT_EQ(handler.stats().duplicates, 1u);
  expect_same_book(handler, exchange);
}

/** @test T-FH-09 (R6) A heartbeat that finds nothing missing does nothing. */
TEST(FeedHandlerPaths, HeartbeatWithNothingMissingDoesNothing){
  Exchange exchange(1, 100);
  const vector<Event> events = publish(exchange, 10);
  FeedHandler handler(exchange);
  deliver_range(handler, events, 1, 10);
  handler.on_heartbeat(10);
  handler.on_heartbeat(7);  // an old heartbeat
  EXPECT_EQ(handler.stats().gaps, 0u);
  EXPECT_EQ(handler.stats().retransmits, 0u);
  expect_same_book(handler, exchange);
}

/** @test T-FH-10 (R6) A heartbeat that arrives while a gap is open recovers up to the exchange's latest seq; the buffered copies are dropped, and the gap is not counted twice. */
TEST(FeedHandlerPaths, HeartbeatDuringAGapRecoversEverything){
  Exchange exchange(1, 100);
  const vector<Event> events = publish(exchange, 10);
  FeedHandler handler(exchange, 3);
  deliver(handler, events, {1, 2, 4, 5});
  handler.on_heartbeat(exchange.last_seq());
  EXPECT_EQ(handler.stats().gaps, 1u);
  EXPECT_EQ(handler.stats().retransmits, 1u);
  EXPECT_EQ(handler.stats().recovered, 8u);  // 3 to 10
  expect_same_book(handler, exchange);
}

/** @test T-FH-11 (R5) A handler that joins late starts from a snapshot, then follows the stream. */
TEST(FeedHandlerPaths, LateJoinStartsFromASnapshot){
  Exchange exchange(6, 1'000);
  publish(exchange, 500);
  LossyLink link({});
  FeedHandler handler(exchange);
  handler.resync();
  EXPECT_EQ(handler.last_applied(), 500u);
  EXPECT_TRUE(handler.book() == exchange.book());
  stream(exchange, link, handler, 500);
  expect_same_book(handler, exchange);
}

// ---- functional: whole streams through the lossy link ----

/** @test T-FH-12 (R1, R7) With no damage, the handler rebuilds the book and never sees a gap. */
TEST(FeedHandler, LosslessStreamRebuildsTheBook){
  Exchange exchange(1, 1'000);
  LossyLink link({});
  FeedHandler handler(exchange);
  stream(exchange, link, handler, 5'000);
  expect_same_book(handler, exchange);
  EXPECT_EQ(handler.stats().gaps, 0u);
}

/** @test T-FH-13 (R4, R7) Seqs 101-150 are lost; one retransmission brings back exactly those 50, with no heartbeat needed. */
TEST(FeedHandler, InjectedGapIsRecoveredByRetransmission){
  Exchange exchange(2, 1'000);
  LinkConfig config;
  config.drop_ranges = {{101, 150}};
  LossyLink link(config);
  FeedHandler handler(exchange);
  stream(exchange, link, handler, 1'000, false);
  expect_same_book(handler, exchange);
  EXPECT_EQ(handler.stats().gaps, 1u);
  EXPECT_EQ(handler.stats().retransmits, 1u);
  EXPECT_EQ(handler.stats().recovered, 50u);
  EXPECT_EQ(handler.stats().snapshots, 0u);
}

/** @test T-FH-14 (R2, R3, R7) Events arrive late or twice but none is lost: every gap closes within the window, so nothing is retransmitted. */
TEST(FeedHandler, ReorderingAndDuplicatesNeedNoRecovery){
  Exchange exchange(3, 1'000);
  LinkConfig config;
  config.reorder = 0.2;
  config.duplicate = 0.1;
  config.seed = 3 ^ link_salt;
  LossyLink link(config);
  FeedHandler handler(exchange);
  stream(exchange, link, handler, 5'000);
  expect_same_book(handler, exchange);
  EXPECT_GT(handler.stats().gaps, 0u);
  EXPECT_EQ(handler.stats().retransmits, 0u);
  EXPECT_GT(handler.stats().duplicates, 0u);
}

/** @test T-FH-15 (R6, R7) The last five events are lost and nothing follows them; only the heartbeat reveals the loss. */
TEST(FeedHandler, HeartbeatExposesLossOfTheLastEvents){
  Exchange exchange(4, 1'000);
  LinkConfig config;
  config.drop_ranges = {{496, 500}};
  LossyLink link(config);
  FeedHandler handler(exchange);
  stream(exchange, link, handler, 500, false);
  EXPECT_EQ(handler.last_applied(), 495u);
  handler.on_heartbeat(exchange.last_seq());
  expect_same_book(handler, exchange);
  EXPECT_EQ(handler.stats().gaps, 1u);  // a gap no arrival opened still counts
  EXPECT_EQ(handler.stats().retransmits, 1u);
}

/** @test T-FH-16 (R5, R7) Seqs 201-600 are lost and the exchange keeps only 100 events: recovery needs a snapshot. */
TEST(FeedHandler, GapOlderThanTheHistoryIsRecoveredBySnapshot){
  Exchange exchange(5, 100);
  LinkConfig config;
  config.drop_ranges = {{201, 600}};
  LossyLink link(config);
  FeedHandler handler(exchange);
  stream(exchange, link, handler, 1'000);
  expect_same_book(handler, exchange);
  EXPECT_EQ(handler.stats().snapshots, 1u);
}

/** @test T-FH-17 (R7, R8) 20 seeds of 20,000 events with loss, duplication and reordering at once, under both recovery policies: the book matches every time. */
TEST(FeedHandler, RandomDamageAcrossSeedsNeverLeavesAWrongBook){
  for (const Recovery recovery : {Recovery::every_hole, Recovery::first_hole}){
    for (uint64_t seed = 1; seed <= 20; ++seed){
      Exchange exchange(seed, 2'000);
      LinkConfig config;
      config.loss = 0.10;
      config.duplicate = 0.02;
      config.reorder = 0.05;
      config.seed = seed ^ link_salt;
      LossyLink link(config);
      FeedHandler handler(exchange, 3, recovery);
      stream(exchange, link, handler, 20'000);
      SCOPED_TRACE("seed " + to_string(seed) + (recovery == Recovery::every_hole ? ", every hole" : ", first hole"));
      expect_same_book(handler, exchange);
    }
  }
}

}  // namespace
