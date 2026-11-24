// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

/**
 * @file
 * feed_demo: runs the feed handler against an exchange through a lossy
 * link and shows what happens, one scenario at a time. Run with no arguments,
 * it asks which scenario to run and how: the number of events, the seed, the
 * damage. Every setting can also be given on the command line, every run uses
 * a fresh random seed unless one is given, and every run prints its seed and
 * the command that replays it exactly. Each scenario ends by checking the
 * handler's book against the exchange's own book. `feed_demo --help` lists
 * the scenarios and options.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "cli/options.hpp"
#include "exchange/exchange.hpp"
#include "handler/feed_handler.hpp"
#include "lamport/lamport.hpp"
#include "multi/multi_exchange.hpp"
#include "multi/multi_feed_handler.hpp"
#include "multi/multi_lossy_link.hpp"
#include "network/lossy_link.hpp"

using namespace std;

namespace{

const char* const usage = R"(feed_demo: a market data feed handler that rebuilds an exchange's order book
from a feed that loses, duplicates and reorders events, and shows it doing so.

usage: feed_demo                       ask which scenario to run, and how, one question at a time
       feed_demo SCENARIO [options]    run a scenario with these settings
       feed_demo SCENARIO --ask        ask for its settings, offering any options given as defaults

scenarios
  watch     a live stream with heavy loss; the book stays in sync all the way
  burst     a run of lost events, recovered by one retransmission
  reorder   late and doubled events, absorbed with no recovery at all
  tail      the last events are lost; the exchange's heartbeat exposes them
  outage    a gap older than the exchange's history, recovered by a snapshot
  random    random damage at any scale, timed, with both books' fingerprints
  multi     multiple instruments partitioned with independent order books and recovery
  lamport   two handler instances with skewed clocks hand the stream over
  all       burst, reorder, tail, outage, random, multi and lamport in turn

options (defaults in brackets)
  --seed N          seed for the exchange and the link       [a new random one]
  --events N        events the exchange publishes; 1e6 works [per scenario]
  --loss P          chance an event is lost: 0.2 or 20%      [watch, random: 20%]
  --dup P           chance an event arrives twice            [1%; reorder: 15%]
  --reorder P       chance an event arrives one place late   [2%; reorder: 20%]
  --gap A-B         the seqs lost, in burst and outage       [11-15; 201-600]
  --lost N          tail: how many final events are lost     [3]
  --history N       events the exchange keeps to retransmit  [10000; outage: 100]
  --orders N        the most orders resting in the book      [200]
  --window N        arrivals a gap waits before it is lost   [3]
  --recovery R      first: one hole at a time; every: all    [every]
  --rate N          watch: events a second                   [200000]
  --latency-ms N    lamport: the handoff message's delay     [5]
  --skews-ms LIST   lamport: the clock skews to try          [0,2,4,5,6,8,10,20]
  --show N          show the first and last N events, and what happened to each
                                                             [watch, random: 5]
  --quiet           trace only gaps and recoveries; streams over 60 events always are
  --ask             ask for each setting at a prompt
  -h, --help        show this text

examples
  feed_demo
  feed_demo random --events 50000 --loss 30% --show 10
  feed_demo random --seed 482193 --events 5e6 --recovery first
  feed_demo burst --gap 101-150 --events 300
  feed_demo lamport --latency-ms 10 --skews-ms 0,10,20,40

Every run prints its seed and the command that replays it exactly.
exit status: 0 if every book matched the exchange's, 1 if one did not,
             2 if the command line could not be used
)";

constexpr uint64_t link_salt = 0x9e3779b97f4a7c15ULL;  /**< the link's seed is the exchange's seed xor this */
constexpr uint64_t max_events = 1'000'000'000;
constexpr uint64_t trace_limit = 60;  /**< streams this long or shorter trace every arrival */

/** @return `n` with commas between groups of three digits */
string grouped(uint64_t n){
  string digits = to_string(n);
  for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3){
    digits.insert(static_cast<size_t>(at), ",");
  }
  return digits;
}

/** @return a chance as a percentage, such as 20% or 2.5% */
string percent(double chance){
  ostringstream text;
  text << setprecision(3) << chance * 100 << "%";
  return text.str();
}

string hex(uint64_t value){
  ostringstream text;
  text << "0x" << setw(16) << setfill('0') << std::hex << value;
  return text.str();
}

string describe(const optional<Level>& level){
  return level ? to_string(level->price) + " x " + to_string(level->quantity) : "none";
}

string describe(Recovery recovery){
  return recovery == Recovery::every_hole ? "every-hole recovery" : "first-hole recovery";
}

/** @return an event in words, such as "add order 12: bid 37 at 9,998" */
string describe(const Event& event){
  const string order = "order " + grouped(event.order_id);
  switch (event.type){
    case EventType::add:
      return "add " + order + ": " + (event.side == Side::bid ? "bid " : "ask ") + to_string(event.quantity) + " at " +
             grouped(static_cast<uint64_t>(event.price));
    case EventType::modify:
      return "modify " + order + " to " + to_string(event.quantity);
    case EventType::remove:
      return "remove " + order;
    default:
      return "execute " + to_string(event.quantity) + " of " + order;
  }
}

string join(const vector<uint64_t>& values){
  string text;
  for (const uint64_t v : values){
    text += (text.empty() ? "" : ",") + to_string(v);
  }
  return text;
}

/** A seed for a run that names none: random, short enough to read out, and printed so the run can be replayed. */
uint64_t fresh_seed(){
  random_device device;
  const uint64_t noise = (static_cast<uint64_t>(device()) << 32) ^ device() ^
                         static_cast<uint64_t>(chrono::steady_clock::now().time_since_epoch().count());
  return 1 + noise % 999'999;
}

/** Thrown when the person at the prompt types q, or input ends. */
struct Quit{};

/**
 * Where a scenario's settings come from: the command line, or answers typed
 * at a prompt. Either way each value is checked by the same parser with the
 * same messages; at a prompt a bad answer is explained and asked again. Every
 * setting is remembered, so a run can print the command that replays it.
 */
class Settings{
 public:
  /** A check across settings: a message if the value does not fit, else nothing. */
  template <typename T>
  using Check = function<optional<string>(const T&)>;

  /**
   * @param options the command line: its values are used, or offered as the
   *        defaults when asking
   * @param asking whether to ask for each setting at a prompt
   */
  Settings(const Options& options, bool asking) : options_(options), asking_(asking){}

  /** @return whether the next setting will be asked for */
  bool asking() const{
    return asking_ && !silent_;
  }

  /** Reads settings from the command line and defaults only, even when asking, until called with false. */
  void silently(bool silent){
    silent_ = silent;
  }

  /** @return whether the command line gives this flag */
  bool flag(const string& name) const{
    return options_.has(name);
  }

  uint64_t integer(const string& name, const string& question, uint64_t fallback, uint64_t min, uint64_t max,
                   const Check<uint64_t>& check = nullptr){
    return get<uint64_t>(name, question, [&](const Options& o){ return o.integer(name, fallback, min, max); },
                         [](const uint64_t& v){ return grouped(v); }, [](const uint64_t& v){ return to_string(v); },
                         check);
  }

  double probability(const string& name, const string& question, double fallback){
    return get<double>(name, question, [&](const Options& o){ return o.probability(name, fallback); }, percent,
                       percent);
  }

  pair<uint64_t, uint64_t> range(const string& name, const string& question, pair<uint64_t, uint64_t> fallback,
                                 const Check<pair<uint64_t, uint64_t>>& check){
    const auto text = [](const pair<uint64_t, uint64_t>& r){ return to_string(r.first) + "-" + to_string(r.second); };
    return get<pair<uint64_t, uint64_t>>(name, question, [&](const Options& o){ return o.range(name, fallback); },
                                         text, text, check);
  }

  string choice(const string& name, const string& question, const string& fallback, const vector<string>& allowed){
    const auto text = [](const string& s){ return s; };
    return get<string>(name, question, [&](const Options& o){ return o.choice(name, fallback, allowed); }, text, text);
  }

  vector<uint64_t> integers(const string& name, const string& question, const vector<uint64_t>& fallback,
                            uint64_t min, uint64_t max){
    return get<vector<uint64_t>>(name, question, [&](const Options& o){ return o.integers(name, fallback, min, max); },
                                 join, join);
  }

  /** @return the seed: from the command line, typed at the prompt, or a fresh random one */
  uint64_t seed(){
    const optional<uint64_t> given =
        options_.has("seed") ? optional<uint64_t>(options_.integer("seed", 0, 0, UINT64_MAX)) : nullopt;
    optional<uint64_t> seed = given;
    if (asking()){
      while (true){
        const string answer = ask("Seed? Any number replays a run exactly; Enter picks a random one",
                                  given ? to_string(*given) : "random");
        if (answer.empty() || answer == "random"){
          seed = answer.empty() ? given : nullopt;
          break;
        }
        try{
          seed = Options({"--seed", answer}, {}, {"seed"}).integer("seed", 0, 0, UINT64_MAX);
          break;
        } catch (const UsageError& error){
          complain(error.what());
        }
      }
    }
    if (!seed){
      seed = fresh_seed();
      if (asking()){
        cout << "    the random seed is " << *seed << "\n";
      }
    }
    record("seed", to_string(*seed));
    return *seed;
  }

  /** Asks a yes-or-no question; from the command line, the answer is `fallback`. */
  bool yes(const string& question, bool fallback){
    if (!asking()){
      return fallback;
    }
    while (true){
      string answer = ask(question, fallback ? "yes" : "no");
      transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char c){ return tolower(c); });
      if (answer.empty()){
        return fallback;
      }
      if (answer == "y" || answer == "yes"){
        return true;
      }
      if (answer == "n" || answer == "no"){
        return false;
      }
      complain("answer yes or no");
    }
  }

  /** Remembers a flag the run used, for the replay command. */
  void note(const string& option){
    replay_.push_back(option);
  }

  /** @return the options that replay this run exactly, such as "--events 5000 --seed 7" */
  string replay() const{
    string text;
    for (const string& part : replay_){
      text += " " + part;
    }
    return text;
  }

  /**
   * Prints a question with its default, and reads the answer.
   *
   * @return the answer, trimmed; empty means the default
   * @throws Quit if the answer is q or quit, or input has ended
   */
  string ask(const string& question, const string& fallback){
    cout << "  " << question << " [" << fallback << "] " << flush;
    string line;
    if (!getline(cin, line)){
      cout << "\n";
      throw Quit{};
    }
    const size_t first = line.find_first_not_of(" \t\r");
    line = first == string::npos ? "" : line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (isatty(STDIN_FILENO) == 0){
      cout << line << "\n";  // typed answers echo themselves; piped ones are shown, so the transcript reads well
    }
    if (line == "q" || line == "quit"){
      throw Quit{};
    }
    return line;
  }

  void complain(const string& message){
    cout << "    " << message << "; try again\n";
  }

 private:
  /**
   * Reads one setting: from the command line, or else its default; then,
   * when asking, offers that as the default and checks the answer the same
   * way.
   */
  template <typename T, typename Read, typename Show, typename Arg>
  T get(const string& name, const string& question, Read read, Show show, Arg arg, const Check<T>& check = nullptr){
    T value = read(options_);
    string text = options_.has(name) ? options_.text(name, "") : arg(value);
    optional<string> problem = check ? check(value) : nullopt;
    if (!asking()){
      if (problem){
        throw UsageError(*problem);
      }
      record(name, text);
      return value;
    }
    for (bool first = true;; first = false){
      if (problem && !first){
        complain(*problem);
      }
      const string answer = ask(question, show(value));
      if (!answer.empty()){
        try{
          value = read(Options({"--" + name, answer}, {}, {name}));
          text = answer;
        } catch (const UsageError& error){
          complain(error.what());
          continue;
        }
      }
      problem = check ? check(value) : nullopt;
      if (!problem){
        break;
      }
    }
    record(name, text);
    return value;
  }

  void record(const string& name, const string& text){
    replay_.push_back("--" + name + " " + text);
  }

  const Options& options_;
  bool asking_;
  bool silent_ = false;
  vector<string> replay_;
};

/** The settings that shape one stream: the exchange, the link and the handler. */
struct Stream{
  uint64_t seed = 1;
  uint64_t events = 0;
  size_t history = 10'000;
  size_t orders = 200;
  size_t window = 3;
  Recovery recovery = Recovery::every_hole;
  LinkConfig link;
  bool quiet = false;
  uint64_t show = 0;  /**< how many events to follow at each end of the stream */

  /** @return a one-line summary for a scenario's heading */
  string summary() const{
    return "seed " + to_string(seed) + ", window " + to_string(window) + ", " + describe(recovery);
  }
};

/** Reads how many events a stream has, and its seed. */
Stream read_start(Settings& in, uint64_t events){
  Stream stream;
  stream.events = in.integer("events", "How many events should the exchange publish?", events, 1, max_events);
  stream.seed = in.seed();
  stream.link.seed = stream.seed ^ link_salt;
  return stream;
}

/**
 * Reads the handler's settings and the exchange's. At a prompt they are
 * asked for only if wanted, since their defaults suit every scenario.
 *
 * @param in where the settings come from
 * @param stream the stream they go into
 * @param history the scenario's default history
 * @param ask_history whether to ask for the history even so
 */
void read_handler(Settings& in, Stream& stream, size_t history, bool ask_history){
  if (ask_history){
    stream.history = in.integer("history", "How many past events does the exchange keep, to retransmit?", history, 1,
                                max_events);
  }
  in.silently(!in.yes("Change the handler's settings too (reorder window, recovery, book size)?", false));
  stream.window = in.integer("window", "Reorder window: how many arrivals may a gap wait before it counts as lost?", 3,
                             0, 1'000'000);
  stream.recovery = in.choice("recovery", "Recovery: first (one hole at a time) or every (all holes at once)?",
                              "every", {"first", "every"}) == "first"
                        ? Recovery::first_hole
                        : Recovery::every_hole;
  if (!ask_history){
    stream.history = in.integer("history", "How many past events does the exchange keep, to retransmit?", history,
                                1, max_events);
  }
  stream.orders = in.integer("orders", "At most how many orders may rest in the book?", 200, 1, 1'000'000);
  in.silently(false);
}

/** Reads how many events to show at each end of the stream, and whether to trace every arrival. */
void read_show(Settings& in, Stream& stream, uint64_t show){
  stream.show = in.integer("show", "How many events to show at each end of the stream, and what happened to each?",
                           show, 0, 1'000'000);
  stream.quiet = in.flag("quiet") || stream.events > trace_limit;
  if (in.flag("quiet")){
    in.note("--quiet");
  }
}

/**
 * Follows the first and the last few events of a stream from the exchange
 * into the handler's book, to show them at the end: each event, what the
 * link did with it, and how it reached the book.
 */
class Journal{
 public:
  /**
   * @param events the stream's length
   * @param show how many events to follow at each end
   */
  Journal(uint64_t events, uint64_t show) : events_(events), show_(min(show, events)){}

  /** Notes a published event and what the link did with it. */
  void sent(const Event& event, const vector<Event>& arrived, bool lost){
    if (!follows(event.seq)){
      return;
    }
    Entry& entry = entries_[event.seq];
    entry.event = event;
    const auto copies = count_if(arrived.begin(), arrived.end(), [&](const Event& e){ return e.seq == event.seq; });
    entry.link = lost ? "lost" : copies >= 2 ? "arrived twice" : copies == 1 ? "arrived" : "held back, arrived late";
  }

  /**
   * Notes how the events the handler has just applied reached its book.
   *
   * @param handler the handler, after the call
   * @param arrival the seq that just arrived, or 0 for a heartbeat
   * @param before the handler's counts before the call
   * @param applied_before its last applied seq before the call
   * @param buffered_before how many events were waiting before the call
   */
  void reached(const FeedHandler& handler, uint64_t arrival, const HandlerStats& before, uint64_t applied_before,
               size_t buffered_before){
    if (show_ == 0){
      return;
    }
    if (arrival != 0 && follows(arrival)){
      Entry& entry = entries_[arrival];
      if (++entry.arrivals == 1 && arrival <= applied_before){
        entry.link = "late: already recovered";  // recovered before its original came; the original is dropped
      }
    }
    const uint64_t applied = handler.last_applied();
    const bool snapshot = handler.stats().snapshots > before.snapshots;
    const auto note = [&](uint64_t first, uint64_t last){
      for (uint64_t seq = max(first, applied_before + 1); seq <= min(last, applied); ++seq){
        Entry& entry = entries_[seq];
        const bool expected = seq == applied_before + 1;  // the seq the handler was waiting for
        entry.handler = snapshot                     ? "in the book through a snapshot"
                        : seq == arrival && !expected ? "set off a recovery, then applied"
                        : seq == arrival              ? (buffered_before == 0 ? "applied at once" : "applied on arrival: closed a gap")
                        : entry.arrivals              ? "waited in the buffer, then applied"
                        : arrival == 0                ? "recovered after the heartbeat"
                                                      : "recovered by retransmission";
      }
    };
    note(1, show_);
    note(events_ - show_ + 1, events_);
  }

  /** Prints the followed events, and what happened to each. */
  void print() const{
    if (show_ == 0){
      return;
    }
    const bool everything = 2 * show_ >= events_;
    cout << "  " << (everything ? "all " + grouped(events_) + " events"
                                : "the first " + grouped(show_) + " and the last " + grouped(show_) + " of " +
                                      grouped(events_) + " events")
         << ", and what happened to each:\n"
         << "  " << setw(11) << "seq" << "  " << left << setw(36) << "event" << "  " << setw(24) << "on the link"
         << "  in the handler\n"
         << right;
    uint64_t previous = 0;
    for (const auto& [seq, entry] : entries_){
      if (previous != 0 && seq != previous + 1){
        cout << "  " << setw(11) << "..." << "  " << grouped(seq - previous - 1) << " more\n";
      }
      cout << "  " << setw(11) << grouped(seq) << "  " << left << setw(36) << describe(entry.event) << "  " << setw(24)
           << entry.link << "  " << entry.handler << "\n" << right;
      previous = seq;
    }
  }

 private:
  struct Entry{
    Event event{};
    string link, handler;
    uint64_t arrivals = 0;
  };

  bool follows(uint64_t seq) const{
    return show_ > 0 && (seq <= show_ || seq > events_ - show_);
  }

  uint64_t events_, show_;
  map<uint64_t, Entry> entries_;
};

/** Prints what the handler did with each arrival. */
class Tracer{
 public:
  /** @param quiet print only what is not a plain in-order apply */
  explicit Tracer(bool quiet) : quiet_(quiet){}

  /** Prints what the handler did with one arriving event. */
  void arrival(const FeedHandler& handler, const Event& event, const HandlerStats& before, uint64_t applied_before){
    const HandlerStats& after = handler.stats();
    const uint64_t applied = handler.last_applied();
    string what;
    if (after.duplicates > before.duplicates){
      what = "duplicate, dropped";
    } else if (after.snapshots > before.snapshots){
      what = missing() + " still missing, too old to retransmit: snapshot, book now at seq " + to_string(applied);
    } else if (after.retransmits > before.retransmits){
      what = missing() + " still missing after " + to_string(handler.reorder_window()) +
             " more arrivals: retransmitted, book now at seq " + to_string(applied);
    } else if (after.gaps > before.gaps){
      missing_first_ = applied_before + 1;
      missing_last_ = event.seq - 1;
      what = "early, buffered: gap opens, " + missing() + " missing";
    } else if (applied > applied_before + 1){
      const uint64_t drained = applied - applied_before - 1;
      what = "late, fills the gap: applied, plus " + to_string(drained) + " buffered event" + (drained == 1 ? "" : "s") +
             ", book now at seq " + to_string(applied);
    } else if (applied == applied_before + 1){
      if (quiet_){
        return;
      }
      what = "applied, book at seq " + to_string(applied);
    } else{
      what = "early, buffered (" + to_string(handler.buffered()) + " waiting)";
    }
    cout << "  seq " << setw(4) << event.seq << " arrives: " << what << "\n";
  }

  /** Prints what the handler did with the exchange's heartbeat. */
  void heartbeat(const FeedHandler& handler, uint64_t last_seq, const HandlerStats& before, uint64_t applied_before){
    cout << "  heartbeat: the exchange is at seq " << last_seq << ": ";
    if (handler.stats().retransmits > before.retransmits){
      cout << "seqs " << applied_before + 1 << "-" << last_seq << " missing: retransmitted, book now at seq "
           << handler.last_applied() << "\n";
    } else if (handler.stats().snapshots > before.snapshots){
      cout << "missing events too old to retransmit: snapshot, book now at seq " << handler.last_applied() << "\n";
    } else{
      cout << "nothing missing\n";
    }
  }

 private:
  string missing() const{
    return missing_first_ == missing_last_ ? "seq " + to_string(missing_first_)
                                           : "seqs " + to_string(missing_first_) + "-" + to_string(missing_last_);
  }

  bool quiet_;
  uint64_t missing_first_ = 0;  /**< the gap being waited on */
  uint64_t missing_last_ = 0;
};

/** One stream: the exchange publishes, the link damages, the handler rebuilds; traced and followed as asked. */
class Pipeline{
 public:
  explicit Pipeline(const Stream& stream)
      : exchange_(stream.seed, stream.history, stream.orders), link_(stream.link),
        handler_(exchange_, stream.window, stream.recovery), journal_(stream.events, stream.show){}

  /** Prints what the handler does with each arrival from now on. */
  void trace(bool quiet){
    tracer_.emplace(quiet);
  }

  /** Publishes one event and hands whatever arrives to the handler. */
  void publish(){
    const Event event = exchange_.next();
    const uint64_t lost_before = link_.lost();
    const vector<Event> arrived = link_.send(event);
    journal_.sent(event, arrived, link_.lost() > lost_before);
    for (const Event& e : arrived){
      arrive(e);
    }
  }

  /** Delivers any event the link still holds, then, if asked, the exchange's heartbeat. */
  void finish(bool heartbeat){
    for (const Event& e : link_.flush()){
      arrive(e);
    }
    if (heartbeat){
      const HandlerStats before = handler_.stats();
      const uint64_t applied_before = handler_.last_applied();
      handler_.on_heartbeat(exchange_.last_seq());
      if (tracer_){
        tracer_->heartbeat(handler_, exchange_.last_seq(), before, applied_before);
      }
      journal_.reached(handler_, 0, before, applied_before, 0);
    }
  }

  const Exchange& exchange() const{
    return exchange_;
  }
  const LossyLink& link() const{
    return link_;
  }
  const FeedHandler& handler() const{
    return handler_;
  }
  const Journal& journal() const{
    return journal_;
  }

 private:
  void arrive(const Event& event){
    const HandlerStats before = handler_.stats();
    const uint64_t applied_before = handler_.last_applied();
    const size_t buffered_before = handler_.buffered();
    handler_.on_event(event);
    if (tracer_){
      tracer_->arrival(handler_, event, before, applied_before);
    }
    journal_.reached(handler_, event.seq, before, applied_before, buffered_before);
  }

  Exchange exchange_;
  LossyLink link_;
  FeedHandler handler_;
  Journal journal_;
  optional<Tracer> tracer_;
};

/**
 * Shows the followed events, then compares the handler's book with the
 * exchange's and prints the verdict.
 *
 * @return whether the books match and nothing was rejected
 */
bool check(const Pipeline& pipeline){
  pipeline.journal().print();
  const FeedHandler& handler = pipeline.handler();
  const Exchange& exchange = pipeline.exchange();
  const bool same = handler.book() == exchange.book() && handler.last_applied() == exchange.last_seq() &&
                    handler.stats().rejected == 0;
  const size_t orders = handler.book().orders().size();
  cout << "  result: book at seq " << grouped(handler.last_applied()) << ", " << orders
       << (orders == 1 ? " order" : " orders") << ", best bid " << describe(handler.book().best_bid())
       << ", best ask " << describe(handler.book().best_ask()) << "\n";
  if (same){
    cout << "  fingerprint " << hex(handler.book().fingerprint()) << " on both sides: matches the exchange's own book\n";
  } else{
    cout << "  DIFFERS: handler " << hex(handler.book().fingerprint()) << ", exchange "
         << hex(exchange.book().fingerprint()) << " (seq " << handler.last_applied() << " vs " << exchange.last_seq()
         << ")\n";
  }
  return same;
}

/** Runs a traced stream: every event published, then the link flushed and, if asked, a heartbeat. */
bool traced(const Stream& s, bool heartbeat){
  Pipeline pipeline(s);
  pipeline.trace(s.quiet);
  for (uint64_t sent = 0; sent < s.events; ++sent){
    pipeline.publish();
  }
  pipeline.finish(heartbeat);
  return check(pipeline);
}

// ---- the scenarios: each reads its settings first, then runs ----

struct Burst{
  Stream stream;
  pair<uint64_t, uint64_t> gap;
};

Burst read_burst(Settings& in){
  Burst b{read_start(in, 22), {}};
  b.gap = in.range("gap", "Which seqs should the link lose? A range like 11-15", {11, 15},
                   [&](const pair<uint64_t, uint64_t>& g) -> optional<string>{
                     if (g.second >= b.stream.events){
                       return "burst: --gap must end before the last event, so later events reveal it; for loss at "
                              "the very end, run the tail scenario";
                     }
                     return nullopt;
                   });
  b.stream.link.drop_ranges = {b.gap};
  read_handler(in, b.stream, 1'000, false);
  read_show(in, b.stream, 0);
  return b;
}

bool burst(const Burst& b){
  cout << "== burst: seqs " << b.gap.first << "-" << b.gap.second << " are lost on the link ("
       << grouped(b.stream.events) << " events, " << b.stream.summary() << ") ==\n";
  return traced(b.stream, false);
}

Stream read_reorder(Settings& in){
  Stream s = read_start(in, 20);
  s.link.reorder = in.probability("reorder", "Chance an event arrives one place late, e.g. 20% or 0.2?", 0.2);
  s.link.duplicate = in.probability("dup", "Chance an event arrives twice?", 0.15);
  read_handler(in, s, 1'000, false);
  read_show(in, s, 0);
  return s;
}

bool reorder(const Stream& s){
  cout << "== reorder: " << percent(s.link.reorder) << " of events arrive one place late and " << percent(s.link.duplicate)
       << " arrive twice; none is lost (" << grouped(s.events) << " events, " << s.summary() << ") ==\n";
  Pipeline pipeline(s);
  pipeline.trace(s.quiet);
  for (uint64_t sent = 0; sent < s.events; ++sent){
    pipeline.publish();
  }
  pipeline.finish(true);
  cout << "  (" << pipeline.handler().stats().retransmits << " retransmissions needed)\n";
  return check(pipeline);
}

struct Tail{
  Stream stream;
  uint64_t lost;
};

Tail read_tail(Settings& in){
  Tail t{read_start(in, 20), 0};
  t.lost = in.integer("lost", "How many of the last events should the link lose?", 3, 1, max_events,
                      [&](const uint64_t& lost) -> optional<string>{
                        if (lost > t.stream.events){
                          return "tail: --lost cannot be more than --events";
                        }
                        return nullopt;
                      });
  t.stream.link.drop_ranges = {{t.stream.events - t.lost + 1, t.stream.events}};
  read_handler(in, t.stream, 1'000, false);
  read_show(in, t.stream, 0);
  return t;
}

bool tail(const Tail& t){
  const Stream& s = t.stream;
  cout << "== tail: the last " << t.lost << " events, seqs " << s.events - t.lost + 1 << "-" << s.events
       << ", are lost before the stream goes quiet (" << s.summary() << ") ==\n";
  return traced(s, true);
}

struct Outage{
  Stream stream;
  pair<uint64_t, uint64_t> gap;
};

Outage read_outage(Settings& in){
  Outage o{read_start(in, 1'000), {}};
  o.gap = in.range("gap", "Which seqs should the link lose? A range like 201-600", {201, 600},
                   [&](const pair<uint64_t, uint64_t>& g) -> optional<string>{
                     if (g.second > o.stream.events){
                       return "outage: --gap must end at or before the last event (--events)";
                     }
                     return nullopt;
                   });
  o.stream.link.drop_ranges = {o.gap};
  read_handler(in, o.stream, 100, true);
  read_show(in, o.stream, 0);
  return o;
}

bool outage(const Outage& o){
  const Stream& s = o.stream;
  cout << "== outage: seqs " << o.gap.first << "-" << o.gap.second << " are lost; the exchange keeps only its last "
       << grouped(s.history) << " events (" << grouped(s.events) << " events, " << s.summary() << ") ==\n";
  return traced(s, true);
}

/** The settings of a stream damaged at random: random and watch. */
Stream read_damage(Settings& in, uint64_t events){
  Stream s = read_start(in, events);
  s.link.loss = in.probability("loss", "Chance an event is lost, e.g. 20% or 0.2?", 0.2);
  s.link.duplicate = in.probability("dup", "Chance an event arrives twice?", 0.01);
  s.link.reorder = in.probability("reorder", "Chance an event arrives one place late?", 0.02);
  return s;
}

Stream read_random(Settings& in){
  Stream s = read_damage(in, 1'000'000);
  read_handler(in, s, 10'000, false);
  read_show(in, s, 5);
  return s;
}

string damage(const Stream& s){
  return grouped(s.events) + " events, " + percent(s.link.loss) + " lost, " + percent(s.link.duplicate) +
         " duplicated, " + percent(s.link.reorder) + " reordered";
}

bool random_damage(const Stream& s){
  cout << "== random: " << damage(s) << " (" << s.summary() << ") ==\n";
  Pipeline pipeline(s);
  const auto start = chrono::steady_clock::now();
  for (uint64_t sent = 0; sent < s.events; ++sent){
    pipeline.publish();
  }
  pipeline.finish(true);
  const double seconds = chrono::duration<double>(chrono::steady_clock::now() - start).count();

  const HandlerStats& stats = pipeline.handler().stats();
  const uint64_t lost = pipeline.link().lost();
  cout << "  " << grouped(lost) << " events lost on the link (" << percent(static_cast<double>(lost) / s.events)
       << "); " << grouped(stats.gaps) << " gaps; " << grouped(stats.retransmits) << " retransmissions brought back "
       << grouped(stats.recovered) << " events, " << stats.snapshots << " snapshots; " << grouped(stats.duplicates)
       << " duplicates dropped\n";
  cout << "  " << grouped(s.events) << " events in " << fixed << setprecision(2) << seconds << " s: "
       << setprecision(1) << s.events / seconds / 1e6 << " million a second through exchange, link and handler\n"
       << defaultfloat << setprecision(6);
  return check(pipeline);
}

struct Watch{
  Stream stream;
  uint64_t rate;
};

Watch read_watch(Settings& in){
  Watch w{read_damage(in, 2'000'000), 0};
  w.rate = in.integer("rate", "How many events a second should the exchange publish?", 200'000, 10, 100'000'000);
  read_handler(in, w.stream, 10'000, false);
  read_show(in, w.stream, 5);
  return w;
}

/** One status line: how far the stream has got and whether the handler keeps up. */
string status(const Pipeline& p){
  const uint64_t behind = p.exchange().last_seq() - p.handler().last_applied();
  ostringstream line;
  line << "  published " << setw(9) << grouped(p.exchange().last_seq()) << "   lost " << setw(7)
       << grouped(p.link().lost()) << "   recovered " << setw(7) << grouped(p.handler().stats().recovered)
       << "   behind " << setw(2) << behind << "   best " << describe(p.handler().book().best_bid()) << " / "
       << describe(p.handler().book().best_ask());
  return line.str();
}

bool watch(const Watch& w){
  const Stream& s = w.stream;
  cout << "== watch: " << damage(s) << ", at " << grouped(w.rate) << " events a second (" << s.summary() << ") ==\n";
  Pipeline pipeline(s);
  const bool live = isatty(STDOUT_FILENO) != 0;
  const uint64_t per_tick = max<uint64_t>(1, w.rate / 10);
  auto next_tick = chrono::steady_clock::now();
  for (uint64_t sent = 0, ticks = 1; sent < s.events; ++ticks){
    const uint64_t batch = min(per_tick, s.events - sent);
    for (uint64_t i = 0; i < batch; ++i){
      pipeline.publish();
    }
    sent += batch;
    if (live){
      cout << "\r" << status(pipeline) << "\x1b[K" << flush;
    } else if (ticks % 10 == 0){
      cout << status(pipeline) << "\n";  // piped: one line a second
    }
    next_tick += chrono::milliseconds(100);
    this_thread::sleep_until(next_tick);
  }
  pipeline.finish(true);
  cout << (live ? "\r" : "") << status(pipeline) << (live ? "\x1b[K" : "") << "\n";
  return check(pipeline);
}

struct Lamport{
  uint64_t events;
  uint64_t seed;
  int64_t latency_ms;
  vector<uint64_t> skews_ms;
};

Lamport read_lamport(Settings& in){
  Lamport l{};
  l.events = in.integer("events", "How many events should the exchange publish? Half go to each instance", 200, 2,
                        10'000'000);
  l.seed = in.seed();
  l.latency_ms = static_cast<int64_t>(
      in.integer("latency-ms", "How many ms does the handoff message take?", 5, 1, 1'000'000));
  l.skews_ms = in.integers("skews-ms", "Which clock skews to try, in ms? A list like 0,5,10,20",
                           {0, 2, 4, 5, 6, 8, 10, 20}, 0, 1'000'000);
  return l;
}

bool lamport(const Lamport& l){
  cout << "== lamport: instance 0 hands the stream to instance 1; the handoff message takes " << l.latency_ms
       << " ms (" << grouped(l.events) << " events, seed " << l.seed << ") ==\n"
       << "  instance 0's clock runs ahead by    updates   misordered by physical time   by Lamport time\n";
  bool lamport_never_misorders = true;
  for (const uint64_t skew_ms : l.skews_ms){
    const LamportDemoResult result =
        run_lamport_demo(static_cast<int64_t>(skew_ms) * 1'000, l.latency_ms * 1'000, l.events, l.seed);
    cout << setw(34) << skew_ms << " ms" << setw(10) << result.updates << setw(31) << result.physical_violations
         << setw(18) << result.lamport_violations << "\n";
    lamport_never_misorders = lamport_never_misorders && result.lamport_violations == 0;
  }
  cout << "  Physical time misorders once the skew passes the " << l.latency_ms
       << " ms handoff; Lamport time never does.\n";
  return lamport_never_misorders;
}

struct Multi{
  uint64_t events_per_symbol;
  uint64_t seed;
  double loss;
  size_t window;
  Recovery recovery;
  bool quiet;
};

Multi read_multi(Settings& in){
  Multi m{};
  m.events_per_symbol = in.integer("events", "How many events should each instrument publish?", 300, 10, 10'000'000);
  m.seed = in.seed();
  m.loss = in.probability("loss", "Chance an event is lost, e.g. 10% or 0.10?", 0.10);
  m.window = in.integer("window", "Reorder window: arrivals a gap may wait before it counts as lost?", 3, 0, 1'000'000);
  m.recovery = in.choice("recovery", "Recovery: first (one hole at a time) or every (all holes at once)?",
                         "every", {"first", "every"}) == "first"
                   ? Recovery::first_hole
                   : Recovery::every_hole;
  m.quiet = in.flag("quiet") || m.events_per_symbol > trace_limit;
  if (in.flag("quiet")){
    in.note("--quiet");
  }
  return m;
}

bool multi_scenario(const Multi& m){
  const vector<string> symbols = {"RELIANCE", "TCS", "INFY"};
  cout << "== multi: " << symbols.size() << " instruments (RELIANCE, TCS, INFY), "
       << grouped(m.events_per_symbol) << " events each, " << percent(m.loss)
       << " loss, seed " << m.seed << " ==\n";

  // --- Step 1: Showcase targeted scenario transitions ---
  cout << "  -- Phase 1: Showcasing specific event behaviors and handling --\n";
  {
    MultiExchange ex;
    ex.add_instrument("RELIANCE", 101, 100);
    ex.add_instrument("TCS", 102, 100);
    MultiFeedHandler h(ex, 3);

    // 1. Interleaving without false gaps
    InstrumentEvent r1 = ex.next("RELIANCE");
    InstrumentEvent t1 = ex.next("TCS");
    InstrumentEvent r2 = ex.next("RELIANCE");

    cout << "  [Interleaving]      Publish RELIANCE seq " << r1.event.seq
         << ", TCS seq " << t1.event.seq << ", RELIANCE seq " << r2.event.seq << "\n";
    h.on_event(r1);
    cout << "                      -> RELIANCE seq 1 applied (last_applied=" << h.handler("RELIANCE").last_applied() << ")\n";
    h.on_event(t1);
    cout << "                      -> TCS seq 1 applied (last_applied=" << h.handler("TCS").last_applied() << ", RELIANCE unaffected)\n";
    h.on_event(r2);
    cout << "                      -> RELIANCE seq 2 applied (no false gap, gaps=" << h.handler("RELIANCE").stats().gaps << ")\n";

    // 2. Injected gap on one symbol while another proceeds smoothly
    InstrumentEvent r3 = ex.next("RELIANCE"); // lost
    InstrumentEvent r4 = ex.next("RELIANCE");
    InstrumentEvent t2 = ex.next("TCS");
    InstrumentEvent t3 = ex.next("TCS");

    cout << "  [Isolated Gap]      Lose RELIANCE seq 3, arrive seq 4; arrive TCS seq 2 and 3\n";
    h.on_event(t2);
    h.on_event(t3);
    h.on_event(r4); // early arrival for RELIANCE
    cout << "                      -> TCS seqs 2-3 applied normally (last_applied=" << h.handler("TCS").last_applied() << ")\n";
    cout << "                      -> RELIANCE seq 4 arrives early: buffered (buffered="
         << h.handler("RELIANCE").buffered() << ", gap detected=" << h.handler("RELIANCE").stats().gaps << ")\n";

    // Wait out window for RELIANCE and recover
    for (int i = 0; i < 3; ++i) {
      h.on_event(ex.next("RELIANCE"));
    }
    cout << "                      -> Window elapsed: RELIANCE retransmits seq 3, book caught up to seq "
         << h.handler("RELIANCE").last_applied() << " (retransmits="
         << h.handler("RELIANCE").stats().retransmits << ")\n";

    // 3. Unknown symbol safety check
    InstrumentEvent bogus{"WIPRO", Event{1, EventType::add, 999, Side::bid, 500, 10}};
    cout << "  [Unknown Symbol]    Arrive event for unregistered symbol 'WIPRO'\n";
    const bool accepted = h.on_event(bogus);
    cout << "                      -> Accepted: " << (accepted ? "yes" : "no")
         << " (safely rejected, unknown_symbols=" << h.unknown_symbols() << ")\n";
  }

  // --- Step 2: Full parallel stream through lossy link ---
  cout << "  -- Phase 2: Streaming " << grouped(m.events_per_symbol)
       << " interleaved events across all symbols through lossy links --\n";

  MultiExchange exchange;
  MultiLossyLink link;

  for (size_t i = 0; i < symbols.size(); ++i){
    const uint64_t sym_seed = m.seed + i * 1'000 + 1;
    exchange.add_instrument(symbols[i], sym_seed, 500);

    LinkConfig config;
    config.loss = m.loss;
    config.reorder = 0.05;
    config.duplicate = 0.02;
    config.seed = sym_seed ^ link_salt;
    link.add_instrument(symbols[i], config);
  }

  MultiFeedHandler handler(exchange, m.window, m.recovery);

  for (uint64_t step = 0; step < m.events_per_symbol; ++step){
    for (const string& sym : symbols){
      for (const auto& ev : link.send(exchange.next(sym))){
        handler.on_event(ev);
      }
    }
  }

  for (const auto& ev : link.flush()){
    handler.on_event(ev);
  }

  for (const string& sym : symbols){
    handler.on_heartbeat(sym, exchange.exchange(sym).last_seq());
  }

  bool all_matched = true;
  for (const string& sym : symbols){
    const FeedHandler& h = handler.handler(sym);
    const Exchange& e = exchange.exchange(sym);
    const bool matched = h.book() == e.book() && h.last_applied() == e.last_seq() && h.stats().rejected == 0;
    all_matched = all_matched && matched;

    const size_t orders = h.book().orders().size();
    cout << "  " << left << setw(10) << sym << right
         << ": book at seq " << grouped(h.last_applied())
         << ", " << orders << " orders, best bid " << describe(h.book().best_bid())
         << ", best ask " << describe(h.book().best_ask())
         << "\n             gaps=" << h.stats().gaps
         << ", recovered=" << h.stats().recovered
         << " (" << h.stats().retransmits << " retransmits), "
         << h.stats().duplicates << " dups dropped"
         << (matched ? " -> matches exchange book" : " -> DIFFERS") << "\n";
  }

  if (all_matched){
    cout << "  All " << symbols.size() << " instruments reconstructed correctly with zero cross-contamination.\n";
  }
  return all_matched;
}

const vector<pair<string, string>> scenarios = {
    {"watch", "a live stream with heavy loss"},
    {"burst", "a run of lost events, recovered by one retransmission"},
    {"reorder", "late and doubled events, absorbed with no recovery"},
    {"tail", "the last events lost; the heartbeat finds them"},
    {"outage", "a gap older than the exchange's history: a snapshot"},
    {"random", "any damage you like, at any scale, timed"},
    {"multi", "multiple instruments partitioned with independent order books"},
    {"lamport", "two handlers with skewed clocks hand the stream over"},
    {"all", "every scenario but watch, one after another, with their defaults"},
};

/** A scenario, read and ready to run. */
using Ready = function<bool()>;

/** Reads a scenario's settings, checking them all. @return the scenario, ready to run */
Ready prepare(const string& scenario, Settings& in){
  if (scenario == "watch") return [w = read_watch(in)]{ return watch(w); };
  if (scenario == "burst") return [b = read_burst(in)]{ return burst(b); };
  if (scenario == "reorder") return [r = read_reorder(in)]{ return reorder(r); };
  if (scenario == "tail") return [t = read_tail(in)]{ return tail(t); };
  if (scenario == "outage") return [o = read_outage(in)]{ return outage(o); };
  if (scenario == "random") return [r = read_random(in)]{ return random_damage(r); };
  if (scenario == "multi") return [m = read_multi(in)]{ return multi_scenario(m); };
  if (scenario == "lamport") return [l = read_lamport(in)]{ return lamport(l); };
  throw UsageError("unknown scenario \"" + scenario + "\"; choose watch, burst, reorder, tail, outage, random, multi, lamport or all");
}

/** Runs a prepared scenario, then prints the command that replays it. */
bool run_one(const string& scenario, const Ready& ready, const Settings& in){
  const bool ok = ready();
  cout << "  replay: feed_demo " << scenario << in.replay() << "\n\n";
  return ok;
}

/** Reads every scenario of `all` first, so a bad setting stops the run before anything is printed, then runs each. */
bool run_all(const Options& options){
  vector<pair<string, Settings>> settings;
  vector<Ready> ready;
  for (const string scenario : {"burst", "reorder", "tail", "outage", "random", "multi", "lamport"}){
    settings.emplace_back(scenario, Settings(options, false));
    ready.push_back(prepare(scenario, settings.back().second));
  }
  bool ok = true;
  for (size_t i = 0; i < ready.size(); ++i){
    ok = run_one(settings[i].first, ready[i], settings[i].second) && ok;
  }
  return ok;
}

/** Asks which scenario to run. @return its name */
string pick(const Options& options){
  cout << "\nScenarios:\n";
  for (size_t i = 0; i < scenarios.size(); ++i){
    cout << "  " << i + 1 << "  " << left << setw(9) << scenarios[i].first << " " << scenarios[i].second << "\n"
         << right;
  }
  Settings in(options, true);
  while (true){
    const string answer = in.ask("Which scenario? A number or a name", "random");
    if (answer.empty()){
      return "random";
    }
    for (size_t i = 0; i < scenarios.size(); ++i){
      if (answer == to_string(i + 1) || answer == scenarios[i].first){
        return scenarios[i].first;
      }
    }
    in.complain("\"" + answer + "\" is not one of the scenarios");
  }
}

/**
 * The interactive session: asks for a scenario (unless the command line
 * names one) and its settings, runs it, and offers another.
 *
 * @return the exit status
 */
int session(const Options& options){
  cout << "A feed handler rebuilds an exchange's order book from a stream of events that a lossy\n"
          "link damages, and checks the result against the exchange's own book. Answer each\n"
          "question, or press Enter for the value in [brackets]; q quits.\n";
  bool all_ok = true;
  try{
    while (true){
      const string scenario = options.positional().empty() ? pick(options) : options.positional()[0];
      if (scenario == "all"){
        all_ok = run_all(options) && all_ok;
      } else{
        Settings in(options, true);
        const Ready ready = prepare(scenario, in);
        cout << "\n";
        all_ok = run_one(scenario, ready, in) && all_ok;
      }
      if (!options.positional().empty() || !Settings(options, true).yes("Run another scenario?", true)){
        break;
      }
    }
  } catch (const Quit&){
  }
  return all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/** Runs the scenario the command line names, with its settings. @return the exit status */
int run(const Options& options){
  const string scenario = options.positional()[0];
  if (scenario == "all"){
    return run_all(options) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  Settings in(options, false);
  const Ready ready = prepare(scenario, in);
  return run_one(scenario, ready, in) ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv){
  try{
    const Options options(vector<string>(argv + 1, argv + argc), {"help", "quiet", "ask"},
                          {"seed", "events", "loss", "dup", "reorder", "gap", "lost", "history", "orders", "window",
                           "recovery", "rate", "latency-ms", "skews-ms", "show"});
    if (options.has("help")){
      cout << usage;
      return EXIT_SUCCESS;
    }
    options.expect_positional(1);
    if (options.positional().empty() || options.has("ask")){
      return session(options);
    }
    return run(options);
  } catch (const UsageError& error){
    cerr << "feed_demo: " << error.what() << "\nRun feed_demo --help to see the scenarios and options.\n";
    return 2;
  } catch (const invalid_argument& error){
    cerr << "feed_demo: " << error.what() << "\n";
    return 2;
  } catch (const exception& error){
    cerr << "feed_demo: internal error: " << error.what() << "\n";
    return 3;
  }
}
