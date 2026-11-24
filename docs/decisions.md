# Design decisions

Every design choice: what was decided, why, and the alternative rejected. The design doc holds the full specification; this log is the short record to defend in the viva. New decisions go at the end.

## D1. Pure core, thin shell (2026-10-23)

- **Decision:** All feed logic is a deterministic state machine that takes messages and the current time and returns actions. Sockets, clocks and the poll loop live only in `shell/`.
- **Why:** The same core runs in the deterministic simulation and in the real processes, so every logic test is exact and reproduces from a seed.
- **Rejected:** Logic inside the poll loop, tested only over real sockets, where timing makes failures hard to reproduce.

## D2. Independent oracle (2026-10-23)

- **Decision:** The exchange simulator keeps its own book, separate code from `order_book/reference_book`, and `exchange` cannot link `order_book`. Both hash a book the same canonical way.
- **Why:** A bug in a shared book class would appear on both sides, and every hash would still match.
- **Rejected:** One book class shared by the exchange and the handler.

## D3. Scope in three tiers (2026-10-23)

- **Decision:** Tier 1 is the required core, tier 2 a Lamport extension run in simulation, tier 3 an optional list built only after the first two.
- **Why:** The assignment requires three things: consume a sequenced stream, rebuild the book, and detect and recover from injected gaps. Lamport timestamps across several instances are only encouraged.
- **Rejected:** The first plan, which added a matching engine, a second line with arbitration, a measurement phase, a runtime library, capture and replay, and a consolidator process.

## D4. Minimal exchange simulator (2026-10-23)

- **Decision:** The simulator generates valid book events directly and does no matching. Executes fill the head order of the best level.
- **Why:** The handler only needs a valid sequenced stream with an exact answer to compare against. Filling the head of the best level is what price-time priority would do.
- **Rejected:** A matching engine, which adds code the handler's tests do not need.

## D5. One line, no arbitration (2026-10-23)

- **Decision:** One UDP multicast line. Lost messages come from a resend or a snapshot.
- **Why:** The assignment does not ask for redundant feeds.
- **Rejected:** Lines A and B with arbitration (Lecture 2, slide 13). It is listed as future work.

## D6. Recovery order (2026-10-23)

- **Decision:** A resend first, then a snapshot when the range is too old or retries run out.
- **Why:** Lecture 2, slide 12's order of increasing cost: a resend costs one round trip, a snapshot the whole book.
- **Rejected:** Snapshot-only recovery, which is simpler but turns every single lost packet into a full book transfer.

## D7. Hole confirmation by timeout (2026-10-23)

- **Decision:** A hole is requested only once it has been open for `T_reord`.
- **Why:** A reordered or late packet looks like a lost one at first. The timeout is a failure detector that can be wrong, and a false suspicion only costs a redundant request because applying by sequence number is idempotent.
- **Rejected:** Requesting on first sight, which turns every reordered packet into a resend.

## D8. What STALE means (2026-10-23)

- **Decision:** The status is STALE in INIT, SNAPSHOT_WAIT and RESENDING, and while the line has been silent for `T_silence`. GAP, an unconfirmed hole, still publishes LIVE. A status change is published at once (invariant S4).
- **Why:** STALE should mean confirmed loss, a silent feed or no book yet. Publishing only after a drain would leave a handler stuck on a hole showing its last LIVE book.
- **Rejected:** STALE on every open hole, which flickers on reordering, and publishing only after drains.

## D9. Fail-safe, never skip (2026-10-23)

- **Decision:** A gap that cannot be recovered leaves the book STALE.
- **Why:** Safety must hold under every fault. In Gärtner's terms (5 August lecture) this is fail-safe: safety kept, liveness given up.
- **Rejected:** Skipping the gap and carrying on, which keeps the handler live but publishes a wrong book (non-masking).

## D10. Snapshot procedure (2026-10-23)

- **Decision:** Buffer live messages before requesting a snapshot. If the buffer fills while waiting, evict the lowest sequence numbers. INIT always takes a snapshot, even at sequence 0.
- **Why:** Buffering first loses nothing between the snapshot and the first buffered message. The lowest buffered messages are the likeliest to be discarded anyway. One path into LIVE is easier to prove than two.
- **Rejected:** Starting another snapshot when the buffer fills during one, which can loop forever.

## D11. Lamport extension in simulation (2026-10-23)

- **Decision:** N handler instances with skewed clocks run inside `simulation`, with planned handoffs. Each writes its updates to its own log, and the logs are merged after the run by physical time and by Lamport time.
- **Why:** Logs merged after the run have no arrival order to fall back on, which is the case Lamport time is for (Lecture 2, slide 26). Running in simulation keeps skew exact and runs reproducible.
- **Rejected:** A consolidator process merging updates live. If the control messages and updates share FIFO links through it, arrival order is already causal, so the experiment would show nothing Lamport time adds.

## D12. Same results on macOS and Linux (2026-10-23)

- **Decision:** Our own random number generator; integer-only decisions in the cores; no iteration over unordered containers; the simulation's event queue keyed on (time, insertion counter); compiler extensions off.
- **Why:** A failing seed from CI on Linux (GCC, libstdc++) must reproduce on a Mac (Apple clang, libc++).
- **Rejected:** `std::` distributions and floating-point decisions, whose results differ between the two standard libraries and CPUs.

## D13. Build and tests (2026-10-23)

- **Decision:** Two presets. `asan` adds AddressSanitizer, UBSan, `-O1 -g`, asserts and a hardened standard library; `release` uses `-O3` and `NDEBUG`. Warnings are errors on our targets only. GoogleTest is fetched at configure time, pinned by URL and SHA-256. `scripts/run_checks.sh` runs configure, build and test through CMake workflow presets.
- **Why:** The sanitizers catch memory and undefined-behaviour bugs early. A pinned dependency builds the same everywhere.
- **Rejected:** A TSan preset, because the code is single-threaded and TSan would find nothing. A system-installed GoogleTest, whose version varies by machine.

## D14. Portable I/O (2026-10-23)

- **Decision:** `poll(2)` and `fcntl` only; `SIGPIPE` ignored in each `main`; the multicast socket bound to its group address.
- **Why:** The same code must build on macOS and Linux. Ignoring `SIGPIPE` keeps a closed TCP peer from killing the process. On Linux a socket bound to `INADDR_ANY` also receives other groups on the same port.
- **Rejected:** `epoll`, `accept4` and `MSG_NOSIGNAL`, which exist only on Linux.

## D15. Full names (2026-10-23)

- **Decision:** Every directory, file, application and script has a full, descriptive name, such as `feed_handler/` instead of `fh/`. The CMake project is `market_data_feed_handler`, and the programs are `feed_demo`, `feed_bench` and `feed_tests`.
- **Why:** Short names make the code and the report harder to read and to explain.
- **Rejected:** Abbreviations such as `exch`, `fh`, `dsim` and `mdfh`.

## D16. The canonical hash function (2026-10-25)

- **Decision:** `BookHasher` folds each field of each order (side, price, order id, quantity) into a 64-bit state as `state = mix(state ^ field)`, where `mix` is the splitmix64 finalizer.
- **Why:** It is order-sensitive, so a wrong queue order changes the hash, and cheap enough to hash the whole book after every event: the 10⁶-event exit test takes about 10 seconds under ASan. Fields are folded as values, never as struct bytes, so padding cannot leak in.
- **Rejected:** Byte-wise FNV-1a, which does several times more work per order, and hashing level totals, which cannot see queue order.

## D17. Two book implementations with different data structures (2026-10-25)

- **Decision:** `exchange/exchange_book` keeps each level as a `std::vector`, finds orders through a `std::map`, and keeps live orders in a vector for random choice. `order_book/reference_book` keeps each level as a `std::list` and finds orders through a hash map that is never iterated.
- **Why:** Independence is the point of the oracle (D2). Different structures make it unlikely that the same mistake appears in both.
- **Rejected:** One book class, or two copies of the same design.

## D18. A third reject reason: invalid_range (2026-10-25)

- **Decision:** `ResendReject` can also say `invalid_range`, for a request with `from_seq > to_seq` or a `to_seq` not yet published.
- **Why:** Every request gets an answer. Without one, a handler bug that asks for the future would wait out `T_rs` and its retries before falling back to a snapshot.
- **Rejected:** Ignoring such a request.

## D19. The exchange throws; the handler's book reports (2026-10-25)

- **Decision:** `ExchangeBook::apply` throws `std::logic_error` on an invalid event. `ReferenceBook::apply` returns an `ApplyResult` and leaves the book unchanged.
- **Why:** The exchange only emits valid events, so an invalid one is a simulator bug and must stop the run. The handler must survive bad input and count it as a desync.
- **Rejected:** Silently ignoring invalid events in either book.

## D20. What the handler knows about missing seqs (2026-10-26)

- **Decision:** Each channel tracks `expected` (the next seq to apply) and `known_end` (the highest seq known to exist, learned from data and from a heartbeat's next seq). Outside a snapshot wait, every seq in `[expected, known_end]` is either buffered or in a hole. After a snapshot at S, the holes are rebuilt from the buffer: every unbuffered run in `[S + 1, known_end]` becomes an ordinary hole, which also covers seqs evicted while waiting.
- **Why:** One invariant gives both hole detection and the snapshot's "request the gap after S" step, with no second bookkeeping path to get wrong.
- **Rejected:** Tracking holes only from arrival order, which loses track of seqs evicted during a snapshot wait.

## D21. How holes are requested (2026-10-26)

- **Decision:** On every call, confirmed holes not already covered by an outstanding request are merged into ranges of at most R_max and sent. A merged range may span buffered seqs between holes. A request gets 1 + N_retry attempts, T_rs apart; then the channel falls back to a snapshot. When the recovery link drops, outstanding requests are forgotten without spending attempts, and the holes are requested afresh when it returns.
- **Why:** Fewest round trips. Resending a few buffered seqs is harmless, because they arrive as duplicates and are dropped by seq. Attempts measure an unresponsive service, not a dead connection.
- **Rejected:** One request per hole, which multiplies round trips after scattered loss.

## D22. A channel that has never heard the line is silent (2026-10-26)

- **Decision:** `silent` starts true and clears on the first packet or heartbeat, so an empty snapshot at S = 0 publishes STALE until the line speaks.
- **Why:** A handler whose multicast join failed would otherwise show an empty book as LIVE.
- **Rejected:** Starting the silence timer at handler start.

## D23. The simulation's recovery link (2026-10-26)

- **Decision:** Two different faults. A recovery link outage disconnects the handler: it is told `on_recovery_down`, frames in flight are lost, and each reconnection gets a new connection id so nothing from the old one arrives. A service outage (the adversary's `recovery_outages`) keeps the connection up but answers nothing.
- **Why:** They exercise different paths: the first discards a partial snapshot and pending requests; the second runs out retries and the snapshot timeout's backoff. Scenario 8 runs both.
- **Rejected:** Modelling only one of them.

## D24. Checking every apply through an observer (2026-10-26)

- **Decision:** The handler core takes an optional `HandlerObserver` that sees every apply and every installed snapshot. The simulation's `InvariantChecker` uses it to check S2 and the book hash against truth at every applied seq (k = 1), on top of S1 at every publish and S3 and S4 after every call. The real shell leaves it null.
- **Why:** Checking only at publishes would miss a wrong intermediate book within one drain.
- **Rejected:** Re-deriving applies from the handler's publishes, which only show the last seq of each call.

## D25. The property test's seeds (2026-10-26)

- **Decision:** Each seed draws its own world: 1 or 2 channels, 1 to 20 actions per ms, books of 8 to 200 orders, loss up to 20 %, duplication up to 5 %, reordering up to 10 % by up to 10 ms, up to 3 bursts of up to 600 seqs, line, service and link outages, and sometimes a 200-to-2,000-event resend window, a 200-to-1,000-message buffer or R_max = 500. One seed in eight is lossless, with reordering strictly inside T_reord. Every seed runs 500 ms of damage, then heals, runs 100 ms more traffic, and must be LIVE at the exchange's last seq within 5 s. The 500 seeds run as five tests of 100; `FEED_SEED=n` replays one. The determinism test pins one seed's output hash, so CI on Linux checks it against the macOS value.
- **Why:** Every recovery path is reached many times (in the first run: about 23,000 holes, 1,100 snapshots, 160 rejects and 87 overflows across 4.2 million checked applies), and a failing seed replays exactly.
- **Rejected:** Fixed damage rates per run, which leave paths such as eviction and too_old untested.

## D26. One thread and one poll loop per process (2026-10-26)

- **Decision:** Each program is a single thread that runs one loop: build a `PollSet`, wait until the next timer, then service what is ready. The handler reads the line first on every turn, up to 1,024 datagrams, then the recovery connection, then the core's 1 ms timer. A loop that falls behind does not burst to catch up: the exchange simply generates more slowly.
- **Why:** The cores are single-threaded state machines, so one thread needs no locks and replays the simulation's order of calls. Reading the line first keeps the kernel's receive buffer from overflowing during recovery.
- **Rejected:** A reader thread per socket, which adds locking around a core that needs none. A callback-registering poll loop, which is more code than a per-turn `PollSet` for two or three descriptors (so `shell/poll_loop` became `shell/poll_set`).

## D27. The adversary runs inside the exchange simulator (2026-10-26)

- **Decision:** The exchange process asks the same `Adversary` as the simulation what to do with each packet before it reaches the socket. Delayed packets wait in a time-ordered queue. A recovery outage means the service reads requests and answers none; the TCP connection stays up.
- **Why:** The real run then exercises exactly the damage the simulation does, from the same config format, with no packet-mangling proxy in between.
- **Rejected:** A separate proxy process on the line, which would be a third program to build and test.

## D28. One TOML file configures both programs (2026-10-26)

- **Decision:** `config/local.toml` has shared tables (`[market_data]`, `[recovery]`, `[checkpoints]`) and one table per program. Every key has a default, and an unknown key or an out-of-range value stops the program with the file and key named. The adversary lives in its own file, found relative to the config.
- **Why:** The two programs cannot disagree on the group, the port, R_max or k when they read one file. Rejecting unknown keys turns a typo into an error instead of a silently ignored setting.
- **Rejected:** Command-line flags, which would repeat every shared setting on two command lines.

## D29. Truth and checkpoint files, checked by awk (2026-10-26)

- **Decision:** Both files hold text lines `<channel> <seq> <hash>` every k seqs, with the hash in 16 hex digits, then one `final` line per channel. `run_loopback_demo.sh` checks every checkpoint against the truth line for the same seq, and each channel's final seq, hash and state. Hashes are compared as strings.
- **Why:** Text files can be read by eye and diffed. String comparison matters: a mutation check found that awk compared a hash made only of decimal digits as a double, so two hashes that differ in the 16th digit compared equal.
- **Rejected:** Binary files, and a separate C++ checker for a 30-line comparison.

## D30. How a real run ends (2026-10-26)

- **Decision:** The exchange generates `messages_per_channel` events per channel, then keeps sending heartbeats and serving recovery for `linger_ms`, then exits. The handler exits after `exit_after_silence_ms` without a packet, counted from its start until the first one. The check requires each channel's final state to be `live`, not its status.
- **Why:** Both programs stop on their own, so a script or ctest can run them. The status is STALE by then, correctly: the line has gone silent (S4). The state shows whether the book caught up. Counting silence from the start makes a handler that never hears the line fail fast.
- **Rejected:** Killing the handler from the script after a fixed wait, which races with recovery.

## D31. Loopback multicast setup (2026-10-26)

- **Decision:** The sender sets `IP_MULTICAST_IF` to 127.0.0.1 with loopback on, and the receiver joins on 127.0.0.1. On Linux, `scripts/setup_loopback_multicast.sh` (run with sudo) turns on lo's MULTICAST flag. No route is added. CI runs it before the tests.
- **Why:** With the interface named, neither system needs a multicast route, so the script changes as little of the host as possible. macOS needs nothing; this was confirmed locally.
- **Rejected:** A route for 239.0.0.0/8 to lo, which would redirect every multicast program on a developer's machine.

## D32. A minimal rewrite (2026-10-26)

- **Decision:** One small CMake project of about 1,000 lines: `order_book`, `exchange` (seeded generator, its own book as truth, retransmit history, snapshots), `lossy_link` (random loss, duplication, reordering and injected gap ranges), `feed_handler` (in-order apply, a buffer for early events, retransmission after a reorder window, a snapshot when the history no longer has the range, a heartbeat for tail loss), and an in-process `lamport` demo. Ten GoogleTests. Everything runs in one process with no sockets. The earlier design (D1 to D31) is kept, unbuilt, in `legacy/`.
- **Why:** The assignment asks for three things: consume a sequenced stream, rebuild the book, and recover from injected gaps. Lamport timestamps are only encouraged. The earlier design grew well beyond that, and a smaller program is easier to defend line by line.
- **Rejected:** Keeping or trimming the earlier design (Rohit chose the rewrite), and the full skew experiment for Lamport (the small handoff demo shows the same result).

## D33. Simplifications after review (2026-10-26)

- **Decision:** The handler takes the `Exchange` directly; there is no recovery interface. An event is a plain struct with an `EventType` and a `switch`, not a `std::variant` with a visitor. The Lamport demo compares `last_applied()` before and after each event instead of using a callback. Gap tracking is one counter of arrivals during a gap, and a heartbeat is one `recover()` call. Code uses `using namespace std` and no project namespace.
- **Why:** Each removed piece had one user or none, and the shorter version is easier to explain line by line.
- **Rejected:** Keeping the abstractions for flexibility nothing here needs.

## D34. Component folders, named demo scenarios, and gap counting (2026-10-26)

- **Decision:** One folder per component under `src/` (`book/`, `exchange/`, `network/`, `handler/`, `lamport/`), the program's `main` in `apps/demo.cpp`, and one test file per component. The demo takes a scenario name (`burst`, `reorder`, `tail`, `outage`, `random`, `lamport`, `all`) and traces what the handler does with each arriving event. A gap is counted when the wait for it starts, so the gap count never falls below the number of recoveries.
- **Why:** Each folder matches one box in the architecture diagram, and each scenario demonstrates one behaviour in under a minute. The earlier gap count missed a gap that was already waiting behind one just recovered, and reported fewer gaps than recoveries.
- **Rejected:** An `include/` and `src/` split, which suits a library meant for others, and a single demo run with numbers only.

## D35. Five fixes from profiling (2026-11-18)

- **Decision:** Recover every hole behind the window at once, not only the first; keep orders in an `unordered_map`; apply an in-order event directly when nothing waits (no trip through the buffer); apply retransmitted events directly instead of buffering them; move the snapshot's book into the handler instead of copying it. The recovery policy stays selectable (`Recovery::first_hole`, `Recovery::every_hole`) so the two can be compared.
- **Why:** Each was a measured cost in `feed_bench` (PERFORMANCE.md): at 20% loss, first-hole recovery left later holes waiting a full window each, and most of `on_event`'s time was map work that an undamaged stream never needs.
- **Rejected:** A flat array or ring buffer for the reorder buffer, which is faster still but needs a bound on how far ahead an event may arrive; the map has none.

## D36. Portable random draws (2026-11-22)

- **Decision:** The link decides each chance by comparing 53 random bits from `mt19937_64` with `chance * 2^53`; the exchange draws with `random() % n`. No `std::bernoulli_distribution` or `uniform_int_distribution`.
- **Why:** The standard fixes `mt19937_64`'s output but not the distributions' algorithms, which differ between libstdc++, libc++ and MSVC. With these draws the same seed gives the same stream, damage and result everywhere (R15). `T-EX-09` pins it; it passes on x86-64 (GCC 13, Clang 18) and on aarch64 (GCC 11).
- **Rejected:** The standard distributions (results that depend on the compiler) and a hand-written generator (`mt19937_64` is already portable). The modulo bias of `% n` for n of at most a few hundred is below 10⁻¹⁶.

## D37. A book fingerprint (2026-11-22)

- **Decision:** `OrderBook::fingerprint()`: the sum, over resting orders, of a splitmix64 hash of (id, side, price, quantity). Equal books have equal fingerprints whatever order their orders arrived in.
- **Why:** The demo can show "the same book" as one number on both sides, and the property test can check the handler's book after every arrival against the exchange's book at that seq, recorded as one number per event, instead of a copy of the book per event.
- **Rejected:** Comparing whole books at every step (memory for a copy per seq), and an ordered hash (needs the orders sorted). A sum can collide, so tests also compare the books themselves wherever they can.

## D38. Our own command-line parser (2026-11-22)

- **Decision:** `Options` (src/cli): `--name value` or `--name=value`, flags, positional words; it rejects unknown options, repeated options, missing values and values given to flags when it parses, and the typed getters (`integer`, `probability`, `range`, `choice`, lists) check each value against its range. Programs read and check every setting before running anything (`feed_demo all` checks all six scenarios first). Exit status 0 for success, 1 if a book did not match, 2 for a bad command line.
- **Why:** The demo had hard-coded scenarios and read positional numbers with `stoull`, which accepts `12abc` and throws on `abc`. A third party must be able to vary every number and get a clear message when one is wrong.
- **Rejected:** `getopt_long` (no value checking, a GNU extension on some systems) and CLI11 (a dependency for fifteen options).

## D39. Statistics over seeds (2026-11-22)

- **Decision:** Every figure in `feed_bench` is the mean over n independent seeds (default 10) with a 95% confidence interval from Student's t with n − 1 degrees of freedom. `compare` runs two settings on the same seeds, alternating which runs first, and tests the per-seed differences with a paired t-test. The t quantiles come from the regularized incomplete beta function, checked against SciPy.
- **Why:** A seed is an independent replication: its own stream and its own damage. One seed's median says nothing about the next stream. Pairing by seed removes the stream-to-stream variation from a comparison, so smaller differences can be detected.
- **Rejected:** Median of repeats of one seed (the earlier bench), the normal approximation (n is small), and the bootstrap (more code, the same answer at these sizes).

## D40. A formal test plan (2026-11-22)

- **Decision:** Sixteen numbered requirements (R1-R16) and numbered tests (T-OB-01 and so on) traced to them in docs/TESTING.md. Structural tests drive each branch of the handler with events in an exact order; functional tests check one requirement each on whole streams; property tests check invariants after every arrival in 200 random worlds; end-to-end tests run the programs. Coverage is measured, and a mutation check plants 16 bugs that must each be caught.
- **Why:** Ten tests that each mixed several behaviours showed the code worked on those cases, but not which requirement each test covered or whether the tests could catch a bug.
- **Rejected:** A mutation-testing framework (mull, dextool), which is heavy to set up for 16 mutants.

## D41. GPL-3.0-or-later (2026-11-22)

- **Decision:** The project is licensed under the GNU General Public License, version 3 or later; every source file starts with an SPDX identifier and a copyright line.
- **Why:** As the instructor suggested: the code is published as an artifact, and copyleft keeps derived versions open.
- **Rejected:** MIT (permissive: derived versions may be closed) and GPL-3.0-only (blocks a move to a later GPL).

## D42. The handoff takes at least 1 ms (2026-11-22)

- **Decision:** `feed_demo lamport --latency-ms` accepts 1 and up.
- **Why:** The exchange publishes one event per millisecond, so a handoff faster than one event is below the model's resolution. At latency 0 the new instance's first update has the same seq as the old instance's last, and the count of misordered updates no longer equals the skew minus the latency.
- **Rejected:** A finer time step, which changes every published Lamport number for no gain.

## D43. The demo asks, and draws a fresh seed (2026-11-23)

- **Decision:** `feed_demo` with no arguments, or any scenario with `--ask`, asks for each setting at a prompt: the scenario, the number of events, the seed, the damage, and, if wanted, the handler's settings. Each question shows its default; a bad answer gets the same message as on the command line, and the question again. Unless `--seed` is given, every run draws a random seed, prints it, and ends with the exact command that replays it.
- **Why:** The instructor read the demo as hard-coded: every run with the same defaults printed the same numbers. A fresh seed shows the result does not depend on one lucky stream, and the printed seed and replay command keep every run reproducible.
- **Rejected:** A menu library such as ncurses (a dependency, and not testable from a script), and keeping fixed default seeds (reproducible, but every run identical). The prompts read standard input line by line, so the end-to-end tests drive them from answer files.

## D44. Showing the events at both ends (2026-11-23)

- **Decision:** `--show N` follows the first and last N events from the exchange to the book and prints each: what it was, what the link did with it (arrived, lost, doubled, held back) and how it reached the book (applied at once, waited in the buffer, recovered by retransmission or after the heartbeat, through a snapshot).
- **Why:** Counts and fingerprints say the book is right; showing individual events says what actually happened to them, which is what a newcomer needs to see. The two ends are where the interesting cases cluster: the start, and the tail that only the heartbeat can recover.
- **Rejected:** Printing every event (millions of lines), and following a random sample (harder to check by eye).

## D45. No made-up project name (2026-11-23)

- **Decision:** The project has no brand name. It is the market data feed handler; the CMake project is `market_data_feed_handler`, its library `market_data_feed`, and its programs `feed_demo`, `feed_bench` and `feed_tests`. The build options are `FEED_SANITIZE`, `FEED_COVERAGE` and `FEED_WERROR`, and the property tests read `FEED_SEED` and `FEED_SEEDS`.
- **Why:** An invented name tells a newcomer nothing and has to be explained before anything else; names that say what a thing is explain themselves, on the slides, in the messages and in the commands.
- **Rejected:** Short abbreviations such as `mdfh` (see D15).
