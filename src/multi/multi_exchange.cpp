// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Implementation of the multi-instrument exchange: manages independent
 * Exchange instances per symbol.
 */

#include "multi/multi_exchange.hpp"

using namespace std;

void MultiExchange::add_instrument(const string& symbol, uint64_t seed, size_t history, size_t max_orders){
  // Instrument symbol must be non-empty and unique.
  if (symbol.empty()){
    throw invalid_argument("an instrument's symbol must not be empty");
  }
  if (exchanges_.contains(symbol)){
    throw invalid_argument("the instrument '" + symbol + "' is already registered");
  }
  exchanges_.emplace(symbol, Exchange(seed, history, max_orders));
}

InstrumentEvent MultiExchange::next(const string& symbol){
  // Generates next event with per-instrument sequence and tags with symbol.
  return {symbol, exchanges_.at(symbol).next()};
}

const Exchange& MultiExchange::exchange(const string& symbol) const{
  return exchanges_.at(symbol);
}

vector<string> MultiExchange::symbols() const{
  // Collects registered symbols in sorted order.
  vector<string> result;
  result.reserve(exchanges_.size());
  for (const auto& [symbol, _] : exchanges_){
    result.push_back(symbol);
  }
  return result;
}
