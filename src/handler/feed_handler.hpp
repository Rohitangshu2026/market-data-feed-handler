// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * The feed handler: consumes the sequenced stream, rebuilds the order book,
 * and detects and recovers from gaps.
 */

#pragma once

#include<cstddef>
#include<cstdint>
#include<map>

#include "exchange/exchange.hpp"
#include "book/order_book.hpp"

using namespace std;

/** What the handler has done so far. */
struct HandlerStats{
  uint64_t duplicates = 0;   /**< events already applied or already buffered */
  uint64_t gaps = 0;         /**< gaps seen, including ones a late event filled */
  uint64_t retransmits = 0;  /**< recoveries by retransmission */
  uint64_t recovered = 0;    /**< events applied from retransmissions */
  uint64_t snapshots = 0;    /**< recoveries by snapshot */
  uint64_t rejected = 0;     /**< events the book refused; stays zero unless something is broken */
};

/** Which missing events the handler asks for once a gap is taken as loss. */
enum class Recovery{
  first_hole,  /**< only the hole before the first buffered event; later holes wait their turn */
  every_hole,  /**< every hole below the newest buffered event, one retransmission each */
};

/**
 * Consumes the sequenced stream, rebuilds the order book, and detects and
 * recovers from gaps.
 *
 * How it works:
 *
 *   - Events are applied strictly in sequence order, so the book is always
 *     the exchange's book as of some seq, never a mix.
 *   - An event that arrives ahead of the next expected seq means a gap: it
 *     waits in a buffer.
 *   - A gap is usually a reordered event that arrives a moment later, so the
 *     handler waits for `reorder_window` more arrivals. If the gap is still
 *     open by then, the missing events are taken as lost and recovered.
 *   - Recovery asks the exchange to retransmit the missing range. If the
 *     exchange no longer keeps that range, the handler takes a snapshot of
 *     the whole book instead and continues from there.
 *   - Loss of the last events before the stream goes quiet shows up only
 *     when the exchange's heartbeat reports a higher last seq.
 */
class FeedHandler{
 public:
  /**
   * @param exchange where retransmissions and snapshots come from
   * @param reorder_window how many more arrivals a gap waits for before it is
   *        taken as loss; 0 recovers at the first sign of a gap
   * @param recovery which holes to recover once a gap is taken as loss
   */
  explicit FeedHandler(const Exchange& exchange, size_t reorder_window = 3, Recovery recovery = Recovery::every_hole)
      : exchange_(exchange), reorder_window_(reorder_window), recovery_(recovery){}

  /**
   * Takes one event as it arrives from the network: possibly late, early or
   * twice. Applies it, buffers it or drops it, and recovers missing events
   * once a gap has outlasted the reorder window.
   *
   * @param event the arriving event
   */
  void on_event(const Event& event);

  /**
   * Takes the exchange's heartbeat, sent while the stream is idle, and
   * recovers anything up to it that has not arrived.
   *
   * @param last_seq the exchange's latest seq
   */
  void on_heartbeat(uint64_t last_seq);

  /**
   * Replaces the book with a snapshot and continues after its seq: how a
   * handler joins a stream mid-way, and the fallback when retransmission fails.
   */
  void resync();

  /** @return the rebuilt book: the exchange's book as of last_applied() */
  const OrderBook& book() const{
    return book_;
  }

  /** @return the seq of the last event applied to the book, 0 before the first */
  uint64_t last_applied() const{
    return next_seq_ - 1;
  }

  /** @return how many events wait in the buffer for a gap to close */
  size_t buffered() const{
    return buffered_.size();
  }

  /** @return what the handler has done so far */
  const HandlerStats& stats() const{
    return stats_;
  }

  /** @return how many more arrivals a gap waits for before it is taken as loss */
  size_t reorder_window() const{
    return reorder_window_;
  }

  /** @return which holes the handler recovers once a gap is taken as loss */
  Recovery recovery() const{
    return recovery_;
  }

 private:
  /** Applies the next event in sequence to the book. */
  void apply(const Event& event);

  /** Applies buffered events for as long as the next one in sequence is there. */
  void apply_buffered();

  /**
   * Fills the missing range [next_seq_, last_missing] by retransmission, or by
   * a snapshot if the exchange no longer keeps all of it.
   *
   * @param last_missing the last seq of the range
   */
  void recover(uint64_t last_missing);

  const Exchange& exchange_;
  size_t reorder_window_;
  Recovery recovery_;
  OrderBook book_;
  uint64_t next_seq_ = 1;  /**< the seq to apply next */
  map<uint64_t, Event> buffered_;  /**< events that arrived ahead of next_seq_, by seq */
  size_t arrivals_during_gap_ = 0;  /**< arrivals since the open gap was seen; recovery starts past reorder_window_ */
  HandlerStats stats_;
};
