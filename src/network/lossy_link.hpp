// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * The network between the exchange and the feed handler.
 */

#pragma once

#include<cstdint>
#include<optional>
#include<random>
#include<utility>
#include<vector>

#include "book/order_book.hpp"

using namespace std;

/** What the link does to events. */
struct LinkConfig{
  double loss = 0.0;       /**< chance an event is lost, from 0 to 1 */
  double duplicate = 0.0;  /**< chance an event arrives twice */
  double reorder = 0.0;    /**< chance an event is held back and arrives after the next one */
  /** Injected gaps: every event with a seq in one of these [first, last]
      ranges is lost. */
  vector<pair<uint64_t, uint64_t>> drop_ranges;
  uint64_t seed = 1;  /**< seeds the link's randomness; use a different seed from the exchange's */
};

/**
 * Behaves like UDP multicast: events can be lost, duplicated or reordered,
 * at random and in scripted gaps. It never changes an event's contents.
 */
class LossyLink{
 public:
  /**
   * @param config what the link does to events, and the seed of its randomness
   * @throws invalid_argument if a chance is not a number from 0 to 1, or a
   *         drop range is empty or starts at seq 0
   */
  explicit LossyLink(const LinkConfig& config);

  /**
   * Sends one event.
   *
   * @param event the event the exchange published
   * @return the events that arrive at the handler now, in arrival order: none,
   *         or up to three (the event, a duplicate of it, and an event held
   *         back earlier)
   */
  vector<Event> send(const Event& event);

  /** @return the event still held back, if any, delivered as the stream ends */
  vector<Event> flush();

  /** @return how many events the link has lost so far */
  uint64_t lost() const{
    return lost_;
  }

 private:
  /**
   * A random decision whose chance is `threshold` out of 2^53: true when 53
   * random bits fall below it. It is integer arithmetic on a standard engine,
   * so a seed draws the same decisions on every platform and standard library.
   */
  bool chance(uint64_t threshold){
    return (random_() >> 11) < threshold;
  }

  LinkConfig config_;
  mt19937_64 random_;
  uint64_t loss_;       /**< config_.loss, as a threshold out of 2^53 */
  uint64_t duplicate_;  /**< config_.duplicate, likewise */
  uint64_t reorder_;    /**< config_.reorder, likewise */
  optional<Event> held_;  /**< a reordered event, delivered after the next one */
  uint64_t lost_ = 0;
};
