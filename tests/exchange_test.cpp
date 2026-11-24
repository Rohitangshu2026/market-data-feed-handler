// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Unit tests for the simulated exchange (R10, R15): its stream, its history
 * and its snapshots. The handler's tests trust the exchange as the source of
 * truth, so these tests check that trust is deserved.
 */

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "exchange/exchange.hpp"

using namespace std;

namespace{

vector<Event> publish(Exchange& exchange, uint64_t count){
  vector<Event> events;
  for (uint64_t i = 0; i < count; ++i){
    events.push_back(exchange.next());
  }
  return events;
}

/** @test T-EX-01 (R10) An exchange that keeps no history, or whose book can hold no order, is refused. */
TEST(Exchange, RejectsZeroHistoryAndZeroOrders){
  EXPECT_THROW(Exchange(1, 0), invalid_argument);
  EXPECT_THROW(Exchange(1, 10, 0), invalid_argument);
  EXPECT_NO_THROW(Exchange(1, 1, 1));
}

/** @test T-EX-02 (R10) Seqs run 1, 2, 3, ... with no gap, and last_seq() follows them. */
TEST(Exchange, NumbersEventsConsecutivelyFromOne){
  Exchange exchange(1, 100);
  EXPECT_EQ(exchange.last_seq(), 0u);
  const vector<Event> events = publish(exchange, 1'000);
  for (size_t i = 0; i < events.size(); ++i){
    ASSERT_EQ(events[i].seq, i + 1);
  }
  EXPECT_EQ(exchange.last_seq(), 1'000u);
}

/** @test T-EX-03 (R10, R15) The same seed gives the same stream; another seed gives another. */
TEST(Exchange, SameSeedSameStream){
  Exchange a(42, 10), b(42, 10), c(43, 10);
  const vector<Event> first = publish(a, 10'000), again = publish(b, 10'000), other = publish(c, 10'000);
  EXPECT_EQ(first, again);
  EXPECT_NE(first, other);
}

/** @test T-EX-04 (R10) Every event fits the book it is applied to: a fresh book accepts the whole stream and ends equal to the exchange's. */
TEST(Exchange, EveryEventFitsAndReplaysToTheSameBook){
  Exchange exchange(3, 10);
  OrderBook replay;
  for (const Event& event : publish(exchange, 50'000)){
    ASSERT_TRUE(replay.apply(event)) << "seq " << event.seq;
  }
  EXPECT_TRUE(replay == exchange.book());
}

/** @test T-EX-05 (R10) All four event types occur, the book never exceeds its order limit, and it is never crossed. */
TEST(Exchange, StreamIsVariedBoundedAndUncrossed){
  constexpr size_t max_orders = 25;
  Exchange exchange(4, 10, max_orders);
  size_t seen[4] = {};
  for (int i = 0; i < 20'000; ++i){
    ++seen[static_cast<int>(exchange.next().type)];
    ASSERT_LE(exchange.book().orders().size(), max_orders);
    const auto bid = exchange.book().best_bid(), ask = exchange.book().best_ask();
    if (bid && ask){
      ASSERT_LT(bid->price, ask->price);
    }
  }
  for (const size_t count : seen){
    EXPECT_GT(count, 1'000u);
  }
}

/** @test T-EX-06 (R10) A retransmission inside the history returns exactly the events that were published. */
TEST(Exchange, RetransmitsExactlyWhatWasPublished){
  Exchange exchange(5, 100);
  const vector<Event> events = publish(exchange, 250);  // keeps 151-250
  const auto missing = exchange.retransmit(160, 175);
  ASSERT_TRUE(missing);
  EXPECT_EQ(*missing, vector<Event>(events.begin() + 159, events.begin() + 175));
  EXPECT_EQ(exchange.retransmit(151, 250)->size(), 100u);  // the whole history
  EXPECT_EQ(exchange.retransmit(250, 250)->front(), events.back());
}

/** @test T-EX-07 (R10) A retransmission is refused when any of the range is older than the history or newer than the stream, or the range is reversed. */
TEST(Exchange, RefusesRetransmissionsItCannotServe){
  Exchange empty(6, 100);
  EXPECT_FALSE(empty.retransmit(1, 1));  // nothing published yet
  Exchange exchange(6, 100);
  publish(exchange, 250);
  EXPECT_FALSE(exchange.retransmit(150, 160));  // 150 has left the history
  EXPECT_TRUE(exchange.retransmit(151, 160));   // the oldest kept event
  EXPECT_FALSE(exchange.retransmit(240, 251));  // 251 is not published yet
  EXPECT_FALSE(exchange.retransmit(200, 199));
}

/** @test T-EX-08 (R10) A snapshot is the book at the latest seq, and a copy: later events do not change it. */
TEST(Exchange, SnapshotIsACopyOfTheBookAtTheLatestSeq){
  Exchange exchange(7, 10);
  publish(exchange, 500);
  const Snapshot snapshot = exchange.snapshot();
  EXPECT_EQ(snapshot.seq, 500u);
  EXPECT_TRUE(snapshot.book == exchange.book());
  publish(exchange, 10);
  EXPECT_FALSE(snapshot.book == exchange.book());
}

/** @test T-EX-09 (R15) The stream for seed 1 is pinned: if this fails, every published number has changed, so the change must be deliberate and the results rerun. */
TEST(Exchange, StreamForSeedOneIsPinned){
  Exchange exchange(1, 10);
  publish(exchange, 100'000);
  EXPECT_EQ(exchange.book().fingerprint(), 0x34442c3112be9bu);
}

}  // namespace
