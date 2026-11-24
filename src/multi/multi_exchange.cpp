// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

#include "multi/multi_exchange.hpp"

using namespace std;

void MultiExchange::add_instrument(const string& symbol, uint64_t seed, size_t history, size_t max_orders){
  if (symbol.empty()){
    throw invalid_argument("an instrument's symbol must not be empty");
  }
  if (exchanges_.contains(symbol)){
    throw invalid_argument("the instrument '" + symbol + "' is already registered");
  }
  exchanges_.emplace(symbol, Exchange(seed, history, max_orders));
}

InstrumentEvent MultiExchange::next(const string& symbol){
  return {symbol, exchanges_.at(symbol).next()};
}

const Exchange& MultiExchange::exchange(const string& symbol) const{
  return exchanges_.at(symbol);
}

vector<string> MultiExchange::symbols() const{
  vector<string> result;
  result.reserve(exchanges_.size());
  for (const auto& [symbol, _] : exchanges_){
    result.push_back(symbol);
  }
  return result;
}
