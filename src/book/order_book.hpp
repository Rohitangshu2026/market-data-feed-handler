// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * The market data events and the order book they build.
 */

#pragma once

#include<cstdint>
#include<map>
#include<optional>
#include<unordered_map>

using namespace std;

/** Which side of the book an order rests on. */
enum class Side{ bid, ask };

/** What an event does to the book. */
enum class EventType{
  add,      /** a new order rests in the book */
  modify,   /** a resting order's quantity changes; its price stays */
  remove,   /** a resting order is cancelled */
  execute,  /** part or all of a resting order is filled */
};

/**
 * One market data event. The exchange numbers events 1, 2, 3, ... in the
 * order it publishes them; `seq` is how the feed handler spots gaps.
 */
struct Event{
  uint64_t seq = 0;                 /** position in the stream, counted from 1 */
  EventType type = EventType::add;  /** what it does to the book */
  uint64_t order_id = 0;            /** the order it adds or acts on */
  Side side = Side::bid;            /** add only */
  int64_t price = 0;                /** add only, in integer ticks */
  uint64_t quantity = 0;            /** add: size; modify: the new size; execute: the size filled */
  bool operator==(const Event&) const = default;
};

/** A resting order, as the book keeps it. */
struct Order{
  Side side;          /** the side it rests on */
  int64_t price;      /** its price, in ticks */
  uint64_t quantity;  /** what is left of it */
  bool operator==(const Order&) const = default;
};

/** One price level: a price and the total quantity resting there. */
struct Level{
  int64_t price;      /** the level's price, in ticks */
  uint64_t quantity;  /** the total quantity of the orders resting at it */
  bool operator==(const Level&) const = default;
};

/**
 * An order book rebuilt from events: every resting order by id, plus the
 * total quantity at each price on each side, so the best bid and best ask
 * are a map lookup away.
 */
class OrderBook{
 public:
  /**
   * Applies one event. The event's seq is not looked at: putting events in
   * order is the feed handler's job.
   *
   * @param event the event to apply
   * @return true if the book changed; false, with the book left unchanged, if
   *         the event does not fit it: an unknown order, a duplicate id, a zero
   *         quantity, or a fill larger than the order
   */
  bool apply(const Event& event);

  /** @return the highest bid level, or nullopt if no bid rests */
  optional<Level> best_bid() const;

  /** @return the lowest ask level, or nullopt if no ask rests */
  optional<Level> best_ask() const;

  /** @return every resting order, by id, in no particular order */
  const unordered_map<uint64_t, Order>& orders() const{
    return orders_;
  }

  /**
   * A 64-bit fingerprint of the book. Books with the same orders have the same
   * fingerprint, whatever order the orders arrived in; a difference in any
   * order's id, side, price or quantity changes it, except with a chance of
   * about 1 in 2^64. Two books can be shown equal at a glance this way.
   *
   * @return the fingerprint
   */
  uint64_t fingerprint() const;

  /**
   * Two books are equal when they hold the same orders; the price levels
   * follow from the orders.
   */
  bool operator==(const OrderBook& other) const{
    return orders_ == other.orders_;
  }

 private:
  /** @return one side's price levels: price -> total quantity */
  map<int64_t, uint64_t>& levels(Side side){
    return side == Side::bid ? bids_ : asks_;
  }

  /**
   * Changes a resting order's quantity and its level's total. At zero the
   * order leaves the book, and so does its level if nothing else rests there.
   *
   * @param order the order to change
   * @param quantity its new quantity; 0 removes it
   */
  void set_quantity(unordered_map<uint64_t, Order>::iterator order, uint64_t quantity);

  unordered_map<uint64_t, Order> orders_;  /**< by order id */
  map<int64_t, uint64_t> bids_;  /**< price -> total quantity; the best bid is the highest price */
  map<int64_t, uint64_t> asks_;  /**< price -> total quantity; the best ask is the lowest price */
};
