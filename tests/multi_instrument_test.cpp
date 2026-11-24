// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Tests for multi-instrument support and partitioning:
 * 1. Creating multiple instruments.
 * 2. Independent order books for different instruments.
 * 3. Routing an event to the correct instrument.
 * 4. Ensuring one instrument's price levels never mix with another's.
 * 5. Independent sequence numbering.
 * 6. A gap in one instrument while another continues normally.
 * 7. Retransmission recovery for one instrument.
 * 8. Snapshot fallback for one instrument.
 * 9. Unknown instrument handling.
 * 10. Existing single-instrument tests and demos remaining functional.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "multi/multi_exchange.hpp"
#include "multi/multi_feed_handler.hpp"
#include "multi/multi_lossy_link.hpp"
#include "support.hpp"

using namespace std;

namespace {

/** Helper to verify reconstructed multi-instrument books match exchange books. */
void expect_same_multi_books(const MultiFeedHandler& handler, const MultiExchange& exchange) {
  for (const string& sym : exchange.symbols()) {
    const FeedHandler& h = handler.handler(sym);
    const Exchange& e = exchange.exchange(sym);
    expect_same_book(h, e);
  }
}

/** 1. Creating multiple instruments */
TEST(MultiInstrument, CreateMultipleInstruments) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 101, 100);
  exchange.add_instrument("TCS", 102, 100);
  exchange.add_instrument("INFY", 103, 100);

  EXPECT_EQ(exchange.size(), 3u);
  EXPECT_TRUE(exchange.contains("RELIANCE"));
  EXPECT_TRUE(exchange.contains("TCS"));
  EXPECT_TRUE(exchange.contains("INFY"));
  EXPECT_FALSE(exchange.contains("WIPRO"));

  EXPECT_THROW(exchange.add_instrument("", 1, 100), invalid_argument);
  EXPECT_THROW(exchange.add_instrument("TCS", 2, 100), invalid_argument);

  MultiFeedHandler handler(exchange);
  EXPECT_EQ(handler.size(), 3u);
  EXPECT_TRUE(handler.contains("RELIANCE"));
  EXPECT_TRUE(handler.contains("TCS"));
  EXPECT_TRUE(handler.contains("INFY"));
  EXPECT_FALSE(handler.contains("WIPRO"));
}

/** 2. Independent order books for different instruments */
TEST(MultiInstrument, IndependentOrderBooks) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 10, 100);
  exchange.add_instrument("TCS", 20, 100);

  MultiFeedHandler handler(exchange);

  // Generate 5 events for RELIANCE and 10 events for TCS
  for (int i = 0; i < 5; ++i) {
    handler.on_event(exchange.next("RELIANCE"));
  }
  for (int i = 0; i < 10; ++i) {
    handler.on_event(exchange.next("TCS"));
  }

  EXPECT_EQ(handler.handler("RELIANCE").last_applied(), 5u);
  EXPECT_EQ(handler.handler("TCS").last_applied(), 10u);

  // Both match their respective authoritative truth books
  expect_same_book(handler.handler("RELIANCE"), exchange.exchange("RELIANCE"));
  expect_same_book(handler.handler("TCS"), exchange.exchange("TCS"));

  // Books differ from each other
  EXPECT_NE(handler.book("RELIANCE").fingerprint(), handler.book("TCS").fingerprint());
}

/** 3. Routing an event to the correct instrument */
TEST(MultiInstrument, EventRouting) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 1, 100);
  exchange.add_instrument("TCS", 2, 100);

  MultiFeedHandler handler(exchange);

  InstrumentEvent rel_event = exchange.next("RELIANCE");
  InstrumentEvent tcs_event = exchange.next("TCS");

  EXPECT_TRUE(handler.on_event(rel_event));
  EXPECT_EQ(handler.handler("RELIANCE").last_applied(), 1u);
  EXPECT_EQ(handler.handler("TCS").last_applied(), 0u);

  EXPECT_TRUE(handler.on_event(tcs_event));
  EXPECT_EQ(handler.handler("RELIANCE").last_applied(), 1u);
  EXPECT_EQ(handler.handler("TCS").last_applied(), 1u);
}

/** 4. Ensuring one instrument's price levels never mix with another's */
TEST(MultiInstrument, PriceLevelsNeverMix) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 111, 200);
  exchange.add_instrument("TCS", 222, 200);

  MultiFeedHandler handler(exchange);

  for (int i = 0; i < 50; ++i) {
    handler.on_event(exchange.next("RELIANCE"));
    handler.on_event(exchange.next("TCS"));
  }

  // Cross-check orders in RELIANCE are NOT in TCS
  const auto& rel_orders = handler.book("RELIANCE").orders();
  const auto& tcs_orders = handler.book("TCS").orders();
  EXPECT_FALSE(rel_orders.empty());
  EXPECT_FALSE(tcs_orders.empty());

  // Even if both order books have order id = 1, they are completely separate books
  EXPECT_EQ(handler.book("RELIANCE"), exchange.exchange("RELIANCE").book());
  EXPECT_EQ(handler.book("TCS"), exchange.exchange("TCS").book());
}

/** 5. Independent sequence numbering */
TEST(MultiInstrument, IndependentSequenceNumbering) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 1, 100);
  exchange.add_instrument("TCS", 2, 100);
  exchange.add_instrument("INFY", 3, 100);

  // Each stream starts at seq 1
  InstrumentEvent r1 = exchange.next("RELIANCE");
  InstrumentEvent t1 = exchange.next("TCS");
  InstrumentEvent i1 = exchange.next("INFY");

  EXPECT_EQ(r1.event.seq, 1u);
  EXPECT_EQ(t1.event.seq, 1u);
  EXPECT_EQ(i1.event.seq, 1u);

  InstrumentEvent r2 = exchange.next("RELIANCE");
  EXPECT_EQ(r2.event.seq, 2u);

  MultiFeedHandler handler(exchange);
  // Interleaved delivery: RELIANCE 1, TCS 1, INFY 1, RELIANCE 2
  handler.on_event(r1);
  handler.on_event(t1);
  handler.on_event(i1);
  handler.on_event(r2);

  // No gaps observed despite interleaved stream!
  EXPECT_EQ(handler.handler("RELIANCE").stats().gaps, 0u);
  EXPECT_EQ(handler.handler("TCS").stats().gaps, 0u);
  EXPECT_EQ(handler.handler("INFY").stats().gaps, 0u);

  EXPECT_EQ(handler.handler("RELIANCE").last_applied(), 2u);
  EXPECT_EQ(handler.handler("TCS").last_applied(), 1u);
  EXPECT_EQ(handler.handler("INFY").last_applied(), 1u);
}

/** 6. A gap in one instrument while another continues normally */
TEST(MultiInstrument, GapInOneInstrumentDoesNotAffectOthers) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 10, 100);
  exchange.add_instrument("TCS", 20, 100);

  MultiFeedHandler handler(exchange, 3);

  InstrumentEvent r1 = exchange.next("RELIANCE");
  InstrumentEvent r2 = exchange.next("RELIANCE"); // will drop
  InstrumentEvent r3 = exchange.next("RELIANCE");

  InstrumentEvent t1 = exchange.next("TCS");
  InstrumentEvent t2 = exchange.next("TCS");

  handler.on_event(r1);
  handler.on_event(t1);
  handler.on_event(t2);

  // Deliver r3 (gap in RELIANCE: expected 2, got 3)
  handler.on_event(r3);

  // RELIANCE has 1 gap and 1 buffered event
  EXPECT_EQ(handler.handler("RELIANCE").stats().gaps, 1u);
  EXPECT_EQ(handler.handler("RELIANCE").buffered(), 1u);
  EXPECT_EQ(handler.handler("RELIANCE").last_applied(), 1u);

  // TCS is completely unaffected
  EXPECT_EQ(handler.handler("TCS").stats().gaps, 0u);
  EXPECT_EQ(handler.handler("TCS").buffered(), 0u);
  EXPECT_EQ(handler.handler("TCS").last_applied(), 2u);
}

/** 7. Retransmission recovery for one instrument */
TEST(MultiInstrument, RetransmissionRecoveryForOneInstrument) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 1, 100);
  exchange.add_instrument("TCS", 2, 100);

  MultiLossyLink link;
  LinkConfig rel_config;
  rel_config.drop_ranges = {{5, 8}}; // drop seqs 5-8 for RELIANCE
  link.add_instrument("RELIANCE", rel_config);
  link.add_instrument("TCS", {}); // no loss for TCS

  MultiFeedHandler handler(exchange, 3);

  // Stream 20 events for each
  for (int i = 0; i < 20; ++i) {
    for (const auto& ev : link.send(exchange.next("RELIANCE"))) {
      handler.on_event(ev);
    }
    for (const auto& ev : link.send(exchange.next("TCS"))) {
      handler.on_event(ev);
    }
  }

  // Flushes & heartbeats
  for (const auto& ev : link.flush()) {
    handler.on_event(ev);
  }
  handler.on_heartbeat("RELIANCE", exchange.exchange("RELIANCE").last_seq());
  handler.on_heartbeat("TCS", exchange.exchange("TCS").last_seq());

  // RELIANCE recovered 4 events via retransmission
  EXPECT_EQ(handler.handler("RELIANCE").stats().retransmits, 1u);
  EXPECT_EQ(handler.handler("RELIANCE").stats().recovered, 4u);
  EXPECT_EQ(handler.handler("RELIANCE").stats().snapshots, 0u);

  // TCS had no retransmission
  EXPECT_EQ(handler.handler("TCS").stats().retransmits, 0u);

  expect_same_multi_books(handler, exchange);
}

/** 8. Snapshot fallback for one instrument */
TEST(MultiInstrument, SnapshotFallbackForOneInstrument) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 1, 5); // history only 5
  exchange.add_instrument("TCS", 2, 100);

  MultiFeedHandler handler(exchange, 0); // window 0: immediate recovery

  // Publish 20 events for RELIANCE
  vector<InstrumentEvent> rel_events;
  for (int i = 0; i < 20; ++i) {
    rel_events.push_back(exchange.next("RELIANCE"));
  }

  // Publish 20 events for TCS
  for (int i = 0; i < 20; ++i) {
    handler.on_event(exchange.next("TCS"));
  }

  // RELIANCE: apply 1, 2, then skip to 15 (history 5 only keeps 16-20)
  handler.on_event(rel_events[0]);
  handler.on_event(rel_events[1]);
  handler.on_event(rel_events[14]); // seq 15, missing 3..14, oldest in history is 16

  EXPECT_EQ(handler.handler("RELIANCE").stats().snapshots, 1u);
  EXPECT_EQ(handler.handler("TCS").stats().snapshots, 0u);

  // Deliver remaining events
  for (size_t i = 15; i < 20; ++i) {
    handler.on_event(rel_events[i]);
  }

  expect_same_multi_books(handler, exchange);
}

/** 9. Unknown instrument handling */
TEST(MultiInstrument, UnknownInstrumentHandling) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 1, 100);

  MultiFeedHandler handler(exchange);

  InstrumentEvent unknown_ev{"UNKNOWN_SYM", Event{1, EventType::add, 1, Side::bid, 100, 10}};
  EXPECT_FALSE(handler.on_event(unknown_ev));
  EXPECT_EQ(handler.unknown_symbols(), 1u);

  EXPECT_FALSE(handler.on_heartbeat("UNKNOWN_SYM", 10));

  EXPECT_THROW(handler.handler("UNKNOWN_SYM"), out_of_range);
  EXPECT_THROW(handler.book("UNKNOWN_SYM"), out_of_range);
}

/** 10. Multi-instrument with full lossy link across multiple symbols */
TEST(MultiInstrument, FullSimulationAcrossSymbols) {
  MultiExchange exchange;
  exchange.add_instrument("RELIANCE", 1, 500);
  exchange.add_instrument("TCS", 2, 500);
  exchange.add_instrument("INFY", 3, 500);

  MultiLossyLink link;
  LinkConfig c1; c1.loss = 0.05; c1.reorder = 0.05; c1.duplicate = 0.02; c1.seed = 101;
  LinkConfig c2; c2.loss = 0.08; c2.reorder = 0.02; c2.duplicate = 0.03; c2.seed = 202;
  LinkConfig c3; c3.loss = 0.02; c3.reorder = 0.08; c3.duplicate = 0.01; c3.seed = 303;

  link.add_instrument("RELIANCE", c1);
  link.add_instrument("TCS", c2);
  link.add_instrument("INFY", c3);

  MultiFeedHandler handler(exchange, 3);

  vector<string> symbols = {"RELIANCE", "TCS", "INFY"};
  for (int i = 0; i < 500; ++i) {
    for (const string& sym : symbols) {
      for (const auto& ev : link.send(exchange.next(sym))) {
        handler.on_event(ev);
      }
    }
  }

  for (const auto& ev : link.flush()) {
    handler.on_event(ev);
  }

  for (const string& sym : symbols) {
    handler.on_heartbeat(sym, exchange.exchange(sym).last_seq());
  }

  expect_same_multi_books(handler, exchange);
}

}  // namespace
