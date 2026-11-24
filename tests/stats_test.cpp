// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Tests for the statistics behind the bench's confidence intervals and
 * comparisons (R14). The reference values come from SciPy 1.18
 * (scipy.stats.t.ppf, scipy.stats.t.sf and scipy.stats.ttest_rel).
 */

#include <cmath>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "stats/stats.hpp"

using namespace std;

namespace{

/** @test T-ST-01 (R14) The two-sided 95% critical value of Student's t matches SciPy's t.ppf(0.975, df). */
TEST(Stats, CriticalValuesMatchReference){
  const pair<double, double> reference[] = {{1, 12.706205}, {2, 4.302653},  {4, 2.776445},  {9, 2.262157},
                                            {10, 2.228139}, {19, 2.093024}, {29, 2.045230}, {30, 2.042272},
                                            {99, 1.984217}, {1000, 1.962339}};
  for (const auto& [df, t] : reference){
    EXPECT_NEAR(student_t_critical_95(df), t, 1e-5) << "df " << df;
  }
}

/** @test T-ST-02 (R14) Two-sided p-values match SciPy's 2 * t.sf(|t|, df). */
TEST(Stats, PValuesMatchReference){
  EXPECT_NEAR(student_t_p_value(2.228, 10), 0.0500118, 1e-6);
  EXPECT_NEAR(student_t_p_value(0.5, 3), 0.6514480, 1e-6);
  EXPECT_NEAR(student_t_p_value(10, 2), 0.0098525, 1e-6);
  EXPECT_NEAR(student_t_p_value(-10, 2), 0.0098525, 1e-6);  // two-sided: the sign does not matter
  EXPECT_NEAR(student_t_p_value(1.96, 1e6), 0.0499961, 1e-6);
  EXPECT_DOUBLE_EQ(student_t_p_value(0, 5), 1.0);
  EXPECT_DOUBLE_EQ(student_t_p_value(INFINITY, 5), 0.0);
}

/** @test T-ST-03 (R14) The mean, standard deviation and 95% half-width of 1..5 are 3, 1.5811 and 1.9632. */
TEST(Stats, EstimateGivesMeanAndConfidenceInterval){
  const Estimate e = estimate({1, 2, 3, 4, 5});
  EXPECT_EQ(e.n, 5u);
  EXPECT_DOUBLE_EQ(e.mean, 3.0);
  EXPECT_NEAR(e.sd, 1.5811388, 1e-6);
  EXPECT_NEAR(e.half_width, 1.9632432, 1e-5);
}

/** @test T-ST-04 (R14) One value has no spread to estimate, and no values is an error. */
TEST(Stats, EstimateEdgeCases){
  const Estimate one = estimate({7});
  EXPECT_DOUBLE_EQ(one.mean, 7.0);
  EXPECT_DOUBLE_EQ(one.half_width, 0.0);
  EXPECT_THROW(estimate({}), invalid_argument);
}

/** @test T-ST-05 (R14) A paired comparison matches SciPy's ttest_rel: t = 6.3246, p = 0.0014566 for these six pairs. */
TEST(Stats, PairedComparisonMatchesReference){
  const Comparison c = compare_paired({10.1, 9.8, 10.4, 10.0, 9.9, 10.3}, {10.6, 10.1, 10.9, 10.2, 10.5, 10.6});
  EXPECT_NEAR(c.difference.mean, 0.4, 1e-9);
  EXPECT_NEAR(c.p_value, 0.0014566, 1e-6);
  EXPECT_NEAR(c.a.mean, 10.083333, 1e-6);
  EXPECT_NEAR(c.b.mean, 10.483333, 1e-6);
}

/** @test T-ST-06 (R14) Identical runs differ with p = 1; a constant nonzero shift is certain; mismatched or single pairs are errors. */
TEST(Stats, PairedComparisonEdgeCases){
  EXPECT_DOUBLE_EQ(compare_paired({1, 2, 3}, {1, 2, 3}).p_value, 1.0);
  EXPECT_DOUBLE_EQ(compare_paired({1, 2, 3}, {2, 3, 4}).p_value, 0.0);
  EXPECT_THROW(compare_paired({1, 2}, {1, 2, 3}), invalid_argument);
  EXPECT_THROW(compare_paired({1}, {2}), invalid_argument);
}

/** @test T-ST-07 (R14) Percentiles are by nearest rank: p50 of 1..10 is 5, p99 is 10, p0 is the minimum. */
TEST(Stats, PercentileIsByNearestRank){
  const vector<double> sorted = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_DOUBLE_EQ(percentile(sorted, 50), 5);
  EXPECT_DOUBLE_EQ(percentile(sorted, 90), 9);
  EXPECT_DOUBLE_EQ(percentile(sorted, 99), 10);
  EXPECT_DOUBLE_EQ(percentile(sorted, 0), 1);
  EXPECT_DOUBLE_EQ(percentile({}, 50), 0);
}

}  // namespace
