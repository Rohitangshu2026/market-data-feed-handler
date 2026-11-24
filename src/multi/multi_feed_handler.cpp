// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Implementation of the multi-instrument feed handler: routes events and
 * heartbeats to the appropriate per-instrument FeedHandler.
 */

#include "multi/multi_feed_handler.hpp"

using namespace std;

MultiFeedHandler::MultiFeedHandler(const MultiExchange& exchange,
                                   size_t reorder_window,
                                   Recovery recovery){
  // Initializes one independent FeedHandler per registered instrument symbol.
  for (const string& symbol : exchange.symbols()){
    handlers_.emplace(symbol, FeedHandler(exchange.exchange(symbol), reorder_window, recovery));
  }
}

bool MultiFeedHandler::on_event(const InstrumentEvent& event){
  // Look up destination feed handler by instrument symbol.
  const auto it = handlers_.find(event.symbol);
  if (it == handlers_.end()){
    ++unknown_symbols_;  // safely drop and count unrecognized instruments
    return false;
  }
  // Route to the symbol's dedicated feed handler.
  it->second.on_event(event.event);
  return true;
}

bool MultiFeedHandler::on_heartbeat(const string& symbol, uint64_t last_seq){
  // Dispatch heartbeat only to the matching instrument handler.
  const auto it = handlers_.find(symbol);
  if (it == handlers_.end()){
    return false;
  }
  it->second.on_heartbeat(last_seq);
  return true;
}

const FeedHandler& MultiFeedHandler::handler(const string& symbol) const{
  return handlers_.at(symbol);
}

const OrderBook& MultiFeedHandler::book(const string& symbol) const{
  return handlers_.at(symbol).book();
}

vector<string> MultiFeedHandler::symbols() const{
  // Collects managed symbols in sorted order.
  vector<string> result;
  result.reserve(handlers_.size());
  for (const auto& [symbol, _] : handlers_){
    result.push_back(symbol);
  }
  return result;
}
