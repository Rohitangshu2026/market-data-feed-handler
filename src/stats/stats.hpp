// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * The statistics the benchmark reports: a mean with its 95% confidence
 * interval, and a paired comparison of two configurations with a p-value.
 *
 * Every benchmark figure is measured once per seed, and seeds are independent
 * runs, so a mean over seeds with a Student-t interval says how far the
 * figure could move with other seeds. Two configurations run on the same
 * seeds see the same workloads, so their difference is tested seed by seed
 * (a paired t-test), which cancels the variation the seeds share.
 */

#pragma once

#include<cstddef>
#include<vector>

using namespace std;

/** A sample mean with its 95% confidence interval. */
struct Estimate{
  double mean = 0;        /**< the sample mean */
  double half_width = 0;  /**< the 95% interval is mean +- half_width; 0 for fewer than 2 values */
  double sd = 0;          /**< the sample standard deviation; 0 for fewer than 2 values */
  size_t n = 0;           /**< how many values */
};

/** Two configurations compared on the same seeds. */
struct Comparison{
  Estimate a;           /**< configuration A */
  Estimate b;           /**< configuration B */
  Estimate difference;  /**< B minus A, seed by seed */
  double p_value = 1;   /**< two-sided paired t-test of "B and A do not differ" */
};

/**
 * Estimates a mean from independent runs, with a Student-t 95% interval.
 *
 * @param sample the values, one per run
 * @return the mean, its interval, the standard deviation and the count
 * @throws invalid_argument if the sample is empty
 */
Estimate estimate(const vector<double>& sample);

/**
 * Compares two configurations run on the same seeds with a paired t-test on
 * the per-seed differences.
 *
 * @param a configuration A's values, one per seed
 * @param b configuration B's values, for the same seeds in the same order
 * @return both estimates, the estimated difference B - A, and the p-value
 * @throws invalid_argument if the samples differ in size or have fewer than 2 values
 */
Comparison compare_paired(const vector<double>& a, const vector<double>& b);

/**
 * The two-sided p-value of Student's t statistic.
 *
 * @param t the statistic
 * @param degrees_of_freedom its degrees of freedom, above 0
 * @return the chance of a statistic at least as far from 0 as `t` if the true
 *         mean difference is 0
 */
double student_t_p_value(double t, double degrees_of_freedom);

/**
 * The critical value for a two-sided 95% interval.
 *
 * @param degrees_of_freedom the degrees of freedom, at least 1
 * @return the t with a two-sided p-value of exactly 0.05
 */
double student_t_critical_95(double degrees_of_freedom);

/**
 * The p-th percentile, by nearest rank.
 *
 * @param sorted the values, sorted ascending
 * @param p the percentile wanted, from 0 to 100
 * @return the value at that rank, or 0 for an empty list
 */
double percentile(const vector<double>& sorted, double p);
