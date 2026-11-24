// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * A multi-instrument feed handler: routes each event to the correct
 * instrument's FeedHandler, so every instrument has an independent order book,
 * independent sequence tracking, and independent recovery.
 */

#pragma once

#include<cstddef>
#include<map>
#include<string>
#include<vector>

#include "handler/feed_handler.hpp"
#include "multi/multi_exchange.hpp"

using namespace std;

/**
 * Wraps one FeedHandler per instrument and routes incoming events by symbol.
 * Each instrument has its own order book, its own gap detection, and its own
 * recovery state. A gap in one instrument does not trigger recovery for
 * another.
 *
 * The existing FeedHandler is reused without modification.
 */
class MultiFeedHandler{
 public:
  /**
   * @param exchange the multi-instrument exchange this handler recovers from
   * @param reorder_window per-instrument reorder window (same for all)
   * @param recovery per-instrument recovery policy (same for all)
   */
  explicit MultiFeedHandler(const MultiExchange& exchange,
                            size_t reorder_window = 3,
                            Recovery recovery = Recovery::every_hole);

  /**
   * Routes one event to the correct instrument's handler.
   *
   * @param event the arriving event, tagged with its symbol
   * @return true if the symbol is known; false if unknown (the event is
   *         dropped and counted)
   */
  bool on_event(const InstrumentEvent& event);

  /**
   * Sends a heartbeat to one instrument's handler.
   *
   * @param symbol which instrument
   * @param last_seq the exchange's latest seq for that instrument
   * @return true if the symbol is known
   */
  bool on_heartbeat(const string& symbol, uint64_t last_seq);

  /**
   * @param symbol which instrument
   * @return the instrument's handler
   * @throws out_of_range if the symbol is unknown
   */
  const FeedHandler& handler(const string& symbol) const;

  /**
   * @param symbol which instrument
   * @return the instrument's rebuilt book
   * @throws out_of_range if the symbol is unknown
   */
  const OrderBook& book(const string& symbol) const;

  /** @return the symbols, in sorted order */
  vector<string> symbols() const;

  /** @return how many instruments are managed */
  size_t size() const{ return handlers_.size(); }

  /** @return true if the symbol is known */
  bool contains(const string& symbol) const{ return handlers_.contains(symbol); }

  /** @return how many events arrived for unknown symbols */
  uint64_t unknown_symbols() const{ return unknown_symbols_; }

 private:
  map<string, FeedHandler> handlers_;
  uint64_t unknown_symbols_ = 0;
};
