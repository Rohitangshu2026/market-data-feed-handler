// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * A multi-instrument lossy link: one independent LossyLink per symbol, so
 * each instrument's events are damaged independently. Drop ranges refer to
 * the instrument's own sequence numbers, not a global sequence.
 */

#pragma once

#include<map>
#include<string>
#include<vector>

#include "multi/multi_exchange.hpp"
#include "network/lossy_link.hpp"

using namespace std;

/**
 * Wraps one LossyLink per instrument. The same LinkConfig can be used for
 * every instrument, or each can have its own. The existing LossyLink is
 * reused without modification.
 */
class MultiLossyLink{
 public:
  /**
   * Adds an instrument with its own link configuration.
   *
   * @param symbol the instrument's ticker
   * @param config what the link does to this instrument's events
   * @throws invalid_argument if the symbol is already registered
   */
  void add_instrument(const string& symbol, const LinkConfig& config);

  /**
   * Sends one event through the correct instrument's link.
   *
   * @param event the event the exchange published, tagged with its symbol
   * @return the events that arrive at the handler now, tagged with their symbol
   * @throws out_of_range if the symbol is unknown
   */
  vector<InstrumentEvent> send(const InstrumentEvent& event);

  /**
   * Flushes every instrument's held-back event.
   *
   * @return the events that arrive, tagged with their symbol
   */
  vector<InstrumentEvent> flush();

  /**
   * Checks whether an instrument has a lossy link configured.
   *
   * @param symbol the instrument ticker to check
   * @return true if the symbol has an active link, false otherwise
   */
  bool contains(const string& symbol) const{ return links_.contains(symbol); }

 private:
  map<string, LossyLink> links_;  /**< independent lossy links per instrument symbol */
};
