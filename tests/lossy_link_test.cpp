// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Unit tests for the lossy link (R12, R15), the test harness's source of
 * damage. Exact behaviour is checked with chances of 0 and 1, which leave
 * nothing to chance; the rates in between are checked statistically, against
 * a tolerance of four standard errors.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "network/lossy_link.hpp"

using namespace std;

namespace{

Event numbered(uint64_t seq){
  Event event{};
  event.seq = seq;
  return event;
}

/** @return the seqs that arrive when seqs 1 to `count` are sent, flush included */
vector<uint64_t> arrivals(LossyLink& link, uint64_t count){
  vector<uint64_t> seqs;
  for (uint64_t seq = 1; seq <= count; ++seq){
    for (const Event& event : link.send(numbered(seq))){
      seqs.push_back(event.seq);
    }
  }
  for (const Event& event : link.flush()){
    seqs.push_back(event.seq);
  }
  return seqs;
}

LinkConfig config(double loss, double duplicate, double reorder, uint64_t seed = 1){
  LinkConfig c;
  c.loss = loss;
  c.duplicate = duplicate;
  c.reorder = reorder;
  c.seed = seed;
  return c;
}

/** @test T-LL-01 (R12) A link with no damage delivers every event once, in order. */
TEST(LossyLink, UndamagedLinkDeliversEverythingInOrder){
  LossyLink link(config(0, 0, 0));
  const vector<uint64_t> seqs = arrivals(link, 1'000);
  ASSERT_EQ(seqs.size(), 1'000u);
  for (size_t i = 0; i < seqs.size(); ++i){
    ASSERT_EQ(seqs[i], i + 1);
  }
  EXPECT_EQ(link.lost(), 0u);
}

/** @test T-LL-02 (R12) Chances below 0, above 1 or not a number are refused; 0 and 1 themselves are fine. */
TEST(LossyLink, RejectsChancesOutsideZeroToOne){
  const double nan = numeric_limits<double>::quiet_NaN();
  for (const double bad : {-0.01, 1.01, nan}){
    EXPECT_THROW(LossyLink(config(bad, 0, 0)), invalid_argument);
    EXPECT_THROW(LossyLink(config(0, bad, 0)), invalid_argument);
    EXPECT_THROW(LossyLink(config(0, 0, bad)), invalid_argument);
  }
  EXPECT_NO_THROW(LossyLink(config(0, 1, 1)));
  EXPECT_NO_THROW(LossyLink(config(1, 0, 0)));
}

/** @test T-LL-03 (R12) Drop ranges drop exactly their seqs, both ends included; a range from 0 or a reversed range is refused. */
TEST(LossyLink, DropRangesDropExactlyTheirSeqs){
  LinkConfig c = config(0, 0, 0);
  c.drop_ranges = {{3, 5}, {9, 9}};
  LossyLink link(c);
  EXPECT_EQ(arrivals(link, 10), (vector<uint64_t>{1, 2, 6, 7, 8, 10}));
  EXPECT_EQ(link.lost(), 4u);

  c.drop_ranges = {{0, 4}};
  EXPECT_THROW(LossyLink{c}, invalid_argument);
  c.drop_ranges = {{5, 4}};
  EXPECT_THROW(LossyLink{c}, invalid_argument);
}

/** @test T-LL-04 (R12) With certain loss nothing arrives, and every event is counted lost. */
TEST(LossyLink, CertainLossLosesEverything){
  LossyLink link(config(1, 0, 0));
  EXPECT_TRUE(arrivals(link, 100).empty());
  EXPECT_EQ(link.lost(), 100u);
}

/** @test T-LL-05 (R12) With certain duplication every event arrives twice, back to back. */
TEST(LossyLink, CertainDuplicationDeliversEveryEventTwice){
  LossyLink link(config(0, 1, 0));
  EXPECT_EQ(arrivals(link, 3), (vector<uint64_t>{1, 1, 2, 2, 3, 3}));
}

/** @test T-LL-06 (R12) A reordered event arrives one place late; flush() releases one still held at the end. */
TEST(LossyLink, ReorderedEventArrivesOnePlaceLate){
  LossyLink link(config(0, 0, 1));  // holds every event it can: one at a time
  EXPECT_EQ(arrivals(link, 5), (vector<uint64_t>{2, 1, 4, 3, 5}));
}

/** @test T-LL-07 (R12) A held event is still released when the event after it is lost. */
TEST(LossyLink, HeldEventIsReleasedWhenTheNextIsLost){
  LinkConfig c = config(0, 0, 1);
  c.drop_ranges = {{2, 2}};
  LossyLink link(c);
  EXPECT_EQ(arrivals(link, 4), (vector<uint64_t>{1, 4, 3}));
}

/** @test T-LL-08 (R12) Measured rates of loss, reordering and duplication match the configured chances, within four standard errors, for five seeds. Each rate is counted over the events it can apply to: a held event cannot be doubled, and nothing is held while another event is. */
TEST(LossyLink, MeasuredRatesMatchTheConfiguredChances){
  constexpr uint64_t n = 200'000;
  const double loss = 0.10, reorder = 0.08, duplicate = 0.05;
  for (uint64_t seed = 1; seed <= 5; ++seed){
    LossyLink link(config(loss, duplicate, reorder, seed));
    double could_hold = 0, held = 0, could_double = 0, doubled = 0;
    bool holding = false;
    for (uint64_t seq = 1; seq <= n; ++seq){
      const uint64_t lost_before = link.lost();
      const vector<Event> events = link.send(numbered(seq));
      const auto copies = count_if(events.begin(), events.end(), [&](const Event& e){ return e.seq == seq; });
      const bool lost = link.lost() > lost_before;
      if (!lost){
        could_hold += !holding;
        held += copies == 0;
        could_double += copies > 0;
        doubled += copies == 2;
      }
      holding = !lost && copies == 0;
    }
    SCOPED_TRACE("seed " + to_string(seed));
    const auto within = [](double hits, double trials, double p){
      EXPECT_NEAR(hits / trials, p, 4 * sqrt(p * (1 - p) / trials));
    };
    within(static_cast<double>(link.lost()), n, loss);
    within(held, could_hold, reorder);
    within(doubled, could_double, duplicate);
  }
}

/** @test T-LL-09 (R15) The same seed damages the stream the same way; another seed damages it differently. */
TEST(LossyLink, SameSeedSameDamage){
  LossyLink a(config(0.2, 0.1, 0.1, 9)), b(config(0.2, 0.1, 0.1, 9)), c(config(0.2, 0.1, 0.1, 10));
  const vector<uint64_t> first = arrivals(a, 10'000);
  EXPECT_EQ(first, arrivals(b, 10'000));
  EXPECT_NE(first, arrivals(c, 10'000));
}

}  // namespace
