// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * feed_bench: measures the feed handler. Every measurement is repeated
 * over independent seeds (each seed is a different stream and different
 * damage) and reported as a mean with its 95% confidence interval, from
 * Student's t over the seeds. The compare section runs two settings of one
 * parameter on the same seeds and tests the difference with a paired t-test.
 * Every number that shapes a run can be set on the command line; run
 * `feed_bench --help` for the list.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/options.hpp"
#include "exchange/exchange.hpp"
#include "handler/feed_handler.hpp"
#include "network/lossy_link.hpp"
#include "stats/stats.hpp"

using namespace std;

namespace{

const char* const usage = R"(feed_bench: measures the feed handler over many seeds and reports each figure as
a mean with its 95% confidence interval.

usage: feed_bench [section] [options]

sections (default: all)
  speed     throughput of the handler, and of the whole pipeline, at each loss rate
  stages    the handler's cost split into the book update and sequencing
  latency   every on_event call timed, by what it did; book lag; a queueing model
  correct   the book against the exchange's, and what recovery did, at each loss rate
  window    each reorder window: cost, recoveries and book lag
  recovery  the cost of one recovery at each gap size: retransmission or snapshot
  compare   two settings of one parameter on the same seeds, with a paired t-test
  all       every section but compare

options (defaults in brackets)
  --seeds N        independent runs behind each figure, 2 or more        [10]
  --seed N         the first seed; runs use N, N+1, ...                  [1]
  --events N       events per run; 1e6 is fine                           [1e6]
  --losses LIST    loss rates for speed and correct                      [0,1%,5%,10%,20%,30%]
  --loss P         loss for latency, window and compare                  [5%]
  --dup P          chance an event arrives twice                         [1%]
  --reorder P      chance an event arrives one place late                [2%]
  --history N      events the exchange keeps for retransmission          [10000]
  --window N       arrivals a gap waits before it counts as loss         [3]
  --windows LIST   windows for the window section                        [0,1,2,3,5,8,16]
  --recovery R     first: one hole at a time; every: all at once         [every]
  --gaps LIST      gap sizes for the recovery section                    [1,3,10,30,100,300,1000,3000,10000]
  --param NAME     compare: window, recovery, history, loss, dup or reorder
  --a V, --b V     compare: the two settings of that parameter
  --csv DIR        also write every per-seed value to DIR/bench.csv
  -h, --help       show this text

examples
  feed_bench speed --seeds 30
  feed_bench correct --losses 0,10%,40% --events 2e5
  feed_bench compare --param recovery --a first --b every --loss 20%
  feed_bench compare --param window --a 0 --b 3 --seeds 20 --csv results

Build with -DCMAKE_BUILD_TYPE=Release. Exit status: 0 if every book matched, 1 if one
did not, 2 if the command line could not be used.
)";

using ull = unsigned long long;
constexpr uint64_t link_salt = 0x9e3779b97f4a7c15ULL;  /**< as in feed_demo */

/** Everything a run depends on but its seed. */
struct Setup{
  uint64_t events = 1'000'000;
  size_t history = 10'000;
  size_t window = 3;
  Recovery recovery = Recovery::every_hole;
  double loss = 0.05, duplicate = 0.01, reorder = 0.02;
  vector<pair<uint64_t, uint64_t>> drops;

  LinkConfig link(uint64_t seed) const{
    LinkConfig config;
    config.loss = loss;
    config.duplicate = duplicate;
    config.reorder = reorder;
    config.drop_ranges = drops;
    config.seed = seed ^ link_salt;
    return config;
  }
};

/** The bench's settings, read from the command line. */
struct Settings{
  Setup base;
  uint64_t first_seed = 1, seeds = 10;
  vector<double> losses;
  vector<uint64_t> windows, gaps;
  string csv_path;

  vector<uint64_t> seed_list() const{
    vector<uint64_t> list;
    for (uint64_t s = 0; s < seeds; ++s){
      list.push_back(first_seed + s);
    }
    return list;
  }
};
Settings settings;

uint64_t sink = 0;      /**< takes results, so the compiler cannot drop the timed work */
double clock_cost = 0;  /**< ns a pair of clock readings adds to one timed call, on average */
bool all_matched = true;

int64_t now_ns(){
  return chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename T>
T nearest_rank(const vector<T>& sorted, double p){
  if (sorted.empty()){
    return T{};
  }
  const auto rank = static_cast<size_t>(ceil(p / 100.0 * static_cast<double>(sorted.size())));
  return sorted[min(max<size_t>(rank, 1), sorted.size()) - 1];
}

// ---- output ----

string number(double value, int precision){
  char text[64];
  snprintf(text, sizeof text, "%.*f", precision, value);
  return text;
}

string percent(double chance){
  return number(chance * 100, chance * 100 == floor(chance * 100) ? 0 : 1) + "%";
}

/** @return "mean ± half-width", with `precision` decimals, or fewer for big values */
string pm(const Estimate& e, int precision = 1){
  if (fabs(e.mean) >= 1'000 && precision > 0){
    precision = 0;
  }
  return number(e.mean, precision) + " ± " + number(e.half_width, precision);
}

/** @return `text` right-aligned in `width` columns; ± counts as one */
string cell(const string& text, size_t width){
  size_t columns = 0;
  for (const char c : text){
    columns += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }
  return columns >= width ? text : string(width - columns, ' ') + text;
}

void print_row(const vector<string>& cells, const vector<size_t>& widths){
  string line = " ";
  for (size_t i = 0; i < cells.size(); ++i){
    line += " " + (i == 0 ? cells[i] + string(widths[i] > cells[i].size() ? widths[i] - cells[i].size() : 0, ' ')
                          : cell(cells[i], widths[i]));
  }
  puts(line.c_str());
}

/** One value per seed for each metric of one setting; every value also goes to the CSV. */
class Series{
 public:
  Series(string section, string setting) : section_(std::move(section)), setting_(std::move(setting)){}

  void add(const string& metric, uint64_t seed, double value){
    values_[metric].push_back(value);
    rows.push_back(section_ + "," + setting_ + "," + metric + "," + to_string(seed) + "," + number(value, 3));
  }

  Estimate operator[](const string& metric) const{
    return estimate(values_.at(metric));
  }

  const vector<double>& values(const string& metric) const{
    return values_.at(metric);
  }

  bool has(const string& metric) const{
    return values_.contains(metric);
  }

  static inline vector<string> rows;  /**< the CSV, without its header */

 private:
  string section_, setting_;
  map<string, vector<double>> values_;
};

// ---- one run ----

/** What one exchange -> link -> handler run did. */
struct Run{
  double handler_ns = 0;  /**< handler time per published event, clock cost taken out */
  double whole_ns = 0;    /**< whole run per published event; meaningful only untimed */
  HandlerStats stats;
  uint64_t lost = 0;
  uint64_t only_late = 0;  /**< duplicates the link did not make: recovered events whose originals came anyway */
  uint64_t checks = 0, wrong = 0;
  vector<uint32_t> lag;  /**< per seq, newer events already published when it was applied; sorted */
  bool matched = false;

  double lag_at(double p) const{
    return nearest_rank(lag, p);
  }

  double delayed_percent() const{
    const auto delayed = lag.end() - upper_bound(lag.begin(), lag.end(), 0u);
    return 100.0 * static_cast<double>(delayed) / static_cast<double>(lag.size());
  }
};

/** How much one run measures, beyond its stats. */
struct Measure{
  bool time_handler = false;  /**< read the clock around each batch of arrivals */
  bool lag = false;           /**< record every seq's lag */
  uint64_t check_every = 0;   /**< compare the books at catch-ups at most this often; 0: only at the end */
};

Run run_once(const Setup& setup, uint64_t seed, Measure measure){
  Exchange exchange(seed, setup.history);
  LossyLink link(setup.link(seed));
  FeedHandler handler(exchange, setup.window, setup.recovery);
  Run run;
  vector<uint32_t> lag(measure.lag ? setup.events + 1 : 0);
  uint64_t link_duplicates = 0, last_check = 0;
  int64_t spent = 0, readings = 0;

  const auto record = [&](uint64_t applied_before){
    if (measure.lag){
      for (uint64_t s = applied_before + 1; s <= handler.last_applied(); ++s){
        lag[s] = static_cast<uint32_t>(exchange.last_seq() - s);
      }
    }
  };
  const auto deliver = [&](const vector<Event>& arrived){
    if (arrived.empty()){
      return;
    }
    const uint64_t applied_before = handler.last_applied();
    for (size_t i = 1; i < arrived.size(); ++i){
      link_duplicates += arrived[i].seq == arrived[i - 1].seq;
    }
    if (measure.time_handler){
      const int64_t start = now_ns();
      for (const Event& event : arrived){
        handler.on_event(event);
      }
      spent += now_ns() - start;
      ++readings;
    } else{
      for (const Event& event : arrived){
        handler.on_event(event);
      }
    }
    record(applied_before);
    if (measure.check_every > 0 && handler.last_applied() == exchange.last_seq() &&
        exchange.last_seq() >= last_check + measure.check_every){
      last_check = exchange.last_seq();
      ++run.checks;
      run.wrong += !(handler.book() == exchange.book());
    }
  };

  const int64_t start = now_ns();
  for (uint64_t sent = 0; sent < setup.events; ++sent){
    deliver(link.send(exchange.next()));
  }
  deliver(link.flush());
  const uint64_t applied_before = handler.last_applied();
  const int64_t heartbeat = now_ns();
  handler.on_heartbeat(exchange.last_seq());
  spent += now_ns() - heartbeat;
  ++readings;
  record(applied_before);
  const double events = static_cast<double>(setup.events);
  run.whole_ns = static_cast<double>(now_ns() - start) / events;
  run.handler_ns = (static_cast<double>(spent) - static_cast<double>(readings) * clock_cost) / events;

  run.stats = handler.stats();
  run.lost = link.lost();
  run.only_late = run.stats.duplicates - link_duplicates;
  run.matched = handler.book() == exchange.book() && handler.last_applied() == exchange.last_seq();
  all_matched = all_matched && run.matched && run.wrong == 0;
  if (measure.lag){
    lag.erase(lag.begin());
    sort(lag.begin(), lag.end());
    run.lag = std::move(lag);
  }
  return run;
}

/** 1% random loss plus 49 scripted bursts, alternately twice the history long (snapshot) and a 20th of it (retransmission). */
Setup bursts(Setup setup){
  const uint64_t spacing = max<uint64_t>(setup.events / 50, 8);
  setup.history = max<uint64_t>(min<uint64_t>(1'000, spacing / 4), 1);
  setup.loss = 0.01;
  setup.drops.clear();
  for (uint64_t k = 1; k < 50 && k * spacing < setup.events; ++k){
    const uint64_t length = k % 2 == 1 ? 2 * setup.history : max<uint64_t>(setup.history / 20, 1);
    setup.drops.push_back({k * spacing, k * spacing + length - 1});
  }
  return setup;
}

// ---- speed ----

void speed(){
  printf("== speed: ns per published event, and millions of events a second ==\n");
  const vector<size_t> widths = {6, 15, 14, 16, 14};
  print_row({"loss", "handler ns", "handler M/s", "whole run ns", "whole M/s"}, widths);
  Setup setup = settings.base;
  run_once(setup, settings.first_seed + settings.seeds, {});  // warms caches and predictors
  for (const double loss : settings.losses){
    setup.loss = loss;
    Series series("speed", percent(loss));
    for (const uint64_t seed : settings.seed_list()){
      const double handler = run_once(setup, seed, {.time_handler = true}).handler_ns;
      const double whole = run_once(setup, seed, {}).whole_ns;
      series.add("handler_ns", seed, handler);
      series.add("handler_mps", seed, 1e3 / handler);
      series.add("whole_ns", seed, whole);
      series.add("whole_mps", seed, 1e3 / whole);
    }
    print_row({percent(loss), pm(series["handler_ns"]), pm(series["handler_mps"], 2), pm(series["whole_ns"]),
               pm(series["whole_mps"], 2)},
              widths);
  }
  puts("  handler: on_event and on_heartbeat calls only, timed per batch of arrivals, with the clock's cost\n"
       "  taken out. whole run: exchange + link + handler, untimed inside.\n");
}

// ---- stages ----

/** Replays an undamaged stream into a bare OrderBook (the book update alone) and into a FeedHandler
    (sequencing as well), alternating which goes first. */
void stages(){
  printf("== stages: an undamaged stream replayed; ns per event ==\n");
  Series series("stages", "undamaged");
  for (const uint64_t seed : settings.seed_list()){
    vector<Event> stream;
    stream.reserve(settings.base.events);
    Exchange exchange(seed, 1);
    for (uint64_t i = 0; i < settings.base.events; ++i){
      stream.push_back(exchange.next());
    }
    const auto run_book = [&]{
      OrderBook book;
      const int64_t start = now_ns();
      for (const Event& event : stream){
        sink += book.apply(event);
      }
      return static_cast<double>(now_ns() - start) / static_cast<double>(stream.size());
    };
    const auto run_handler = [&]{
      FeedHandler handler(exchange, settings.base.window, settings.base.recovery);
      const int64_t start = now_ns();
      for (const Event& event : stream){
        handler.on_event(event);
      }
      const double ns = static_cast<double>(now_ns() - start) / static_cast<double>(stream.size());
      all_matched = all_matched && handler.book() == exchange.book();
      return ns;
    };
    if (seed == settings.first_seed){
      run_book();
      run_handler();
    }
    double book = 0, handler = 0;
    if (seed % 2 == 0){
      book = run_book();
      handler = run_handler();
    } else{
      handler = run_handler();
      book = run_book();
    }
    series.add("book_ns", seed, book);
    series.add("handler_ns", seed, handler);
    series.add("sequencing_ns", seed, handler - book);
  }
  const vector<size_t> widths = {34, 14};
  print_row({"book update: OrderBook::apply", pm(series["book_ns"])}, widths);
  print_row({"sequencing + book: on_event", pm(series["handler_ns"])}, widths);
  print_row({"sequencing alone (the difference)", pm(series["sequencing_ns"])}, widths);
  printf("  sequencing is %.0f%% of on_event\n\n", 100 * series["sequencing_ns"].mean / series["handler_ns"].mean);
}

// ---- latency ----

enum Kind{ in_order, buffered_early, late_fill, dup_dropped, by_retransmit, by_snapshot, kinds };
const char* const kind_names[kinds] = {"in order: applied", "early: buffered", "late: fills a gap",
                                       "duplicate: dropped", "recovery: retransmit", "recovery: snapshot"};

/** Every on_event call of one run, timed and sorted by what it did. @return the call times in arrival order */
vector<int64_t> timed_calls(const Setup& setup, uint64_t seed, vector<int64_t> (&by_kind)[kinds], Run& run){
  Exchange exchange(seed, setup.history);
  LossyLink link(setup.link(seed));
  FeedHandler handler(exchange, setup.window, setup.recovery);
  vector<uint32_t> lag(setup.events + 1);
  vector<int64_t> calls;
  calls.reserve(setup.events + setup.events / 8);
  const auto arrive = [&](const Event& event){
    const HandlerStats before = handler.stats();
    const uint64_t applied_before = handler.last_applied();
    const size_t buffered_before = handler.buffered();
    const int64_t start = now_ns();
    handler.on_event(event);
    const int64_t took = now_ns() - start;
    const HandlerStats& after = handler.stats();
    const uint64_t applied = handler.last_applied();
    const Kind kind = after.snapshots > before.snapshots       ? by_snapshot
                      : after.retransmits > before.retransmits ? by_retransmit
                      : after.duplicates > before.duplicates   ? dup_dropped
                      : applied == applied_before              ? buffered_early
                      : buffered_before == 0                   ? in_order
                                                               : late_fill;
    by_kind[kind].push_back(took);
    calls.push_back(took);
    for (uint64_t s = applied_before + 1; s <= applied; ++s){
      lag[s] = static_cast<uint32_t>(exchange.last_seq() - s);
    }
  };
  for (uint64_t sent = 0; sent < setup.events; ++sent){
    for (const Event& event : link.send(exchange.next())){
      arrive(event);
    }
  }
  for (const Event& event : link.flush()){
    arrive(event);
  }
  const uint64_t applied_before = handler.last_applied();
  handler.on_heartbeat(exchange.last_seq());
  for (uint64_t s = applied_before + 1; s <= handler.last_applied(); ++s){
    lag[s] = 0;
  }
  run.matched = handler.book() == exchange.book() && handler.last_applied() == exchange.last_seq();
  all_matched = all_matched && run.matched;
  lag.erase(lag.begin());
  sort(lag.begin(), lag.end());
  run.lag = std::move(lag);
  return calls;
}

/** The measured calls as a single-server queue: Poisson arrivals at a share of the handler's capacity; a call
    over 50 us, the machine stalling rather than handler work, is replaced by the median call. */
void load(const vector<int64_t>& measured){
  constexpr double stall_ns = 50'000;
  vector<double> service;
  for (const int64_t t : measured){
    service.push_back(max(1.0, static_cast<double>(t) - clock_cost));
  }
  vector<double> sorted = service;
  sort(sorted.begin(), sorted.end());
  size_t stalls = 0;
  double total = 0;
  for (double& s : service){
    if (s > stall_ns){
      s = nearest_rank(sorted, 50);
      ++stalls;
    }
    total += s;
  }
  shuffle(service.begin(), service.end(), mt19937_64(settings.first_seed));
  const double mean = total / static_cast<double>(service.size());
  printf("  load model (seed %llu): the calls as a queue with Poisson arrivals; %zu stalls over 50 us replaced by the\n"
         "  median call; mean service %.0f ns, so capacity %.2f M arrivals/s. ns of wait + service:\n",
         ull(settings.first_seed), stalls, mean, 1e3 / mean);
  const vector<size_t> widths = {6, 13, 8, 8, 8, 10};
  print_row({"load", "arrivals/s", "p50", "p99", "p99.9", "max"}, widths);
  for (const double rho : {0.1, 0.5, 0.7, 0.8, 0.9, 0.95}){
    mt19937_64 random(settings.first_seed);
    exponential_distribution<double> next_gap(rho / mean);
    double arrival = 0, free_at = 0;
    vector<double> latency(service.size());
    for (size_t i = 0; i < service.size(); ++i){
      arrival += next_gap(random);
      free_at = max(arrival, free_at) + service[i];
      latency[i] = free_at - arrival;
    }
    sort(latency.begin(), latency.end());
    print_row({percent(rho), number(rho * 1e3 / mean, 2) + " M", number(nearest_rank(latency, 50), 0),
               number(nearest_rank(latency, 99), 0), number(nearest_rank(latency, 99.9), 0),
               number(latency.back(), 0)},
              widths);
  }
}

void latency(){
  printf("== latency: every on_event call timed, by what it did; ns per call ==\n");
  vector<int64_t> model_calls;
  for (const auto& [name, setup] : {pair<string, Setup>{"random " + percent(settings.base.loss) + " loss", settings.base},
                                    pair<string, Setup>{"bursts", bursts(settings.base)}}){
    Series series("latency", name);
    double share[kinds + 1] = {};
    for (const uint64_t seed : settings.seed_list()){
      vector<int64_t> by_kind[kinds];
      Run run;
      if (seed == settings.first_seed){
        vector<int64_t> warm[kinds];
        Run ignored;
        timed_calls(setup, seed + settings.seeds, warm, ignored);
      }
      vector<int64_t> calls = timed_calls(setup, seed, by_kind, run);
      if (model_calls.empty()){
        model_calls = calls;
      }
      double total = 0;
      for (const int64_t t : calls){
        total += static_cast<double>(t);
      }
      for (int k = 0; k <= kinds; ++k){
        vector<int64_t>& v = k == kinds ? calls : by_kind[k];
        if (v.empty()){
          continue;
        }
        sort(v.begin(), v.end());
        double sum = 0;
        for (const int64_t t : v){
          sum += static_cast<double>(t);
        }
        share[k] += 100 * sum / total / static_cast<double>(settings.seeds);
        const string kind = k == kinds ? "all" : to_string(k);
        series.add(kind + "_calls", seed, static_cast<double>(v.size()));
        series.add(kind + "_p50", seed, static_cast<double>(nearest_rank(v, 50)));
        series.add(kind + "_p99", seed, static_cast<double>(nearest_rank(v, 99)));
        series.add(kind + "_p999", seed, static_cast<double>(nearest_rank(v, 99.9)));
      }
      series.add("lag_p99", seed, run.lag_at(99));
      series.add("lag_p999", seed, run.lag_at(99.9));
      series.add("lag_max", seed, run.lag.back());
      series.add("delayed_percent", seed, run.delayed_percent());
    }
    printf("  %s:\n", name.c_str());
    const vector<size_t> widths = {22, 14, 11, 12, 15, 7};
    print_row({"call", "calls a run", "p50", "p99", "p99.9", "time"}, widths);
    for (int k = 0; k <= kinds; ++k){
      const string kind = k == kinds ? "all" : to_string(k);
      if (!series.has(kind + "_calls")){
        continue;  // no call of this kind in any seed
      }
      // a kind of call that did not happen in every seed has fewer samples than seeds
      const auto show = [&](const string& metric){
        return series.values(metric).size() < 2 ? number(series[metric].mean, 0) : pm(series[metric], 0);
      };
      print_row({k == kinds ? "all arrivals" : kind_names[k], show(kind + "_calls"), show(kind + "_p50"),
                 show(kind + "_p99"), show(kind + "_p999"), number(share[k], 1) + "%"},
                widths);
    }
    printf("    book lag, in newer events already published when a seq was applied: p99 %s, p99.9 %s, max %s;"
           " %s%% of seqs delayed\n", pm(series["lag_p99"]).c_str(), pm(series["lag_p999"]).c_str(),
           pm(series["lag_max"]).c_str(), pm(series["delayed_percent"], 2).c_str());
  }
  load(model_calls);
  puts("");
}

// ---- correctness ----

void correct_row(const string& label, const Setup& setup){
  Series series("correct", label);
  uint64_t matched = 0, checks = 0, wrong = 0;
  for (const uint64_t seed : settings.seed_list()){
    const Run run = run_once(setup, seed, {.check_every = 100});
    matched += run.matched;
    checks += run.checks;
    wrong += run.wrong;
    series.add("lost", seed, static_cast<double>(run.lost));
    series.add("gaps", seed, static_cast<double>(run.stats.gaps));
    series.add("retransmits", seed, static_cast<double>(run.stats.retransmits));
    series.add("recovered", seed, static_cast<double>(run.stats.recovered));
    series.add("snapshots", seed, static_cast<double>(run.stats.snapshots));
    series.add("only_late", seed, static_cast<double>(run.only_late));
    series.add("rejected", seed, static_cast<double>(run.stats.rejected));
  }
  print_row({label, pm(series["lost"], 0), pm(series["gaps"], 0), pm(series["retransmits"], 0),
             pm(series["snapshots"], 1), pm(series["only_late"], 0), number(series["rejected"].mean, 0),
             to_string(checks), to_string(wrong), to_string(matched) + "/" + to_string(settings.seeds)},
            {10, 16, 16, 16, 11, 13, 6, 9, 5, 8});
}

void correct(){
  printf("== correctness: the handler's book against the exchange's, at every catch-up (at most every 100 events)\n"
         "   and at the end; what recovery did, per run ==\n");
  print_row({"damage", "lost", "gaps", "retransmits", "snapshots", "only late", "reject", "checks", "wrong", "matched"},
            {10, 16, 16, 16, 11, 13, 6, 9, 5, 8});
  for (const double loss : settings.losses){
    Setup setup = settings.base;
    setup.loss = loss;
    correct_row(percent(loss) + " loss", setup);
  }
  correct_row("bursts", bursts(settings.base));
  puts("  only late: duplicates the link did not make: events recovered whose originals then arrived anyway.\n"
       "  checks and wrong are totals over every seed; matched counts the seeds whose final book matched.\n");
}

// ---- reorder window ----

void window(){
  printf("== reorder window: %s loss, %s reordered, %s duplicated ==\n", percent(settings.base.loss).c_str(),
         percent(settings.base.reorder).c_str(), percent(settings.base.duplicate).c_str());
  const vector<size_t> widths = {6, 13, 17, 15, 12, 12, 14};
  print_row({"window", "handler ns", "retransmits", "only late", "lag p99", "lag max", "seqs delayed"}, widths);
  Setup setup = settings.base;
  run_once(setup, settings.first_seed + settings.seeds, {});
  for (const uint64_t w : settings.windows){
    setup.window = w;
    Series series("window", to_string(w));
    for (const uint64_t seed : settings.seed_list()){
      const Run run = run_once(setup, seed, {.time_handler = true, .lag = true});
      series.add("handler_ns", seed, run.handler_ns);
      series.add("retransmits", seed, static_cast<double>(run.stats.retransmits));
      series.add("only_late", seed, static_cast<double>(run.only_late));
      series.add("lag_p99", seed, run.lag_at(99));
      series.add("lag_max", seed, run.lag.back());
      series.add("delayed_percent", seed, run.delayed_percent());
    }
    print_row({to_string(w), pm(series["handler_ns"]), pm(series["retransmits"], 0), pm(series["only_late"], 0),
               pm(series["lag_p99"]), pm(series["lag_max"], 0), pm(series["delayed_percent"], 2) + "%"},
              widths);
  }
  puts("");
}

// ---- recovery cost ----

/** Loses seqs 5,001 to 5,000 + gap and times the one on_event call that recovers them. */
double recovery_call(uint64_t gap, size_t history, uint64_t seed){
  Exchange exchange(seed, history);
  LinkConfig config;
  config.drop_ranges = {{5'001, 5'000 + gap}};
  config.seed = seed ^ link_salt;
  LossyLink link(config);
  FeedHandler handler(exchange, settings.base.window, settings.base.recovery);
  while (true){
    for (const Event& event : link.send(exchange.next())){
      const HandlerStats before = handler.stats();
      const int64_t start = now_ns();
      handler.on_event(event);
      const int64_t took = now_ns() - start;
      if (handler.stats().retransmits > before.retransmits || handler.stats().snapshots > before.snapshots){
        all_matched = all_matched && handler.book() == exchange.book();
        return static_cast<double>(took) / 1e3;
      }
    }
  }
}

void recovery(){
  printf("== recovery cost: the one on_event call that closes a gap, in us; the history covers the gap\n"
         "   (retransmission) or keeps 1 event (snapshot) ==\n");
  const vector<size_t> widths = {8, 18, 15, 18};
  print_row({"gap", "retransmission", "ns per event", "snapshot"}, widths);
  for (const uint64_t gap : settings.gaps){
    Series series("recovery", to_string(gap));
    for (const uint64_t seed : settings.seed_list()){
      series.add("retransmit_us", seed, recovery_call(gap, 5'000 + gap + 1'000, seed));
      series.add("snapshot_us", seed, recovery_call(gap, 1, seed));
    }
    const Estimate r = series["retransmit_us"];
    print_row({to_string(gap), pm(r, 2), number(r.mean * 1e3 / static_cast<double>(gap), 0),
               pm(series["snapshot_us"], 2)},
              widths);
  }
  puts("  a snapshot copies the exchange's whole book (at most 200 orders here), whatever the gap.\n");
}

// ---- compare ----

struct Comparing{
  string param, a, b;
  Setup setup_a, setup_b;
};
Comparing comparing;

void compare(){
  const Comparing& c = comparing;
  printf("== compare: --%s %s (A) against %s (B), on the same %llu seeds; paired t-test of B - A ==\n",
         c.param.c_str(), c.a.c_str(), c.b.c_str(), ull(settings.seeds));
  Series a("compare", "A"), b("compare", "B");
  run_once(c.setup_a, settings.first_seed + settings.seeds, {});
  for (const uint64_t seed : settings.seed_list()){
    for (int turn = 0; turn < 2; ++turn){
      const bool run_a = (turn == 0) == (seed % 2 == 0);  // alternate which goes first, so drift hits both
      Series& series = run_a ? a : b;
      const Run run = run_once(run_a ? c.setup_a : c.setup_b, seed, {.time_handler = true, .lag = true});
      series.add("handler ns per event", seed, run.handler_ns);
      series.add("retransmissions", seed, static_cast<double>(run.stats.retransmits));
      series.add("events recovered", seed, static_cast<double>(run.stats.recovered));
      series.add("snapshots", seed, static_cast<double>(run.stats.snapshots));
      series.add("only-late duplicates", seed, static_cast<double>(run.only_late));
      series.add("book lag p99", seed, run.lag_at(99));
      series.add("book lag p99.9", seed, run.lag_at(99.9));
      series.add("book lag max", seed, run.lag.back());
      series.add("seqs delayed %", seed, run.delayed_percent());
    }
  }
  const vector<size_t> widths = {21, 17, 17, 17, 9, 0};
  print_row({"metric", "A", "B", "B - A", "p", "verdict"}, widths);
  for (const string metric : {"handler ns per event", "retransmissions", "events recovered", "snapshots",
                              "only-late duplicates", "book lag p99", "book lag p99.9", "book lag max",
                              "seqs delayed %"}){
    const Comparison result = compare_paired(a.values(metric), b.values(metric));
    const string verdict = result.difference.sd == 0 && result.difference.mean == 0 ? "the same in every seed"
                           : result.p_value < 0.05 ? (result.difference.mean > 0 ? "B is higher" : "B is lower")
                                                   : "no significant difference";
    const int precision = metric == "seqs delayed %" ? 2 : 1;
    print_row({metric, pm(result.a, precision), pm(result.b, precision), pm(result.difference, precision),
               result.p_value < 0.0001 ? "<0.0001" : number(result.p_value, 4), verdict},
              widths);
  }
  puts("  p: the chance of a difference this large if the settings truly made none; below 0.05 counts as\n"
       "  significant. Counts are exact per seed, so they vary only between seeds.\n");
}

// ---- the command line ----

void read_settings(const Options& options){
  Setup& base = settings.base;
  settings.seeds = options.integer("seeds", 10, 2, 100'000);
  settings.first_seed = options.integer("seed", 1, 0, UINT64_MAX - settings.seeds - 1);
  base.events = options.integer("events", 1'000'000, 1, 100'000'000);
  base.loss = options.probability("loss", 0.05);
  base.duplicate = options.probability("dup", 0.01);
  base.reorder = options.probability("reorder", 0.02);
  base.history = options.integer("history", 10'000, 1, 1'000'000'000);
  base.window = options.integer("window", 3, 0, 1'000'000);
  base.recovery = options.choice("recovery", "every", {"first", "every"}) == "first" ? Recovery::first_hole
                                                                                    : Recovery::every_hole;
  settings.losses = options.probabilities("losses", {0, 0.01, 0.05, 0.10, 0.20, 0.30});
  settings.windows = options.integers("windows", {0, 1, 2, 3, 5, 8, 16}, 0, 1'000'000);
  settings.gaps = options.integers("gaps", {1, 3, 10, 30, 100, 300, 1'000, 3'000, 10'000}, 1, 10'000'000);
  const string csv_dir = options.text("csv", "");
  if (options.has("csv") && csv_dir.empty()){
    throw UsageError("--csv needs a directory");
  }
  settings.csv_path = csv_dir.empty() ? "" : csv_dir + "/bench.csv";
}

void read_compare(const Options& options){
  if (!options.has("param") || !options.has("a") || !options.has("b")){
    throw UsageError("compare needs --param, --a and --b, e.g. --param window --a 0 --b 3");
  }
  Comparing& c = comparing;
  c.param = options.choice("param", "", {"window", "recovery", "history", "loss", "dup", "reorder"});
  c.setup_a = c.setup_b = settings.base;
  for (const string side : {"a", "b"}){
    Setup& setup = side == "a" ? c.setup_a : c.setup_b;
    string& text = side == "a" ? c.a : c.b;
    if (c.param == "window"){
      setup.window = options.integer(side, 0, 0, 1'000'000);
      text = to_string(setup.window);
    } else if (c.param == "history"){
      setup.history = options.integer(side, 0, 1, 1'000'000'000);
      text = to_string(setup.history);
    } else if (c.param == "recovery"){
      text = options.choice(side, "", {"first", "every"});
      setup.recovery = text == "first" ? Recovery::first_hole : Recovery::every_hole;
    } else{
      const double p = options.probability(side, 0);
      (c.param == "loss" ? setup.loss : c.param == "dup" ? setup.duplicate : setup.reorder) = p;
      text = percent(p);
    }
  }
}

/** The clock's own cost, and how often the machine pauses the process: both put a floor under the times. */
void calibrate(){
  vector<int64_t> pairs(1'000'000);
  for (int64_t& t : pairs){
    const int64_t start = now_ns();
    t = now_ns() - start;
  }
  // The mean of the fastest 99%, not the median: where the clock is coarse
  // (41.7 ns steps on Apple silicon) most pairs read 0, but the mean is still
  // the true cost.
  sort(pairs.begin(), pairs.end());
  const int64_t clock_p50 = nearest_rank(pairs, 50);
  pairs.resize(pairs.size() * 99 / 100);
  for (const int64_t t : pairs){
    clock_cost += static_cast<double>(t);
  }
  clock_cost /= static_cast<double>(pairs.size());

  vector<int64_t> pauses;
  for (int64_t start = now_ns(), last = start, t = start; t - start < 1'000'000'000; last = t){
    t = now_ns();
    if (t - last > 1'000){
      pauses.push_back(t - last);
    }
  }
  sort(pauses.begin(), pauses.end());
  int64_t paused = 0;
  for (const int64_t p : pauses){
    paused += p;
  }
  printf("a pair of clock readings costs %.1f ns (p50 %lld); the machine paused the process %zu times over 1 us\n"
         "in 1 s of spinning (p50 %.1f us, max %.1f us, %.3f%% of the time)\n\n", clock_cost,
         static_cast<long long>(clock_p50), pauses.size(), static_cast<double>(nearest_rank(pauses, 50)) / 1e3,
         static_cast<double>(pauses.empty() ? 0 : pauses.back()) / 1e3, static_cast<double>(paused) / 1e7);
}

}  // namespace

int main(int argc, char** argv){
  setvbuf(stdout, nullptr, _IOLBF, 0);  // a long run sent to a file shows its progress
  try{
    const Options options(vector<string>(argv + 1, argv + argc), {"help"},
                          {"seeds", "seed", "events", "losses", "loss", "dup", "reorder", "history", "window",
                           "windows", "recovery", "gaps", "param", "a", "b", "csv"});
    if (options.has("help")){
      fputs(usage, stdout);
      return EXIT_SUCCESS;
    }
    options.expect_positional(1);
    const string what = options.positional().empty() ? "all" : options.positional()[0];
    const vector<pair<string, void (*)()>> sections = {{"speed", speed},     {"stages", stages}, {"latency", latency},
                                                       {"correct", correct}, {"window", window}, {"recovery", recovery},
                                                       {"compare", compare}};
    if (what != "all" && none_of(sections.begin(), sections.end(), [&](const auto& s){ return s.first == what; })){
      throw UsageError("unknown section \"" + what +
                       "\"; choose speed, stages, latency, correct, window, recovery, compare or all");
    }
    read_settings(options);
    if (what == "compare"){
      read_compare(options);
    } else if (options.has("param") || options.has("a") || options.has("b")){
      throw UsageError("--param, --a and --b belong to the compare section");
    }
    FILE* csv = nullptr;
    if (!settings.csv_path.empty()){
      csv = fopen(settings.csv_path.c_str(), "w");  // checked now, before anything runs
      if (csv == nullptr){
        throw UsageError("--csv: cannot write " + settings.csv_path + "; does the directory exist?");
      }
    }

    printf("feed_bench: %llu seeds from %llu, %llu events a run; history %zu, window %zu, %s recovery;\n"
           "every figure is a mean ± its 95%% confidence interval over the seeds (Student's t, %llu d.f.)\n",
           ull(settings.seeds), ull(settings.first_seed), ull(settings.base.events), settings.base.history,
           settings.base.window, settings.base.recovery == Recovery::every_hole ? "every-hole" : "first-hole",
           ull(settings.seeds - 1));
    calibrate();
    for (const auto& [name, run] : sections){
      if (what == name || (what == "all" && name != "compare")){
        run();
      }
    }
    if (csv != nullptr){
      fputs("section,setting,metric,seed,value\n", csv);
      for (const string& row : Series::rows){
        fprintf(csv, "%s\n", row.c_str());
      }
      fclose(csv);
      printf("per-seed values written to %s\n", settings.csv_path.c_str());
    }
    if (!all_matched){
      fputs("feed_bench: a handler's book DIFFERS from the exchange's\n", stderr);
      return EXIT_FAILURE;
    }
    return sink == 42 ? 3 : EXIT_SUCCESS;  // reads the sink, so the timed work stays observable
  } catch (const UsageError& error){
    fprintf(stderr, "feed_bench: %s\nRun feed_bench --help to see the sections and options.\n", error.what());
    return 2;
  } catch (const exception& error){
    fprintf(stderr, "feed_bench: internal error: %s\n", error.what());
    return 3;
  }
}
