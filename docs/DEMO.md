# The demo: 15 minutes, 20 slides

The deck is [feed-handler-deck.pptx](feed-handler-deck.pptx) (also as a [PDF](feed-handler-deck.pdf)).
Its speaker notes hold the words for every slide, with start times, including what to say at each
live step. The live part runs from `scripts/demo.sh`, one step per Enter: steps 1 to 7 are the
failure cases (slide 11), steps 8 and 9 the tests (slide 14). The domain introduction is shared
with the rest of the team and comes before this talk.

## Before the talk

- Build in Release: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.
- Run `scripts/demo.sh` once from start to end. Step 9 edits `src/handler/feed_handler.cpp` for a few
  seconds and always puts it back; do not run it with unsaved edits to that file open.
- Terminal: dark text on a light background, font at least 20 pt, at least 110 columns wide (the
  event table in step 5 is about 105 wide).
- In the deck, set the date once in View → Slide Master. The IIIT-B logo goes in the master's top
  right corner, where the lectures have it.
- Presenter view shows the notes. Every run draws a fresh seed, so live numbers differ from the
  slides; the check at the end of each run is what matters.

## The timeline

| Time | Slide | What happens |
| --- | --- | --- |
| 0:00 | 1 Title, 2 Please Mind the Gap, 3 Outline | The hook: a gap you notice and step across; a gap in the feed |
| 0:50 | 4 A Lost Message Becomes a Ghost Order | Wrong costs money, silently; the requirement: never wrong, only behind |
| 1:30 | 5 What I Built | The pipeline and four numbers: 0 wrong, 49 ns, 110 tests, 1 seed |
| 2:15 | 6 to 10 How It Works | Notice, wait, fill; one gap step by step; architecture; the handler; Lamport |
| 5:10 | 11 Live Demo | Switch to the terminal: steps 1 to 7, about 3½ minutes |
| 9:00 | 12, 13 Testing | Five layers of tests; the oracle on every event |
| 10:10 | 14 Plant a Bug | Terminal: step 8 (all 110 tests), step 9 (a planted bug, caught), about 1 minute |
| 11:25 | 15 to 17 Results | Zero wrong books; 49 ns per event; every setting measured |
| 12:50 | 18, 19 Real-World Value | Production feeds use the same mechanisms; what it gives a desk |
| 14:05 | 20 Summary | Leave it up for questions; ends about 14:30 |

## The live steps

| Step | Command | What it shows | Point at |
| --- | --- | --- | --- |
| 1 | `feed_demo burst` | Seqs 11 to 15 lost; the gap waits 3 arrivals, then exactly that range is retransmitted | `still missing after 3 more arrivals: retransmitted` |
| 2 | `feed_demo reorder` | Late and doubled events, none lost | `(0 retransmissions needed)` |
| 3 | `feed_demo tail` | The last 3 events lost; nothing after them | `heartbeat: the exchange is at seq 20: seqs 18-20 missing: retransmitted` |
| 4 | `feed_demo outage` | 400 lost, the exchange keeps 100 | `too old to retransmit: snapshot` |
| 5 | `feed_demo random --ask --show 5` | It asks for every number. Take them from the room; type `150%` for the loss first, to show it refused and asked again; Enter for the seed, so it is random | The event table at both ends, the fingerprint, the `replay:` line |
| 6 | `feed_demo watch` | 2,000,000 events at 200,000 a second, 20% lost | `behind` stays near 0; the fingerprint at the end |
| 7 | `feed_demo lamport` | Two instances, a 5 ms handoff, growing clock skew | Physical time misorders past 5 ms; Lamport stays at 0 |
| 8 | `ctest --test-dir build` | All 110 tests, about 2 seconds | `100% tests passed` |
| 9 | planted bug | One line of clean-up deleted, rebuilt: 4 tests fail, the property test among them; the line put back, all pass | The oracle's `world 3: ...` and `replay:` lines |

Every scenario ends with `matches the exchange's own book` and a `replay:` line. If short of time,
skip step 6 (type `s` at its prompt). If a live run misbehaves, the recorded runs are in
[demo-output/](demo-output/): `2-burst`, `3-reorder`, `4-tail`, `5-outage`, `6-asked`, `1-watch`,
`7-lamport`.

## Varying it live

Any number can be changed on the spot, on the command line or at the prompts; `feed_demo --help`
lists them all.

```bash
./build/feed_demo                                # the menu, then the questions
./build/feed_demo watch --loss 40% --rate 500000
./build/feed_demo burst --gap 101-150 --events 300 --show 3
./build/feed_demo outage --history 1000          # now the history covers the gap: a retransmission
./build/feed_demo random --window 0 --recovery first --show 10
./build/feed_demo random --seed 658947 --events 50000 --loss 30% --show 4    # replays a recorded run
./build/feed_demo lamport --latency-ms 10 --skews-ms 0,10,20,40
./build/feed_bench compare --param window --a 0 --b 3 --seeds 20
```

## Questions to expect

- *Is the demo hard-coded?* No. Run it with no arguments and it asks for every number; every run draws
  a fresh random seed, prints it, and prints the command that replays it exactly.
- *Why not apply events as they arrive?* One reordering would leave the book wrong, with nothing to
  show it. In seq order, the book is always the exchange's book at some seq.
- *Why wait three arrivals?* A reordered event is not lost. Recovering at once would retransmit every
  late event; step 2 shows they need nothing.
- *Why count arrivals, not time?* Counting makes every run reproducible from its seed. A real handler
  would add a time bound for quiet streams.
- *What if a retransmission is itself lost?* It travels over a reliable channel (TCP in a real system,
  a function call here), so it is not.
- *How do you know the book is right, and not just right at the end?* The property test records the
  exchange's book after every event and checks the handler's book against it after every arrival.
- *Would this work on a real feed?* The mechanisms are the ones production feeds use: Nasdaq's
  MoldUDP64 has sequence numbers, a re-request server and heartbeats; CME's MDP 3.0 has TCP replay for
  small gaps and a snapshot loop for large ones. What is missing is a protocol decoder, recovery over
  a real connection off the hot path, and A/B feed arbitration.
- *Why Lamport and not physical (wall-clock) time?* The instances' clocks are not synchronised; once the
  skew is larger than the handoff time, physical timestamps misorder updates (step 7).
- *What does Lamport time not give you?* L(a) < L(b) does not mean a happened before b; it orders
  consistently with causality, nothing more.
- *How significant are the performance differences?* Every figure is a mean over independent seeds
  with a 95% interval; comparisons pair the seeds and give a p-value.
