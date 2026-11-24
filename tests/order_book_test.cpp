// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Unit tests for the order book (requirement R11): each event type, each
 * reason to reject an event, best bid and ask, equality and the fingerprint.
 * Every expected value is worked out by hand, so the book is checked against
 * arithmetic, not against itself.
 */

#include <cstdint>

#include <gtest/gtest.h>

#include "book/order_book.hpp"

using namespace std;

namespace{

Event add(uint64_t order_id, Side side, int64_t price, uint64_t quantity){
  return {0, EventType::add, order_id, side, price, quantity};
}
Event modify(uint64_t order_id, uint64_t quantity){
  return {0, EventType::modify, order_id, Side::bid, 0, quantity};
}
Event remove_order(uint64_t order_id){
  return {0, EventType::remove, order_id, Side::bid, 0, 0};
}
Event execute(uint64_t order_id, uint64_t quantity){
  return {0, EventType::execute, order_id, Side::bid, 0, quantity};
}

/** @test T-OB-01 (R11) An empty book has no best bid, no best ask and no orders. */
TEST(OrderBook, EmptyBookHasNoBestBidOrAsk){
  const OrderBook book;
  EXPECT_FALSE(book.best_bid());
  EXPECT_FALSE(book.best_ask());
  EXPECT_TRUE(book.orders().empty());
  EXPECT_EQ(book.fingerprint(), 0u);
}

/** @test T-OB-02 (R11) Adds rest at their price, and a level's total is the sum of its orders. */
TEST(OrderBook, AddRestsAnOrderAtItsLevel){
  OrderBook book;
  EXPECT_TRUE(book.apply(add(1, Side::bid, 99, 10)));
  EXPECT_TRUE(book.apply(add(2, Side::bid, 99, 5)));
  EXPECT_TRUE(book.apply(add(3, Side::ask, 101, 7)));
  EXPECT_EQ(book.best_bid(), (Level{99, 15}));
  EXPECT_EQ(book.best_ask(), (Level{101, 7}));
  EXPECT_EQ(book.orders().at(2), (Order{Side::bid, 99, 5}));
}

/** @test T-OB-03 (R11) The best bid is the highest bid and the best ask the lowest ask. */
TEST(OrderBook, BestBidIsTheHighestAndBestAskTheLowest){
  OrderBook book;
  book.apply(add(1, Side::bid, 97, 1));
  book.apply(add(2, Side::bid, 99, 2));
  book.apply(add(3, Side::bid, 98, 3));
  book.apply(add(4, Side::ask, 103, 4));
  book.apply(add(5, Side::ask, 101, 5));
  book.apply(add(6, Side::ask, 102, 6));
  EXPECT_EQ(book.best_bid(), (Level{99, 2}));
  EXPECT_EQ(book.best_ask(), (Level{101, 5}));
}

/** @test T-OB-04 (R11) An add with a zero quantity or an id already resting is rejected. */
TEST(OrderBook, AddRejectsZeroQuantityAndDuplicateIds){
  OrderBook book;
  EXPECT_FALSE(book.apply(add(1, Side::bid, 99, 0)));
  EXPECT_TRUE(book.orders().empty());
  EXPECT_TRUE(book.apply(add(1, Side::bid, 99, 10)));
  EXPECT_FALSE(book.apply(add(1, Side::ask, 101, 3)));
  EXPECT_EQ(book.orders().at(1), (Order{Side::bid, 99, 10}));
  EXPECT_FALSE(book.best_ask());
}

/** @test T-OB-05 (R11) A modify sets an order's quantity, keeps its price and moves its level's total. */
TEST(OrderBook, ModifyChangesQuantityAndLevelTotal){
  OrderBook book;
  book.apply(add(1, Side::bid, 99, 10));
  book.apply(add(2, Side::bid, 99, 5));
  EXPECT_TRUE(book.apply(modify(1, 4)));
  EXPECT_EQ(book.orders().at(1), (Order{Side::bid, 99, 4}));
  EXPECT_EQ(book.best_bid(), (Level{99, 9}));
  EXPECT_TRUE(book.apply(modify(2, 20)));
  EXPECT_EQ(book.best_bid(), (Level{99, 24}));
}

/** @test T-OB-06 (R11) A modify to zero, or of an unknown order, is rejected. */
TEST(OrderBook, ModifyRejectsZeroAndUnknownOrders){
  OrderBook book;
  book.apply(add(1, Side::ask, 101, 10));
  EXPECT_FALSE(book.apply(modify(1, 0)));
  EXPECT_FALSE(book.apply(modify(2, 5)));
  EXPECT_EQ(book.best_ask(), (Level{101, 10}));
}

/** @test T-OB-07 (R11) A remove takes the order out; its level goes only when nothing else rests there. */
TEST(OrderBook, RemoveTakesOutTheOrderAndAnEmptyLevel){
  OrderBook book;
  book.apply(add(1, Side::bid, 99, 10));
  book.apply(add(2, Side::bid, 99, 5));
  book.apply(add(3, Side::bid, 98, 1));
  EXPECT_TRUE(book.apply(remove_order(1)));
  EXPECT_EQ(book.best_bid(), (Level{99, 5}));
  EXPECT_TRUE(book.apply(remove_order(2)));
  EXPECT_EQ(book.best_bid(), (Level{98, 1}));
  EXPECT_FALSE(book.orders().contains(2));
  EXPECT_FALSE(book.apply(remove_order(2)));  // already gone
}

/** @test T-OB-08 (R11) A partial fill reduces the order; a full fill removes it and its level. */
TEST(OrderBook, ExecuteFillsPartlyOrFully){
  OrderBook book;
  book.apply(add(1, Side::ask, 101, 10));
  EXPECT_TRUE(book.apply(execute(1, 4)));
  EXPECT_EQ(book.best_ask(), (Level{101, 6}));
  EXPECT_TRUE(book.apply(execute(1, 6)));
  EXPECT_FALSE(book.best_ask());
  EXPECT_TRUE(book.orders().empty());
}

/** @test T-OB-09 (R11) A fill of zero, a fill larger than the order and a fill of an unknown order are rejected. */
TEST(OrderBook, ExecuteRejectsZeroOverfillAndUnknownOrders){
  OrderBook book;
  book.apply(add(1, Side::bid, 99, 10));
  EXPECT_FALSE(book.apply(execute(1, 0)));
  EXPECT_FALSE(book.apply(execute(1, 11)));
  EXPECT_FALSE(book.apply(execute(7, 1)));
  EXPECT_EQ(book.best_bid(), (Level{99, 10}));
}

/** @test T-OB-10 (R11) Every rejected event leaves the book exactly as it was. */
TEST(OrderBook, RejectedEventsLeaveTheBookUnchanged){
  OrderBook book;
  book.apply(add(1, Side::bid, 99, 10));
  book.apply(add(2, Side::ask, 101, 7));
  const OrderBook before = book;
  EXPECT_FALSE(book.apply(add(1, Side::bid, 98, 1)));
  EXPECT_FALSE(book.apply(add(3, Side::bid, 98, 0)));
  EXPECT_FALSE(book.apply(modify(9, 1)));
  EXPECT_FALSE(book.apply(modify(1, 0)));
  EXPECT_FALSE(book.apply(remove_order(42)));
  EXPECT_FALSE(book.apply(execute(2, 8)));
  EXPECT_TRUE(book == before);
  EXPECT_EQ(book.fingerprint(), before.fingerprint());
  EXPECT_EQ(book.best_bid(), before.best_bid());
  EXPECT_EQ(book.best_ask(), before.best_ask());
}

/** @test T-OB-11 (R11) Books holding the same orders are equal, with equal fingerprints, whatever order the orders arrived in. */
TEST(OrderBook, EqualityAndFingerprintIgnoreArrivalOrder){
  OrderBook forward, backward;
  for (uint64_t id = 1; id <= 50; ++id){
    forward.apply(add(id, id % 2 ? Side::bid : Side::ask, id % 2 ? 90 + static_cast<int64_t>(id % 7) : 110, id));
  }
  for (uint64_t id = 50; id >= 1; --id){
    backward.apply(add(id, id % 2 ? Side::bid : Side::ask, id % 2 ? 90 + static_cast<int64_t>(id % 7) : 110, id));
  }
  EXPECT_TRUE(forward == backward);
  EXPECT_EQ(forward.fingerprint(), backward.fingerprint());
}

/** @test T-OB-12 (R11) A different id, side, price or quantity in one order changes both equality and the fingerprint. */
TEST(OrderBook, AnyDifferenceChangesTheFingerprint){
  OrderBook base;
  base.apply(add(1, Side::bid, 99, 10));
  base.apply(add(2, Side::ask, 101, 7));
  const Event variants[] = {add(3, Side::bid, 99, 10), add(1, Side::ask, 99, 10), add(1, Side::bid, 98, 10),
                            add(1, Side::bid, 99, 11)};
  for (const Event& variant : variants){
    OrderBook other;
    other.apply(variant);
    other.apply(add(2, Side::ask, 101, 7));
    EXPECT_FALSE(other == base);
    EXPECT_NE(other.fingerprint(), base.fingerprint());
  }
}

/** @test T-OB-13 (R11) A sequence of every event type, checked against totals worked out by hand. */
TEST(OrderBook, AppliesEachEventAndRejectsOnesThatDoNotFit){
  OrderBook book;
  EXPECT_TRUE(book.apply(add(1, Side::bid, 99, 10)));
  EXPECT_TRUE(book.apply(add(2, Side::bid, 99, 5)));
  EXPECT_TRUE(book.apply(add(3, Side::ask, 101, 7)));
  EXPECT_TRUE(book.apply(execute(1, 4)));
  EXPECT_TRUE(book.apply(modify(2, 1)));
  EXPECT_EQ(book.best_bid(), (Level{99, 7}));
  EXPECT_TRUE(book.apply(remove_order(3)));
  EXPECT_FALSE(book.best_ask());
  EXPECT_TRUE(book.apply(execute(1, 6)));  // fills the rest, so the order leaves
  EXPECT_FALSE(book.orders().contains(1));
  EXPECT_EQ(book.best_bid(), (Level{99, 1}));
}

}  // namespace
