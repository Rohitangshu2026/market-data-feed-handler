#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rohitangshu Bose
"""Mutation check: plants one bug at a time in a copy of the code and checks
that the test suite catches every one.

Each mutant is a small change of the kind a real bug would be: an off-by-one,
a dropped condition, a skipped clean-up. A mutant the tests miss ("survived")
means a behaviour no test pins down.

    python3 tests/mutants.py [--only=3,8] [extra cmake arguments]

e.g. python3 tests/mutants.py -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/usr/src/googletest
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

# (file, original text, mutated text, what the bug is)
MUTANTS = [
    ("src/handler/feed_handler.cpp", "event.seq == next_seq_ && buffered_.empty()", "event.seq == next_seq_",
     "fast path ignores the events waiting in the buffer"),
    ("src/handler/feed_handler.cpp", "event.seq < next_seq_ ||", "event.seq <= next_seq_ ||",
     "the next expected event is taken for a duplicate"),
    ("src/handler/feed_handler.cpp", "++arrivals_during_gap_ > reorder_window_", "++arrivals_during_gap_ >= reorder_window_",
     "the reorder window is one arrival short"),
    ("src/handler/feed_handler.cpp", "    while (!buffered_.empty()){\n      recover(", "    if (!buffered_.empty()){\n      recover(",
     "every-hole recovery fetches only the first hole"),
    ("src/handler/feed_handler.cpp", "  buffered_.erase(buffered_.begin(), buffered_.lower_bound(next_seq_));\n", "",
     "buffered copies of retransmitted events are kept"),
    ("src/handler/feed_handler.cpp", "buffered_.upper_bound(snapshot.seq)", "buffered_.lower_bound(snapshot.seq)",
     "a snapshot keeps the buffered event at its own seq"),
    ("src/handler/feed_handler.cpp", "if (last_seq < next_seq_){", "if (last_seq <= next_seq_){",
     "the heartbeat misses a single lost event"),
    ("src/handler/feed_handler.cpp", "  if (arrivals_during_gap_ == 0){\n    ++stats_.gaps;  // not", "  if (false){\n    ++stats_.gaps;  // not",
     "a gap only the heartbeat finds is not counted"),
    ("src/book/order_book.cpp", "event.quantity > order->second.quantity", "event.quantity >= order->second.quantity",
     "a fill of the whole order is rejected"),
    ("src/book/order_book.cpp", "sum += mix(hash ^ order.quantity);", "sum += hash;",
     "the fingerprint ignores quantities"),
    ("src/exchange/exchange.cpp", "begin + static_cast<ptrdiff_t>(last - first + 1)", "begin + static_cast<ptrdiff_t>(last - first)",
     "a retransmission leaves out its last event"),
    ("src/exchange/exchange.cpp", "first < history_.front().seq ||", "first + 1 < history_.front().seq ||",
     "the exchange retransmits from before its history"),
    ("src/network/lossy_link.cpp", "  } else if (!held_ && chance(reorder_)){", "  } else if (chance(reorder_)){",
     "the link holds a second event, losing the first"),
    ("src/stats/stats.cpp", "student_t_critical_95(degrees_of_freedom)", "student_t_critical_95(degrees_of_freedom + 1)",
     "the interval uses n degrees of freedom, not n - 1"),
    ("src/cli/options.cpp", "*value < min || *value > max){\n    throw UsageError(\"--\" + name + \": \" + *text",
     "*value < min){\n    throw UsageError(\"--\" + name + \": \" + *text",
     "a number above its option's range is accepted"),
    ("src/cli/options.cpp", "if (given_.contains(name)){", "if (false){",
     "an option given twice is accepted"),
]


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def main():
    only = {int(n) for a in sys.argv[1:] if a.startswith("--only=") for n in a[7:].split(",")}
    cmake_arguments = [a for a in sys.argv[1:] if not a.startswith("--only=")]
    source = pathlib.Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory() as scratch:
        work = pathlib.Path(scratch) / "project"
        shutil.copytree(source, work, ignore=shutil.ignore_patterns("build*", ".git", "docs", "legacy"))
        build = work / "build"
        configured = run(["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release", *cmake_arguments], work)
        if configured.returncode != 0:
            sys.exit("cmake failed:\n" + configured.stdout)
        clean = run(["cmake", "--build", "build", "-j8"], work)
        if clean.returncode != 0 or run(["ctest", "-j8", "--timeout", "60"], build).returncode != 0:
            sys.exit("the unmutated code must build and pass first")

        survived = 0
        tried = 0
        for number, (path, original, mutated, bug) in enumerate(MUTANTS, 1):
            if only and number not in only:
                continue
            tried += 1
            file = work / path
            text = file.read_text()
            if text.count(original) != 1:
                sys.exit(f"mutant {number}: the original text is not in {path} exactly once")
            file.write_text(text.replace(original, mutated))
            try:
                built = run(["cmake", "--build", "build", "-j8"], work)
                tested = run(["ctest", "-j8", "--timeout", "60"], build) if built.returncode == 0 else None
                caught = tested is None or tested.returncode != 0
                failing = sum(1 for line in (tested.stdout if tested else "").splitlines() if "***Failed" in line)
                hung = tested is not None and "Timeout" in tested.stdout
                verdict = "killed" if caught else "SURVIVED"
                how = "does not compile" if tested is None else f"{failing} tests fail" + (", one hangs" if hung else "")
                print(f"  {number:2}. {verdict:8}  {bug}  ({path}; {how})", flush=True)
                survived += not caught
            finally:
                file.write_text(text)
        print(f"{tried - survived} of {tried} mutants killed")
        sys.exit(1 if survived else 0)


if __name__ == "__main__":
    main()
