// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Command-line options shared by the programs: `--name value`, `--name=value`,
 * bare flags such as `--quiet`, and positional words such as a scenario name.
 * Each value is checked when the program reads it, and every error names the
 * option and says what was wrong, so a typo never runs with a silent default.
 */

#pragma once

#include<cstdint>
#include<map>
#include<optional>
#include<set>
#include<stdexcept>
#include<string>
#include<utility>
#include<vector>

using namespace std;

/** A command line that cannot be used as given. Its message is written for the user. */
class UsageError : public runtime_error{
 public:
  using runtime_error::runtime_error;
};

/**
 * A parsed command line. The program names every option it knows when it
 * parses, so a misspelt option is an error at once instead of being ignored;
 * it then asks for each value with a default and the values it accepts.
 */
class Options{
 public:
  /**
   * Splits a command line into options and positional words.
   *
   * @param arguments the words after the program's name
   * @param flags the options that take no value, such as `quiet`; `-h` is
   *        read as `--help`
   * @param valued the options that take a value, such as `seed`
   * @throws UsageError if an option is not one of `flags` or `valued`, is given
   *         twice, gives a flag a value, or gives no value where one is needed
   */
  Options(const vector<string>& arguments, const set<string>& flags, const set<string>& valued);

  /** @return the words that are not options, in order */
  const vector<string>& positional() const{
    return positional_;
  }

  /**
   * @param name the option or flag, without `--`
   * @return whether it was given
   */
  bool has(const string& name) const{
    return given_.contains(name);
  }

  /**
   * @param name the option, without its dashes
   * @param fallback what to return when the option is not given
   * @return the option's value as written
   */
  string text(const string& name, const string& fallback) const{
    return value(name).value_or(fallback);
  }

  /**
   * A whole number, written `1000000`, `1e6`, `1'000'000` or `1_000_000`.
   *
   * @param name the option, without `--`
   * @param fallback the value if the option is absent
   * @param min the smallest value accepted
   * @param max the largest value accepted
   * @return the value
   * @throws UsageError if the value is not a whole number from `min` to `max`
   */
  uint64_t integer(const string& name, uint64_t fallback, uint64_t min, uint64_t max) const;

  /**
   * A chance, written `0.2` or `20%`.
   *
   * @param name the option, without `--`
   * @param fallback the value if the option is absent
   * @return the value, from 0 to 1
   * @throws UsageError if the value is not a chance from 0 to 1
   */
  double probability(const string& name, double fallback) const;

  /**
   * A comma-separated list of whole numbers, such as `0,1,3,8`.
   *
   * @param name the option, without `--`
   * @param fallback the list if the option is absent
   * @param min the smallest value accepted
   * @param max the largest value accepted
   * @return the values, in the order given
   * @throws UsageError if an item is not a whole number from `min` to `max`
   */
  vector<uint64_t> integers(const string& name, const vector<uint64_t>& fallback, uint64_t min, uint64_t max) const;

  /**
   * A comma-separated list of chances, such as `0,5%,0.2`.
   *
   * @param name the option, without `--`
   * @param fallback the list if the option is absent
   * @return the values, in the order given
   * @throws UsageError if an item is not a chance from 0 to 1
   */
  vector<double> probabilities(const string& name, const vector<double>& fallback) const;

  /**
   * A range of seqs, written `101-150`.
   *
   * @param name the option, without `--`
   * @param fallback the range if the option is absent
   * @return the first and last seq of the range
   * @throws UsageError unless the value is two whole numbers with
   *         1 <= first <= last
   */
  pair<uint64_t, uint64_t> range(const string& name, pair<uint64_t, uint64_t> fallback) const;

  /**
   * One of a fixed set of words.
   *
   * @param name the option, without `--`
   * @param fallback the word if the option is absent
   * @param allowed the words accepted
   * @return the word
   * @throws UsageError if the value is not one of `allowed`
   */
  string choice(const string& name, const string& fallback, const vector<string>& allowed) const;

  /**
   * Checks the number of positional words.
   *
   * @param max_positional how many positional words the program takes
   * @throws UsageError naming the first word beyond `max_positional`
   */
  void expect_positional(size_t max_positional) const;

 private:
  /** @return the option's value, or nullopt if it was not given */
  optional<string> value(const string& name) const;

  map<string, optional<string>> given_;  /**< option -> its value; nullopt for a flag */
  vector<string> positional_;
};

/**
 * Parses a whole number the way Options::integer() does.
 *
 * @param text the number as written
 * @return the number, or nullopt if `text` is not a whole number that fits in 64 bits
 */
optional<uint64_t> parse_whole_number(const string& text);

/**
 * Parses a chance the way Options::probability() does.
 *
 * @param text the chance as written: `0.2` or `20%`
 * @return the chance, or nullopt if `text` is not a number from 0 to 1
 */
optional<double> parse_probability(const string& text);
