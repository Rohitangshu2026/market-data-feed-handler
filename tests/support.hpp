// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Helpers shared by the test files: driving a stream end to end, and the
 * check every handler test ends with.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "exchange/exchange.hpp"
#include "handler/feed_handler.hpp"
#include "network/lossy_link.hpp"

/** The link's seed is the exchange's seed xor this, as in the programs. */
constexpr uint64_t link_salt = 0x9e3779b97f4a7c15ULL;

/**
 * Sends `events` events from the exchange through the link to the handler.
 *
 * @param exchange the stream's source
 * @param link the damage on the way
 * @param handler the handler under test
 * @param events how many events the exchange publishes
 * @param heartbeat whether to end with the exchange's heartbeat, as an idle
 *        stream would
 */
inline void stream(Exchange& exchange, LossyLink& link, FeedHandler& handler, uint64_t events, bool heartbeat = true){
  for (uint64_t sent = 0; sent < events; ++sent){
    for (const Event& event : link.send(exchange.next())){
      handler.on_event(event);
    }
  }
  for (const Event& event : link.flush()){
    handler.on_event(event);
  }
  if (heartbeat){
    handler.on_heartbeat(exchange.last_seq());
  }
}

/**
 * The verdict every handler test ends with: the same last seq, the same
 * orders, the same best bid and ask and the same fingerprint as the
 * exchange's own book, nothing still buffered, and no event rejected.
 */
inline void expect_same_book(const FeedHandler& handler, const Exchange& exchange){
  EXPECT_EQ(handler.last_applied(), exchange.last_seq());
  EXPECT_TRUE(handler.book() == exchange.book());
  EXPECT_EQ(handler.book().best_bid(), exchange.book().best_bid());
  EXPECT_EQ(handler.book().best_ask(), exchange.book().best_ask());
  EXPECT_EQ(handler.book().fingerprint(), exchange.book().fingerprint());
  EXPECT_EQ(handler.buffered(), 0u);
  EXPECT_EQ(handler.stats().rejected, 0u);
}

/**
 * Publishes `count` events and returns them, for tests that hand events to
 * the handler directly, in an order of their choosing.
 */
inline std::vector<Event> publish(Exchange& exchange, uint64_t count){
  std::vector<Event> events;
  for (uint64_t i = 0; i < count; ++i){
    events.push_back(exchange.next());
  }
  return events;
}
