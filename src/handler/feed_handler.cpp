// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

#include "handler/feed_handler.hpp"

#include<utility>

using namespace std;

void FeedHandler::on_event(const Event& event){
  // The common case: the next event in sequence, with nothing waiting.
  if (event.seq == next_seq_ && buffered_.empty()){
    apply(event);
    return;
  }

  // Already applied, or already waiting in the buffer: a duplicate.
  if (event.seq < next_seq_ || buffered_.contains(event.seq)){
    ++stats_.duplicates;
    return;
  }

  buffered_.emplace(event.seq, event);
  apply_buffered();

  if (buffered_.empty()){  // a late event just closed the gap
    arrivals_during_gap_ = 0;
    return;
  }
  // A gap: something before the first buffered event is missing. It is
  // counted when the wait for it starts.
  if (arrivals_during_gap_ == 0){
    ++stats_.gaps;
  }
  // Give reordered events `reorder_window` more arrivals to turn up; after
  // that, recover what is missing.
  if (++arrivals_during_gap_ > reorder_window_){
    if (recovery_ == Recovery::first_hole){
      recover(buffered_.begin()->first - 1);
      return;
    }
    while (!buffered_.empty()){
      recover(buffered_.begin()->first - 1);
    }
  }
}

void FeedHandler::on_heartbeat(uint64_t last_seq){
  if (last_seq < next_seq_){
    return;  // nothing is missing
  }
  if (arrivals_during_gap_ == 0){
    ++stats_.gaps;  // not counted yet: e.g. lost at the end of the stream, which no later event revealed
  }
  // Retransmitting events that are already buffered is harmless: they are
  // the same events, and the buffered copies are dropped.
  recover(last_seq);
}

void FeedHandler::resync(){
  ++stats_.snapshots;
  Snapshot snapshot = exchange_.snapshot();
  book_ = std::move(snapshot.book);
  next_seq_ = snapshot.seq + 1;
  // Buffered events the snapshot already includes are dropped.
  buffered_.erase(buffered_.begin(), buffered_.upper_bound(snapshot.seq));
  arrivals_during_gap_ = 0;
  apply_buffered();
}

void FeedHandler::apply(const Event& event){
  if (!book_.apply(event)){
    ++stats_.rejected;
  }
  ++next_seq_;
}

void FeedHandler::apply_buffered(){
  for (auto next = buffered_.find(next_seq_); next != buffered_.end(); next = buffered_.find(next_seq_)){
    apply(next->second);
    buffered_.erase(next);
  }
}

void FeedHandler::recover(uint64_t last_missing){
  arrivals_during_gap_ = 0;
  const optional<vector<Event>> missing = exchange_.retransmit(next_seq_, last_missing);
  if (!missing){
    resync();  // the range is older than the exchange's history
    return;
  }
  ++stats_.retransmits;
  // The retransmission is seqs next_seq_ to last_missing, in order, so it is
  // applied straight away rather than through the buffer.
  for (const Event& event : *missing){
    apply(event);
  }
  stats_.recovered += missing->size();
  // Copies of those events that had arrived anyway are no longer needed.
  buffered_.erase(buffered_.begin(), buffered_.lower_bound(next_seq_));
  apply_buffered();
}
