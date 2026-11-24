// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * Tests for the command-line parser shared by feed_demo and feed_bench
 * (R13): every accepted form of every kind of value, and the message for
 * every kind of mistake. The programs themselves are run end to end by the
 * CTest cases in CMakeLists.txt.
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/options.hpp"

using namespace std;

namespace{

Options parse(const vector<string>& words){
  return Options(words, {"help", "quiet"}, {"seed", "loss", "gap", "list", "chances", "mode"});
}

/** @return the UsageError's message, or "no error" */
string error_of(const function<void()>& action){
  try{
    action();
  } catch (const UsageError& error){
    return error.what();
  }
  return "no error";
}

/** @test T-CL-01 (R13) Values are read from --name=value and --name value; flags, -h and positional words are recognised. */
TEST(Options, ReadsBothFormsOfValueAndFlags){
  const Options options = parse({"burst", "--seed=7", "--loss", "20%", "--quiet", "-h"});
  EXPECT_EQ(options.positional(), (vector<string>{"burst"}));
  EXPECT_EQ(options.integer("seed", 1, 0, 100), 7u);
  EXPECT_DOUBLE_EQ(options.probability("loss", 0), 0.2);
  EXPECT_TRUE(options.has("quiet"));
  EXPECT_TRUE(options.has("help"));
  EXPECT_FALSE(options.has("gap"));
  EXPECT_EQ(options.integer("gap", 42, 0, 100), 42u);  // absent: the fallback
}

/** @test T-CL-02 (R13) Mistakes in the shape of the command line are refused when it is parsed, before anything runs. */
TEST(Options, RefusesMalformedCommandLines){
  EXPECT_EQ(error_of([]{ parse({"--sed", "5"}); }), "unknown option --sed");
  EXPECT_EQ(error_of([]{ parse({"--seed"}); }), "--seed needs a value");
  EXPECT_EQ(error_of([]{ parse({"--seed", "--quiet"}); }), "--seed needs a value");
  EXPECT_EQ(error_of([]{ parse({"--quiet=yes"}); }), "--quiet takes no value");
  EXPECT_EQ(error_of([]{ parse({"--seed=1", "--seed", "2"}); }), "--seed is given twice");
  EXPECT_EQ(error_of([]{ parse({"--=5"}); }), "\"--=5\" is not an option: options look like --name or --name=value");
  EXPECT_EQ(error_of([]{ parse({"a", "b"}).expect_positional(1); }), "unexpected word \"b\"");
}

/** @test T-CL-03 (R13) Whole numbers may use digit separators and exponents; signs, fractions, junk and overflow are refused. */
TEST(Options, ParsesWholeNumbers){
  EXPECT_EQ(parse_whole_number("1000000"), 1'000'000u);
  EXPECT_EQ(parse_whole_number("1'000'000"), 1'000'000u);
  EXPECT_EQ(parse_whole_number("1_000"), 1'000u);
  EXPECT_EQ(parse_whole_number("1e6"), 1'000'000u);
  EXPECT_EQ(parse_whole_number("2.5e3"), 2'500u);
  EXPECT_EQ(parse_whole_number("18446744073709551615"), UINT64_MAX);
  for (const string bad : {"", "-5", "+5", "1.5", "abc", "12abc", " 7", "1e30", "18446744073709551616", "0x10", "nan"}){
    EXPECT_FALSE(parse_whole_number(bad)) << '"' << bad << '"';
  }
}

/** @test T-CL-04 (R13) A whole number outside its option's range is refused, and the message gives the range. */
TEST(Options, RefusesNumbersOutOfRange){
  EXPECT_EQ(error_of([]{ parse({"--seed", "abc"}).integer("seed", 1, 0, 9); }), "--seed: \"abc\" is not a whole number");
  EXPECT_EQ(error_of([]{ parse({"--seed", "10"}).integer("seed", 1, 0, 9); }), "--seed: 10 is out of range; use 0 to 9");
  EXPECT_EQ(error_of([]{ parse({"--seed", "0"}).integer("seed", 1, 1, 9); }), "--seed: 0 is out of range; use 1 to 9");
  EXPECT_EQ(parse({"--seed", "9"}).integer("seed", 1, 0, 9), 9u);  // both ends are included
  EXPECT_EQ(parse({"--seed", "0"}).integer("seed", 1, 0, 9), 0u);
}

/** @test T-CL-05 (R13) Chances are written as fractions or percentages, from 0 to 1 inclusive. */
TEST(Options, ParsesChances){
  EXPECT_DOUBLE_EQ(*parse_probability("0.2"), 0.2);
  EXPECT_DOUBLE_EQ(*parse_probability("20%"), 0.2);
  EXPECT_DOUBLE_EQ(*parse_probability("2.5%"), 0.025);
  EXPECT_DOUBLE_EQ(*parse_probability("1"), 1.0);
  EXPECT_DOUBLE_EQ(*parse_probability("100%"), 1.0);
  EXPECT_DOUBLE_EQ(*parse_probability("0"), 0.0);
  for (const string bad : {"", "%", "1.5", "-0.1", "120%", "abc", "nan", "inf", "0.2 ", "20 %"}){
    EXPECT_FALSE(parse_probability(bad)) << '"' << bad << '"';
  }
  EXPECT_EQ(error_of([]{ parse({"--loss", "1.5"}).probability("loss", 0); }),
            "--loss: \"1.5\" is not a chance from 0 to 1; write it as 0.2 or 20%");
}

/** @test T-CL-06 (R13) Lists are comma separated, and each item is checked; an empty item is an error. */
TEST(Options, ParsesLists){
  EXPECT_EQ(parse({"--list", "0,2,4"}).integers("list", {}, 0, 10), (vector<uint64_t>{0, 2, 4}));
  EXPECT_EQ(parse({"--chances", "0,5%,0.2"}).probabilities("chances", {}), (vector<double>{0, 0.05, 0.2}));
  EXPECT_EQ(error_of([]{ parse({"--list", "1,x"}).integers("list", {}, 0, 10); }),
            "--list: \"x\" is not a whole number; give a list like 1,2,3");
  EXPECT_EQ(error_of([]{ parse({"--list", "1,,2"}).integers("list", {}, 0, 10); }),
            "--list: \"\" is not a whole number; give a list like 1,2,3");
  EXPECT_EQ(error_of([]{ parse({"--list", "1,2,"}).integers("list", {}, 0, 10); }),
            "--list: \"\" is not a whole number; give a list like 1,2,3");
  EXPECT_EQ(error_of([]{ parse({"--list", "1,11"}).integers("list", {}, 0, 10); }),
            "--list: 11 is out of range; use 0 to 10");
  EXPECT_EQ(error_of([]{ parse({"--list="}).integers("list", {}, 0, 10); }), "--list needs a value");
  EXPECT_EQ(error_of([]{ parse({"--chances="}).probabilities("chances", {}); }), "--chances needs a value");
  EXPECT_EQ(error_of([]{ parse({"--chances", "0.1,2"}).probabilities("chances", {}); }),
            "--chances: \"2\" is not a chance from 0 to 1; give a list like 0,5%,0.2");
}

/** @test T-CL-07 (R13) A range of seqs runs from 1 up, first no later than last. */
TEST(Options, ParsesSeqRanges){
  EXPECT_EQ(parse({"--gap", "101-150"}).range("gap", {1, 1}), (pair<uint64_t, uint64_t>{101, 150}));
  EXPECT_EQ(parse({"--gap", "5-5"}).range("gap", {1, 1}), (pair<uint64_t, uint64_t>{5, 5}));
  for (const string bad : {"15-11", "0-5", "5", "a-b", "1-2-3", "-5", "5-"}){
    EXPECT_EQ(error_of([&]{ parse({"--gap", bad}).range("gap", {1, 1}); }),
              "--gap: \"" + bad + "\" is not a range of seqs like 101-150 (from 1 up, first <= last)");
  }
}

/** @test T-CL-08 (R13) A choice must be one of its allowed words, and the message lists them. */
TEST(Options, ParsesChoices){
  EXPECT_EQ(parse({"--mode", "first"}).choice("mode", "every", {"first", "every"}), "first");
  EXPECT_EQ(parse({}).choice("mode", "every", {"first", "every"}), "every");
  EXPECT_EQ(error_of([]{ parse({"--mode", "all"}).choice("mode", "every", {"first", "every"}); }),
            "--mode: \"all\" is not one of: first, every");
}

}  // namespace
