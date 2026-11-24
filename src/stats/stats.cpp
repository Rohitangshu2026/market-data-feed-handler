// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

#include "stats/stats.hpp"

#include<algorithm>
#include<cmath>
#include<stdexcept>

using namespace std;

namespace{

/** The continued fraction for the incomplete beta function, by Lentz's method. */
double beta_fraction(double a, double b, double x){
  constexpr double tiny = 1e-300;
  const auto guard = [](double v){ return fabs(v) < tiny ? tiny : v; };
  double c = 1.0;
  double d = 1.0 / guard(1.0 - (a + b) * x / (a + 1.0));
  double h = d;
  for (int m = 1; m <= 500; ++m){
    const double even = m * (b - m) * x / ((a - 1.0 + 2 * m) * (a + 2 * m));
    d = 1.0 / guard(1.0 + even * d);
    c = guard(1.0 + even / c);
    h *= d * c;
    const double odd = -(a + m) * (a + b + m) * x / ((a + 2 * m) * (a + 1.0 + 2 * m));
    d = 1.0 / guard(1.0 + odd * d);
    c = guard(1.0 + odd / c);
    h *= d * c;
    if (fabs(d * c - 1.0) < 1e-15){
      break;
    }
  }
  return h;
}

/** The regularized incomplete beta function I_x(a, b). */
double incomplete_beta(double a, double b, double x){
  if (x <= 0.0){
    return 0.0;
  }
  if (x >= 1.0){
    return 1.0;
  }
  const double front = exp(lgamma(a + b) - lgamma(a) - lgamma(b) + a * log(x) + b * log1p(-x));
  if (x < (a + 1.0) / (a + b + 2.0)){
    return front * beta_fraction(a, b, x) / a;
  }
  return 1.0 - front * beta_fraction(b, a, 1.0 - x) / b;
}

}  // namespace

double student_t_p_value(double t, double degrees_of_freedom){
  return incomplete_beta(degrees_of_freedom / 2.0, 0.5, degrees_of_freedom / (degrees_of_freedom + t * t));
}

double student_t_critical_95(double degrees_of_freedom){
  double low = 0.0, high = 1'000.0;  // the p-value falls as t grows; bisect for 0.05
  for (int step = 0; step < 200; ++step){
    const double middle = (low + high) / 2.0;
    (student_t_p_value(middle, degrees_of_freedom) > 0.05 ? low : high) = middle;
  }
  return (low + high) / 2.0;
}

Estimate estimate(const vector<double>& sample){
  if (sample.empty()){
    throw invalid_argument("cannot estimate a mean from no values");
  }
  Estimate result;
  result.n = sample.size();
  for (const double value : sample){
    result.mean += value;
  }
  result.mean /= static_cast<double>(result.n);
  if (result.n < 2){
    return result;
  }
  double squares = 0.0;
  for (const double value : sample){
    squares += (value - result.mean) * (value - result.mean);
  }
  const double degrees_of_freedom = static_cast<double>(result.n - 1);
  result.sd = sqrt(squares / degrees_of_freedom);
  result.half_width = student_t_critical_95(degrees_of_freedom) * result.sd / sqrt(static_cast<double>(result.n));
  return result;
}

Comparison compare_paired(const vector<double>& a, const vector<double>& b){
  if (a.size() != b.size()){
    throw invalid_argument("a paired comparison needs one value per seed from each configuration");
  }
  if (a.size() < 2){
    throw invalid_argument("a paired comparison needs at least 2 seeds");
  }
  vector<double> differences(a.size());
  for (size_t i = 0; i < a.size(); ++i){
    differences[i] = b[i] - a[i];
  }
  Comparison result{estimate(a), estimate(b), estimate(differences), 1.0};
  const Estimate& d = result.difference;
  if (d.sd == 0.0){
    result.p_value = d.mean == 0.0 ? 1.0 : 0.0;  // every seed moved by exactly the same amount
  } else{
    const double t = d.mean / (d.sd / sqrt(static_cast<double>(d.n)));
    result.p_value = student_t_p_value(t, static_cast<double>(d.n - 1));
  }
  return result;
}

double percentile(const vector<double>& sorted, double p){
  if (sorted.empty()){
    return 0.0;
  }
  const auto rank = static_cast<size_t>(ceil(p / 100.0 * static_cast<double>(sorted.size())));
  return sorted[min(max<size_t>(rank, 1), sorted.size()) - 1];
}
