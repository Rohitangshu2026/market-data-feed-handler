// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

#include "multi/multi_lossy_link.hpp"

#include<stdexcept>

using namespace std;

void MultiLossyLink::add_instrument(const string& symbol, const LinkConfig& config){
  if (links_.contains(symbol)){
    throw invalid_argument("the instrument '" + symbol + "' already has a link");
  }
  links_.emplace(symbol, LossyLink(config));
}

vector<InstrumentEvent> MultiLossyLink::send(const InstrumentEvent& event){
  vector<Event> arrived = links_.at(event.symbol).send(event.event);
  vector<InstrumentEvent> result;
  result.reserve(arrived.size());
  for (Event& e : arrived){
    result.push_back({event.symbol, std::move(e)});
  }
  return result;
}

vector<InstrumentEvent> MultiLossyLink::flush(){
  vector<InstrumentEvent> result;
  for (auto& [symbol, link] : links_){
    for (Event& event : link.flush()){
      result.push_back({symbol, std::move(event)});
    }
  }
  return result;
}
