// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * A multi-instrument exchange: one independent Exchange per symbol, each with
 * its own event stream numbered 1, 2, 3, ..., its own order book, and its own
 * retransmission history.
 */

#pragma once

#include<map>
#include<stdexcept>
#include<string>
#include<vector>

#include "exchange/exchange.hpp"

using namespace std;

/** An event tagged with the instrument it belongs to. */
struct InstrumentEvent{
  string symbol;  /**< the instrument: "RELIANCE", "TCS", ... */
  Event event;    /**< the event, with a per-instrument seq */
};

/**
 * Wraps one Exchange per instrument. Each instrument has its own sequence
 * numbers starting at 1, its own truth book, and its own retransmission
 * history. The existing Exchange class is reused without modification.
 */
class MultiExchange{
 public:
  /**
   * Adds an instrument. Must be called before any events are generated.
   *
   * @param symbol the instrument's ticker
   * @param seed seeds the instrument's event generator
   * @param history how many events the instrument keeps for retransmission
   * @param max_orders the most orders that rest in the instrument's book
   * @throws invalid_argument if the symbol is empty or already exists
   */
  void add_instrument(const string& symbol, uint64_t seed, size_t history, size_t max_orders = 200);

  /**
   * Generates the next event for the named instrument.
   *
   * @param symbol which instrument
   * @return the event, tagged with its symbol
   * @throws out_of_range if the symbol is unknown
   */
  InstrumentEvent next(const string& symbol);

  /**
   * @param symbol which instrument
   * @return the instrument's exchange
   * @throws out_of_range if the symbol is unknown
   */
  const Exchange& exchange(const string& symbol) const;

  /** @return the symbols, in sorted order */
  vector<string> symbols() const;

  /** @return how many instruments are registered */
  size_t size() const{ return exchanges_.size(); }

  /** @return true if the symbol is known */
  bool contains(const string& symbol) const{ return exchanges_.contains(symbol); }

 private:
  map<string, Exchange> exchanges_;
};
