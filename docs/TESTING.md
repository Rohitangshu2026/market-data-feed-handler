# Testing the feed handler

This is the test plan: what the program must do, how each requirement is tested, and how well the
tests are known to work. Every number below can be reproduced with the commands in the last section.

## The tests in plain words

A test is a small program that runs a piece of the program on a situation chosen in advance and compares
the result with an answer worked out independently. If they differ, the test fails and says where.
There are 110 of them, they run in about two seconds, and they run after every change, so a mistake
is caught minutes after it is made rather than in front of an audience.

They work at five levels, from the smallest piece to the finished program:

1. **Unit tests check one part on its own**, against answers worked out by hand. Put an order to buy
   10 at 99 and another to buy 5 at 99 into the book: it must show 15 at 99. Fill the first order
   completely: it must show 5. Try to fill more than is left: the book must refuse, and stay exactly as
   it was. The exchange, the lossy link, the Lamport clock, the command-line parser and the statistics
   each get the same treatment.
2. **Structural tests make every decision inside the handler happen at least once.** The handler has a
   handful of branches: an event in order, a duplicate, an early event, a gap that waits, a recovery, a
   snapshot, a heartbeat. Each structural test feeds it events in one exact order, chosen to take one
   branch, and checks what it did. For example: deliver events 1, 2, 4, 6, 8 and 9, so 3, 5 and 7 are
   missing; with a reorder window of 3, the arrival of 9 must start recovery, and afterwards the book
   must be at 9.
3. **Functional tests check one requirement each on a whole stream**, without looking inside: lose
   events 101 to 150 of 1,000, and at the end the book must equal the exchange's, after exactly one
   retransmission of exactly 50 events.
4. **Property tests try hundreds of random situations and check rules that must hold in all of
   them.** Each "world" is drawn from a seed: how long the stream is, how much is lost, doubled or
   reordered, how much history the exchange keeps, how big the book may grow. After **every single
   arrival** they check the main promise: the handler's book equals the exchange's book as it was at
   the handler's position in the stream. Never wrong, at most behind. A failure prints its seed and the
   one command that replays it.
5. **End-to-end tests run the finished programs as a person would**, including typing answers at the
   prompts, some of them wrong on purpose, and check what the programs print and how they exit. Good
   input must end with "matches the exchange's own book"; bad input with a clear message, and the
   question asked again.

Then three checks on the tests themselves:

- **Coverage** measures how much of the code the tests actually run: 98% of the lines and 95% of the
  decisions. What is left are safety checks that only a bug could reach.
- **Mutation testing** asks whether the tests would notice a bug. A script plants 16 realistic bugs one
  at a time (a reorder window one arrival short, a recovery that fetches only one of several holes, a
  retransmission missing its last event, and so on) and runs the tests against each. All 16 are
  caught.
- **Sanitizers** rerun everything with the compiler's checks for memory errors and undefined behaviour
  switched on. Reading past the end of an array, using freed memory or overflowing a number would fail
  the run.

## 1. Requirements

The feed handler's job, and the support code the tests rely on, stated so that each can be tested.

| ID | Requirement | Kind |
| --- | --- | --- |
| R1 | An event that arrives in sequence, with nothing waiting, is applied at once. | functional |
| R2 | A duplicate (an event already applied, or already waiting) is dropped, never applied twice. | functional |
| R3 | An event that arrives early waits in a reorder buffer and is applied, in seq order, once the events before it arrive. Reordering that resolves within the reorder window needs no recovery. | functional |
| R4 | A gap still open after the reorder window (W further arrivals) is recovered by retransmitting exactly the missing range. | functional |
| R5 | A gap the exchange can no longer retransmit (older than its history) is recovered from a snapshot of the exchange's book. | functional |
| R6 | Loss at the end of the stream, which no later event reveals, is found by the exchange's heartbeat. | functional |
| R7 | **Correctness.** After any mix of loss, duplication and reordering, the handler's book equals the exchange's. At every moment before that, it equals the exchange's book as of the handler's last applied seq: never wrong, at most behind. | functional |
| R8 | Both recovery policies, first-hole and every-hole, meet R4 to R7. | functional |
| R9 | Lamport timestamps order the book updates of a handoff between two instances correctly for any clock skew; physical timestamps misorder them once the skew exceeds the handoff latency. | functional |
| R10 | The exchange publishes valid events numbered 1, 2, 3, ..., determined by its seed; retransmits exactly what it published, within its history; and snapshots its book. | component |
| R11 | The order book applies add, modify, remove and execute, and rejects an event that does not fit, unchanged. | component |
| R12 | The lossy link loses, duplicates and reorders events at the configured rates, and drops exactly the configured seq ranges. | component |
| R13 | Every input, on the command line or typed at a prompt, is checked before anything runs. A bad one gets a message naming the setting and what it accepts: on the command line, exit status 2; at a prompt, the question again. | interface |
| R14 | The statistics behind the bench (mean, 95% confidence interval, paired t-test) agree with reference values. | analysis |
| R15 | **Reproducibility.** The same seed and settings give the same events, damage and results, on every platform. Every run prints its seed, a fresh random one unless one was given, and the command that replays it. | non-functional |
| R16 | **Memory safety.** The whole suite runs clean under AddressSanitizer and UndefinedBehaviorSanitizer. | non-functional |

Speed and latency are measured, not pass/fail: see [PERFORMANCE.md](PERFORMANCE.md).

## 2. How each kind of test works

| Level | What it checks | Where | Count |
| --- | --- | --- | ---: |
| Unit | one component against hand-worked values | `order_book_test`, `exchange_test`, `lossy_link_test`, `lamport_test`, `cli_test`, `stats_test` | 51 |
| Structural (white-box) | each branch of the handler, driven by events in an exact order | `feed_handler_test`, suite `FeedHandlerPaths` | 11 |
| Functional (black-box) | one requirement per test, on whole streams through the lossy link | `feed_handler_test`, suite `FeedHandler` | 6 |
| Property | invariants over random worlds, each from one seed | `property_test` | 2 |
| End to end | the programs themselves, from the command line and at the prompts: exit status and output, good input and bad | `CMakeLists.txt`, `expect()` and `answer()` | 40 |
| | | **total, run by `ctest`** | **110** |

**Unit tests** check against arithmetic, not against the code: a level's total is the sum worked out by
hand, a retransmission is compared with the events recorded when they were published.

**Structural tests** cover every path through `FeedHandler::on_event`, `recover`, `resync` and
`on_heartbeat`: the in-order fast path; both kinds of duplicate (already applied, already buffered); a
late event closing a gap; the reorder window, checked to be exact for W = 0, 1, 2, 3, 5 and 8; both
recovery policies, which differ in when later holes are fetched; the fall-back from a refused
retransmission to a snapshot; a late original of an event already recovered; and the heartbeat with
nothing missing, with the tail missing, and with a gap already open.

**Functional tests** are written from the requirements alone: lose seqs 101-150 and expect one
retransmission of exactly 50 events; reorder 20% and duplicate 10% and expect no retransmission at all;
lose the last 5 events and expect only the heartbeat to find them; lose 400 events with 100 kept and
expect a snapshot; 20 seeds of mixed damage under each policy.

**Property tests** build a random world from each seed: stream length, history, book size, window,
policy, loss, duplication, reordering and lost bursts. In every world they check, after **every**
arrival, that the handler's book is the exchange's book as of the handler's last applied seq (the
fingerprint of the exchange's book is recorded after each event, as the truth), that the last applied
seq never goes down, that no event is rejected, and at the end that the handler has caught up exactly.
A second test runs worlds twice and expects identical outcomes. 200 worlds run by default; any number
can be run, and a failure prints its world and the command that replays it:

```bash
FEED_SEED=1 FEED_SEEDS=5000 ./build/feed_tests --gtest_filter='Property.*'
```

**Statistical tests.** The link's rates are checked over 200,000 events for each of 5 seeds: the
measured rate must lie within four standard errors, 4 sqrt(p(1-p)/n), of the configured chance, each
counted over the events it can apply to (a held event cannot be doubled; nothing is held while another
event is). Chances of 0 and 1 are tested exactly.

**Reference values.** The statistics are checked against SciPy 1.18: `t.ppf(0.975, df)` for ten
degrees of freedom, two-sided p-values from `t.sf`, and a paired comparison from `ttest_rel`.

**Regression pin.** `T-EX-09` pins the fingerprint of the book after 100,000 events of seed 1. If the
generator or the random draws ever change, every published number changes with them, so the test fails
until the change is made deliberately.

**End-to-end tests** run `feed_demo` and `feed_bench` as a user would. Each scenario must exit 0
with the line that proves it worked (`retransmitted`, `snapshot`, `matches the exchange's own book`);
each kind of bad input must exit 2 with its message: a non-number, a chance above 1, a value out of
range, an unknown option, a missing value, an option given twice, a value given to a flag, an unknown
scenario, a stray word, a reversed range, a gap past the end of the stream, a directory that cannot be
written. One test checks that `feed_demo all` validates all six scenarios' settings before running
any of them. Five more run the demo's prompts with answers read from `tests/answers/`: a whole session;
wrong answers (an unknown scenario, a non-number, a gap past the end of the stream), each explained and
asked again; quitting with `q`; the Lamport scenario; and command-line values offered as the defaults.
Others check that a run without `--seed` prints a random seed and the command that replays it, and that
`--show` prints the events at both ends with what happened to each.

**Test design techniques.** Boundary values: the window at 0 and its exact edge, the oldest event
kept in the history and the one before it, both ends of every numeric range, chances of exactly 0 and
1. Equivalence classes: for each option, a valid value, each kind of invalid value, and absence.
State coverage: every transition in the handler's state diagram (README).

## 3. Traceability

Every requirement, and the tests that check it. Test IDs are in each test's `@test` comment, so the
generated API documentation has a Test List page with all of them.

| Requirement | Tests |
| --- | --- |
| R1 in order | T-FH-01, T-FH-12 |
| R2 duplicates | T-FH-02, T-FH-03, T-FH-08, T-FH-14 |
| R3 reorder buffer | T-FH-03, T-FH-04, T-FH-05, T-FH-14, e2e.demo.reorder |
| R4 retransmission | T-FH-05, T-FH-06, T-FH-08, T-FH-13, e2e.demo.burst |
| R5 snapshot | T-FH-07, T-FH-11, T-FH-16, e2e.demo.outage |
| R6 heartbeat | T-FH-09, T-FH-10, T-FH-15, e2e.demo.tail |
| R7 correctness | T-FH-12 to T-FH-17, T-PR-01, e2e.demo.all, e2e.demo.watch, e2e.demo.random, e2e.demo.show, e2e.bench.correct |
| R8 both policies | T-FH-06, T-FH-17, T-PR-01, e2e.demo.random |
| R9 Lamport | T-LC-01 to T-LC-05, e2e.demo.lamport |
| R10 exchange | T-EX-01 to T-EX-08 |
| R11 order book | T-OB-01 to T-OB-13 |
| R12 link | T-LL-01 to T-LL-08 |
| R13 command line and prompts | T-CL-01 to T-CL-08, 15 e2e.demo error tests, 5 e2e.bench error tests, the 5 e2e.ask tests |
| R14 statistics | T-ST-01 to T-ST-07 |
| R15 reproducibility | T-EX-03, T-EX-09, T-LL-09, T-PR-02, T-LC-05, e2e.demo.random_seed, e2e.ask.random |
| R16 memory safety | the whole suite, built with `-DFEED_SANITIZE=ON` |

## 4. How good are the tests?

**Coverage** of the library (`src/`) by the suite, measured with gcov and gcovr 8.8:

| | Covered |
| --- | --- |
| Lines | 564 of 574 (98%) |
| Branches | 592 of 618 (95%) |

The uncovered lines are defensive and can only be reached by a bug: the exchange's self-check that
throws if it generated an event that does not fit its own book; the handler's `rejected` counter (the
tests assert, everywhere, that it stays 0); and the impossible `add` case after the `switch` that
handles adds before it. The uncovered branches are the same, plus short-circuit halves of compound
conditions.

**Mutation testing** asks the harder question: would the tests notice a bug? `tests/mutants.py` plants
16 bugs, one at a time, in a copy of the code, and runs the suite against each. Every one must make a
test fail (or hang, which the 60-second timeout turns into a failure):

| # | The bug | Caught by |
| ---: | --- | --- |
| 1 | the fast path ignores events waiting in the buffer | 2 tests |
| 2 | the next expected event is taken for a duplicate | 4 tests |
| 3 | the reorder window is one arrival short | T-FH-05 |
| 4 | every-hole recovery fetches only the first hole | T-FH-06 |
| 5 | buffered copies of retransmitted events are kept | 4 tests |
| 6 | a snapshot keeps the buffered event at its own seq | a test hangs: the handler loops |
| 7 | the heartbeat misses a single lost event | 3 tests |
| 8 | a gap only the heartbeat finds is not counted | T-FH-15 |
| 9 | a fill of the whole order is rejected | 29 tests |
| 10 | the fingerprint ignores quantities | 2 tests |
| 11 | a retransmission leaves out its last event | 3 tests, and one hangs |
| 12 | the exchange retransmits from before its history | T-EX-07 |
| 13 | the link holds a second event, losing the first | 5 tests |
| 14 | the interval uses n degrees of freedom, not n - 1 | T-ST-03 |
| 15 | a number above its option's range is accepted | T-CL-04, e2e.demo.out_of_range |
| 16 | an option given twice is accepted | T-CL-02, e2e.demo.given_twice |

All 16 are killed. Mutant 8 survived the first run of the check; T-FH-15 now also checks the gap count,
which kills it. That is what the check is for.

**Sanitizers.** The suite passes under AddressSanitizer and UndefinedBehaviorSanitizer (GCC 13), with
`-fno-sanitize-recover=all`, so any report fails the run.

**Compilers.** GCC 13 and Clang 18 with `-Wall -Wextra -Wpedantic -Werror`, Clang also with
`-Wdocumentation`, which checks every `@param` against the code.

## 5. Running it

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build                          # all 110
ctest --test-dir build -L unit                  # the 70 GoogleTest tests
ctest --test-dir build -L e2e                   # the 40 end-to-end tests
./build/feed_tests --gtest_filter='FeedHandlerPaths.*'

cmake -S . -B build-asan -DFEED_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j && ctest --test-dir build-asan

cmake -S . -B build-cov -DFEED_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cov -j && ctest --test-dir build-cov && cmake --build build-cov --target coverage

python3 tests/mutants.py                        # about 5 minutes
```

CI (`.github/workflows/ci.yml`) runs the suite with GCC and Clang, under the sanitizers, with 5,000
property worlds, and produces the coverage report, the mutation check and the API documentation on
every push.
