// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * The exchange: the source of the sequenced event stream, and the service a
 * feed handler recovers missed events from.
 */

#pragma once

#include<cstddef>
#include<cstdint>
#include<deque>
#include<optional>
#include<random>
#include<vector>

#include "book/order_book.hpp"

using namespace std;

/** The exchange's whole book as it stood after event `seq`. */
struct Snapshot{
  uint64_t seq;    /**< the last event the book includes */
  OrderBook book;  /**< the book as it stood after `seq` */
};

/**
 * Generates a valid event stream from a seed and numbers it 1, 2, 3, ...
 *
 * Every event is applied to the exchange's own book, so that book is the
 * truth a feed handler's book is checked against. The last `history` events
 * are kept for retransmission; anything older can only be recovered with a
 * snapshot. In a real system retransmit() and snapshot() would be requests
 * over TCP to the exchange's recovery service; here they are function calls.
 */
class Exchange{
 public:
  /**
   * @param seed seeds the generator: the same seed gives the same stream, on
   *        every platform
   * @param history how many of the latest events to keep for retransmission
   * @param max_orders the most orders that rest in the book at once
   * @throws invalid_argument if `history` or `max_orders` is 0
   */
  Exchange(uint64_t seed, size_t history, size_t max_orders = 200);

  /**
   * Generates the next event, applies it to the exchange's book, numbers it
   * and keeps it for retransmission.
   *
   * @return the event, numbered one past the previous one
   */
  Event next();

  /** @return the seq of the latest event, 0 before the first */
  uint64_t last_seq() const{
    return last_seq_;
  }

  /** @return the exchange's own book: the truth */
  const OrderBook& book() const{
    return book_;
  }

  /** @return how many of the latest events are kept for retransmission */
  size_t history() const{
    return history_limit_;
  }

  /**
   * The events `first` to `last`, as they were published.
   *
   * @param first the first seq wanted
   * @param last the last seq wanted
   * @return the events in seq order, or nullopt if the range is empty, reaches
   *         past the latest event, or starts before the oldest one still kept
   */
  optional<vector<Event>> retransmit(uint64_t first, uint64_t last) const;

  /** @return the whole book as of the latest event */
  Snapshot snapshot() const{
    return {last_seq_, book_};
  }

 private:
  /** @return a random event that is valid for the current book, not yet numbered */
  Event generate();

  /** @return a random number from 0 to `bound` - 1 */
  uint64_t random_below(uint64_t bound){
    return random_() % bound;
  }

  mt19937_64 random_;
  OrderBook book_;
  vector<uint64_t> resting_;  /**< ids of the resting orders, to pick one at random */
  uint64_t next_order_id_ = 1;
  uint64_t last_seq_ = 0;
  size_t history_limit_;
  size_t max_orders_;
  deque<Event> history_;  /**< the last history_limit_ events, oldest first */
};
