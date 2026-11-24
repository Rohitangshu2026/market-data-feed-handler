# Performance

How fast the feed handler runs, how long each arrival takes, how far its book lags, whether the book
stays correct, and what each design choice costs. Every figure comes from `feed_bench`
(`apps/bench.cpp`) and is a **mean over 10 independent seeds with its 95% confidence interval**
(Student's t, 9 degrees of freedom). Each seed is a different stream with different damage, so an
interval says how much a figure would move on other streams, not just on a rerun. Comparisons run both
settings on the same seeds and test the per-seed differences with a paired t-test.

## In short

- **Correct at any loss rate tried.** From 0% to 50% random loss, and with long bursts, the final
  book matched the exchange's in 10 of 10 seeds at every rate, and at every one of about 880,000
  intermediate checks; no event was ever rejected.
- **Fast.** The handler alone takes 49.0 ± 0.7 ns per event on a clean stream (20.4 M events/s),
  101.5 ± 4.2 ns at 20% loss (9.9 M/s) and 110.1 ± 4.0 ns at 30% (9.1 M/s). With the exchange and the
  link included, the whole pipeline runs 4.2 to 5.5 M events/s.
- **Short tails.** A typical arrival takes 83 ns (the median, at 5% loss); the p99 is 680 ± 28 ns, set
  by recovery calls. At 20% loss the book is at most 7 events behind at the p99.
- **The bottleneck is the book itself**: on an undamaged stream the book update is 68.9 of the 69.7 ns
  `on_event` takes; sequencing adds 0.8 ± 0.7 ns.
- **Design choices, measured** (all p < 0.0001): every-hole recovery is 29% faster than first-hole at
  20% loss and cuts book lag p99 from 41 to 7 events; a reorder window of 3 instead of 0 avoids 16,000
  needless retransmissions per million events, for 3 events more lag at the p99; a snapshot costs
  about 12 µs whatever the gap, so it is cheaper than retransmission beyond about 120 events.

## How it was measured

| | |
| --- | --- |
| Machine | MacBook Air (Apple silicon), Ubuntu 22.04 VM with 4 vCPUs; g++ 11.4, `-O3 -DNDEBUG` (as CMake's Release build) |
| Workload | 1,000,000 events per run; 1% duplicated, 2% reordered, and 0-50% lost at random unless a section says otherwise; history 10,000; window 3; every-hole recovery |
| Seeds | 10 per figure, seeds 1 to 10; seed s drives the exchange and seed s xor a constant drives the link |
| Timing | `steady_clock`. The handler's own time is read around each batch of arrivals, with the clock's cost (27.2 ns a pair) taken out. On this machine the clock steps every 41.7 ns, so single-call times come in multiples of it |
| Noise | Measured each run: spinning for 1 s, the VM paused the process about 1,300 times for more than 1 µs (median 1.2 µs, worst 37-158 µs), 0.4-0.6% of the time. Percentiles of single calls beyond the p99.9 are mostly these pauses |
| Statistics | mean ± t₀.₉₇₅,₉ × sd / √10. A comparison alternates which setting runs first, seed by seed, so a slow spell of the machine hits both |

Raw per-seed values: `feed_bench <section> --csv DIR` writes every one to `DIR/bench.csv`.

## 1. Speed

ns per published event, and millions of events a second (`feed_bench speed`):

| loss | handler ns | handler M/s | whole run ns | whole M/s |
| ---: | ---: | ---: | ---: | ---: |
| 0% | 49.0 ± 0.7 | 20.40 ± 0.27 | 181.6 ± 0.5 | 5.51 ± 0.02 |
| 1% | 54.4 ± 0.4 | 18.38 ± 0.12 | 187.5 ± 1.7 | 5.34 ± 0.05 |
| 5% | 70.8 ± 2.4 | 14.15 ± 0.43 | 206.7 ± 8.6 | 4.85 ± 0.18 |
| 10% | 91.8 ± 13.8 | 11.21 ± 1.21 | 248.4 ± 51.0 | 4.23 ± 0.57 |
| 20% | 101.5 ± 4.2 | 9.88 ± 0.35 | 232.8 ± 1.0 | 4.29 ± 0.02 |
| 30% | 110.1 ± 4.0 | 9.11 ± 0.30 | 240.6 ± 1.1 | 4.16 ± 0.02 |

*Handler* is `on_event` and `on_heartbeat` alone; *whole run* adds the exchange generating and
applying each event to its own book, and the link. The wide interval at 10% is one seed that met a
slow spell of the machine; the interval reports it rather than hiding it.

**Where the handler's time goes** on an undamaged stream, replayed (`feed_bench stages`):

| stage | ns per event |
| --- | ---: |
| book update: `OrderBook::apply` | 68.9 ± 0.4 |
| sequencing + book: `on_event` | 69.7 ± 0.8 |
| sequencing alone (the difference) | 0.8 ± 0.7 |

## 2. Latency

Every `on_event` call timed and sorted by what it did, at 5% loss (`feed_bench latency`); ns per
call, and each kind's share of the handler's time:

| call | calls a run | p50 | p99 | p99.9 | time |
| --- | ---: | ---: | ---: | ---: | ---: |
| in order: applied | 754,640 ± 584 | 83 ± 0 | 184 ± 38 | 346 ± 301 | 60.6% |
| early: buffered | 138,633 ± 325 | 67 ± 15 | 96 ± 28 | 175 ± 124 | 8.9% |
| late: fills a gap | 14,541 ± 58 | 250 ± 0 | 380 ± 83 | 1,487 ± 2,228 | 3.7% |
| duplicate: dropped | 10,061 ± 92 | 42 ± 0 | 46 ± 9 | 63 ± 29 | 0.3% |
| recovery: retransmit | 41,364 ± 95 | 596 ± 14 | 1,050 ± 334 | 6,192 ± 10,061 | 26.4% |
| **all arrivals** | 959,239 ± 165 | 83 ± 0 | 680 ± 28 | 1,075 ± 515 | 100% |

Retransmitting calls are 4% of arrivals and 26% of the time. Their p99.9 interval is wide because a
few of the 41,000 calls a run meet a pause of the machine.

**Book lag**, in newer events the exchange had already published when a seq reached the handler's book:
at 5% loss, p99 4.0 ± 0.0, p99.9 6.0 ± 0.0, worst 8.7 ± 0.6, and 19.0% of seqs delayed at all. In the
burst scenario (25 bursts longer than the history, 24 shorter) the worst lag is 2,004 events: the
handler waits for the snapshot that a 2,000-event gap needs, never showing a wrong book meanwhile.

**Under load.** Feeding the measured call times of one run to a single-server queue with Poisson
arrivals: capacity is 13.8 M arrivals/s at 5% loss. At 50% of it the p99 latency (wait + service) is
1.4 µs; at 90% it is 10 µs; at 95%, 16 µs.

## 3. Correctness

The handler's book against the exchange's at every catch-up, at most every 100 events, and at the end
(`feed_bench correct --losses 0,1%,5%,10%,20%,30%,40%,50%`):

| damage | lost | gaps | retransmissions | snapshots | only late | rejected | checks | wrong | matched |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0% loss | 0 | 19,610 ± 116 | 0 | 0 | 0 | 0 | 99,975 | 0 | 10/10 |
| 1% loss | 10,034 ± 77 | 28,078 ± 90 | 10,110 ± 84 | 0 | 185 ± 11 | 0 | 99,878 | 0 | 10/10 |
| 5% loss | 50,063 ± 124 | 55,905 ± 139 | 48,283 ± 123 | 0 | 759 ± 17 | 0 | 99,517 | 0 | 10/10 |
| 10% loss | 99,917 ± 157 | 80,110 ± 88 | 91,026 ± 147 | 0 | 1,231 ± 15 | 0 | 99,117 | 0 | 10/10 |
| 20% loss | 199,823 ± 301 | 106,272 ± 100 | 161,200 ± 163 | 0 | 1,562 ± 26 | 0 | 98,420 | 0 | 10/10 |
| 30% loss | 299,850 ± 297 | 114,038 ± 72 | 211,032 ± 185 | 0 | 1,535 ± 34 | 0 | 97,730 | 0 | 10/10 |
| 40% loss | 399,704 ± 281 | 111,028 ± 66 | 240,658 ± 180 | 0 | 1,309 ± 30 | 0 | 96,971 | 0 | 10/10 |
| 50% loss | 500,009 ± 285 | 100,958 ± 76 | 250,397 ± 185 | 0 | 1,007 ± 24 | 0 | 96,052 | 0 | 10/10 |
| bursts | 60,728 ± 75 | 26,670 ± 96 | 9,623 ± 82 | 25 | 178 ± 10 | 0 | 94,988 | 0 | 10/10 |

Means per run; checks and wrong are totals over the 10 seeds. *Only late* counts events recovered whose
originals then arrived anyway (dropped as duplicates). At 0% loss the 19,610 gaps are reordered events,
each closed by the late event itself: no retransmission. One retransmission often recovers several
events, so above 20% loss there are fewer retransmissions than lost events.

## 4. Design choices, measured

Each row compares two settings on the same 10 seeds (`feed_bench compare`); the difference is B − A
with its 95% interval and the paired t-test's p-value.

**Recovery policy**, at 20% loss: first-hole (A) against every-hole (B), the default.

| metric | first-hole | every-hole | difference | p |
| --- | ---: | ---: | ---: | ---: |
| handler ns per event | 140.4 ± 0.3 | 100.3 ± 0.7 | −40.1 ± 0.8 | < 0.0001 |
| book lag p99, events | 40.7 ± 1.0 | 7.0 ± 0.0 | −33.7 ± 1.0 | < 0.0001 |
| book lag, worst | 92.3 ± 8.1 | 14.4 ± 1.2 | −77.9 ± 8.3 | < 0.0001 |
| seqs delayed | 80.3% | 51.0% | −29.4 points | < 0.0001 |
| events recovered that arrived anyway | 0 | 1,562 ± 26 | +1,562 | < 0.0001 |

First-hole recovery fetches one hole per window, so under heavy loss holes queue up: they open at
about *loss* per arrival and close at one per *window + 1* arrivals, a queue at utilisation
loss × (window + 1) = 0.8 at 20% loss. Every-hole fetches them all at once, at the price of sometimes
fetching an event that was only reordered (0.8% of recovered events).

**Reorder window**, 0 (A) against 3 (B), at 5% loss:

| metric | window 0 | window 3 | difference | p |
| --- | ---: | ---: | ---: | ---: |
| retransmissions | 64,278 ± 163 | 48,283 ± 123 | −15,995 ± 51 | < 0.0001 |
| events recovered that arrived anyway | 17,630 ± 62 | 759 ± 17 | −16,871 ± 49 | < 0.0001 |
| handler ns per event | 58.0 ± 0.5 | 70.0 ± 0.4 | +12.0 ± 0.4 | < 0.0001 |
| book lag p99, events | 1.0 | 4.0 | +3.0 | < 0.0001 |
| seqs delayed | 6.9% | 19.0% | +12.2 points | < 0.0001 |

The full sweep (`feed_bench window`): window 1 already cuts the needless retransmissions from 17,630
to 837 per run, because this link holds an event back by exactly one place; each step beyond it adds
lag and saves little. A real network's reorder depth should set the window.

| window | 0 | 1 | 2 | 3 | 5 | 8 | 16 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| handler ns | 58.7 ± 0.6 | 62.6 ± 0.4 | 66.7 ± 0.4 | 71.9 ± 1.8 | 80.1 ± 1.2 | 88.9 ± 0.5 | 113.5 ± 7.1 |
| only late | 17,630 | 837 | 805 | 759 | 707 | 615 | 487 |
| lag p99 | 1 | 2 | 3 | 4 | 7 | 10 | 18 |
| seqs delayed | 6.9% | 11.3% | 15.3% | 19.0% | 25.5% | 33.5% | 48.3% |

**History**, 3 events (A) against 10,000 (B), at 5% loss: with only 3 events kept, every loss is
older than the history by the time the window ends, so each recovery is a snapshot: 41,327 ± 96 of them
per run, and 554.0 ± 3.7 ns per event against 70.2 ± 1.4 (p < 0.0001). The book is just as correct.

**Recovery cost by gap size**: the one `on_event` call that closes a gap of each size, in µs
(`feed_bench recovery`):

| gap, events | 1 | 10 | 100 | 1,000 | 10,000 |
| --- | ---: | ---: | ---: | ---: | ---: |
| retransmission | 0.88 ± 0.17 | 1.38 ± 0.06 | 10.68 ± 4.41 | 75.67 ± 8.86 | 746.39 ± 56.80 |
| snapshot (book of up to 200 orders) | 13.03 ± 0.80 | 11.70 ± 0.23 | 11.61 ± 0.18 | 11.69 ± 0.33 | 12.00 ± 0.23 |

Retransmission costs about 75 ns per event for large gaps; a snapshot costs about 12 µs whatever the
gap. They cross between 100 and 300 events, near 120 by interpolation: beyond that, for a book this size, a
snapshot is cheaper.

## 5. Bottlenecks

**Found and fixed (18 November).** Profiling the first version found five, ranked by cost; each fix
was measured alone and all five are now in the code (decision D35). Measured then with the earlier
single-seed bench on the same machine, the handler went from 137 to 52 ns per event on a clean stream
and from 199 to 102 ns at 20% loss; on a cloud VM, book lag p99 at 20% loss fell from 43 to 7 events.

1. **Recovery filled one hole per window**, so holes queued up (above). Fix: every-hole recovery.
2. **The book was three node-based `std::map`s**; the order lookup walked a red-black tree. Fix:
   orders in an `unordered_map` (book update 119 → 70 ns).
3. **Every in-order event made a round trip through the reorder buffer**: one allocation, one free and
   two rebalances per event. Fix: apply it directly when nothing waits (sequencing 30 → 2 ns).
4. **Retransmitted events went into the buffer and out again.** Fix: apply them directly (about 45%
   less per recovered event).
5. **A snapshot copied the book twice.** Fix: move it (19 → 12 µs).

**What limits it now, worst first.**

1. **The book update** is 99% of an in-order `on_event`. Prices sit in a fixed band, so an array of
   levels indexed by price offset and a preallocated pool of orders would remove the remaining
   allocation and pointer chasing.
2. **Recovery runs inline.** A 10,000-event retransmission holds `on_event` for 0.75 ms. Choosing a
   snapshot beyond the measured crossover (about 120 events here) would cap it near 12 µs, and in a real
   system recovery belongs off the hot path, with live events buffered meanwhile.
3. **The window is wider than this link reorders.** Window 1 would delay 11% of seqs instead of 19% for
   77 more needless recoveries per million events.
4. **Every-hole recovery fetches some events that were only late** (0.8% at 20% loss). Recovering only
   holes older than the window would avoid it; not measured.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/feed_bench all --csv results            # sections 1 to 5, about 3 minutes
./build/feed_bench correct --losses 0,1%,5%,10%,20%,30%,40%,50%
./build/feed_bench compare --param recovery --a first --b every --loss 20%
./build/feed_bench compare --param window --a 0 --b 3
./build/feed_bench compare --param history --a 3 --b 10000
./build/feed_bench speed --seeds 30 --events 2e6  # tighter intervals
```

Numbers from another machine will differ in time, not in the counts: every count above is fixed by the
seeds and is the same on every platform.
