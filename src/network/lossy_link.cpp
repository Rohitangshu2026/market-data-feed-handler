// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

#include "network/lossy_link.hpp"

#include<cmath>
#include<sstream>
#include<stdexcept>
#include<string>

using namespace std;

namespace{

/** Checks a chance and turns it into a threshold out of 2^53 (exact for 0 and 1). */
uint64_t threshold(double chance, const char* name){
  if (!(chance >= 0.0 && chance <= 1.0)){  // also rejects NaN
    ostringstream message;
    message << "the link's " << name << " chance must be from 0 to 1, not " << chance;
    throw invalid_argument(message.str());
  }
  return static_cast<uint64_t>(ldexp(chance, 53));
}

}  // namespace

LossyLink::LossyLink(const LinkConfig& config)
    : config_(config),
      random_(config.seed),
      loss_(threshold(config.loss, "loss")),
      duplicate_(threshold(config.duplicate, "duplicate")),
      reorder_(threshold(config.reorder, "reorder")){
  for (const auto& [first, last] : config_.drop_ranges){
    if (first == 0 || first > last){
      throw invalid_argument("a drop range must run from seq 1 or later to a seq no lower than its start, not " +
                             to_string(first) + "-" + to_string(last));
    }
  }
}

vector<Event> LossyLink::send(const Event& event){
  vector<Event> arrived;

  bool in_drop_range = false;
  for (const auto& [first, last] : config_.drop_ranges){
    in_drop_range = in_drop_range || (first <= event.seq && event.seq <= last);
  }

  if (in_drop_range || chance(loss_)){
    ++lost_;
  } else if (!held_ && chance(reorder_)){
    held_ = event;  // arrives after the next event
    return arrived;
  } else{
    arrived.push_back(event);
    if (chance(duplicate_)){
      arrived.push_back(event);
    }
  }

  // An event held back last time arrives now, after the later one.
  if (held_){
    arrived.push_back(*held_);
    held_.reset();
  }
  return arrived;
}

vector<Event> LossyLink::flush(){
  vector<Event> arrived;
  if (held_){
    arrived.push_back(*held_);
    held_.reset();
  }
  return arrived;
}
