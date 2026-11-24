#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rohitangshu Bose
#
# The live demo, one step per Enter. Steps 1 to 7 are the failure cases;
# steps 8 and 9 are the tests: all 110, then a planted bug that they catch.
# Each step shows its command, waits, then runs it. At a prompt, type s to
# skip the step or q to stop. Run it from the repository root, after building:
#
#     scripts/demo.sh [build directory, default build]

set -u
build=${1:-build}
demo=$build/feed_demo
if [ ! -x "$demo" ]; then
  echo "no $demo here: build first, with cmake -S . -B $build && cmake --build $build -j" >&2
  exit 2
fi

step(){
  local title=$1
  shift
  printf '\n== %s\n$ %s ' "$title" "$*"
  read -r answer || exit 0
  case $answer in
    q) exit 0 ;;
    s) return ;;
  esac
  "$@"
}

# The summary lines of a full test run: how many passed, which failed.
tests(){
  ctest --test-dir "$build" -j8 2>&1 | grep -E 'tests passed|tests FAILED|^[[:space:]]+[0-9]+ - |Total Test time'
}

# Deletes one line of clean-up from the handler, rebuilds, and runs the tests,
# which must fail; then puts the line back, rebuilds, and runs them again. The
# file is restored even if the step is interrupted.
plant_bug(){
  local file=src/handler/feed_handler.cpp
  local line='  buffered_.erase(buffered_.begin(), buffered_.lower_bound(next_seq_));'
  if [ "$(grep -cxF -- "$line" "$file")" != 1 ]; then
    echo "the line to delete is not in $file exactly once: skipping"
    return
  fi
  saved=$(mktemp) || return
  cp "$file" "$saved"
  trap 'cp "$saved" src/handler/feed_handler.cpp; rm -f "$saved"; exit 130' INT TERM
  grep -vxF -- "$line" "$saved" > "$file"
  echo "-- the bug: this line deleted from $file"
  diff "$saved" "$file"
  echo "-- rebuilt with the bug; all 110 tests:"
  cmake --build "$build" -j8 >/dev/null 2>&1 && tests
  echo "-- what the property test reports:"
  "$build/feed_tests" --gtest_brief=1 --gtest_filter='Property.HandlerIsNeverWrongAndAlwaysCatchesUp' 2>&1 |
    grep -E 'world [0-9]+:|replay:'
  cp "$saved" "$file"
  rm -f "$saved"
  trap - INT TERM
  echo "-- the line put back; rebuilt; all 110 tests:"
  cmake --build "$build" -j8 >/dev/null 2>&1 && tests
}

clear
step "1. A burst of 5 lost events: wait out the window, then retransmit exactly them" "$demo" burst
step "2. Late and doubled events, none lost: no recovery needed" "$demo" reorder
step "3. The last 3 events lost: only the heartbeat can tell" "$demo" tail
step "4. 400 events lost, but the exchange keeps only 100: a snapshot" "$demo" outage
step "5. Your numbers: it asks for each setting; a random seed; both ends of the stream" "$demo" random --ask --show 5
step "6. Scale: 2,000,000 events at 200,000 a second, one in five lost" "$demo" watch
step "7. Failover between two instances with skewed clocks: physical time against Lamport time" "$demo" lamport
step "8. All 110 tests" ctest --test-dir "$build" -j8
step "9. Plant a bug, and watch the tests catch it" plant_bug
printf '\nThat is the demo.\n'
