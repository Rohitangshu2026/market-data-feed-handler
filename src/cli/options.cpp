// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Rohitangshu Bose

#include "cli/options.hpp"

#include<charconv>
#include<cmath>
#include<cstdlib>
#include<sstream>

using namespace std;

namespace{

/** @return `text` split at commas */
vector<string> split(const string& text){
  vector<string> items;
  string item;
  istringstream stream(text);
  while (getline(stream, item, ',')){
    items.push_back(item);
  }
  if (!text.empty() && text.back() == ','){
    items.push_back("");  // a trailing comma leaves an empty item, which is an error
  }
  return items;
}

/** @return `text` as a finite double, or nullopt if any of it is not part of the number */
optional<double> parse_real(const string& text){
  if (text.empty() || isspace(static_cast<unsigned char>(text.front())) || text.find_first_of("xX") != string::npos){
    return nullopt;  // strtod would skip leading spaces and read hex
  }
  char* end = nullptr;
  const double value = strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !isfinite(value)){
    return nullopt;
  }
  return value;
}

string quoted(const string& text){
  return "\"" + text + "\"";
}

}  // namespace

optional<uint64_t> parse_whole_number(const string& text){
  string digits;
  for (const char c : text){
    if (c != '\'' && c != '_'){
      digits += c;
    }
  }
  if (digits.empty() || digits.front() == '-' || digits.front() == '+'){
    return nullopt;
  }
  if (digits.find_first_of(".eE") != string::npos){  // 1e6, 2.5e3
    const optional<double> value = parse_real(digits);
    if (!value || *value < 0 || *value != floor(*value) || *value >= 18446744073709551616.0){
      return nullopt;
    }
    return static_cast<uint64_t>(*value);
  }
  uint64_t value = 0;
  const auto [end, error] = from_chars(digits.data(), digits.data() + digits.size(), value);
  if (error != errc{} || end != digits.data() + digits.size()){
    return nullopt;
  }
  return value;
}

optional<double> parse_probability(const string& text){
  const bool percent = !text.empty() && text.back() == '%';
  optional<double> value = parse_real(percent ? text.substr(0, text.size() - 1) : text);
  if (!value){
    return nullopt;
  }
  if (percent){
    *value /= 100.0;
  }
  if (*value < 0.0 || *value > 1.0){
    return nullopt;
  }
  return value;
}

Options::Options(const vector<string>& arguments, const set<string>& flags, const set<string>& valued){
  for (size_t i = 0; i < arguments.size(); ++i){
    const string& word = arguments[i];
    if (word == "-h"){
      given_.emplace("help", nullopt);
      continue;
    }
    if (word.rfind("--", 0) != 0){
      positional_.push_back(word);
      continue;
    }
    const size_t equals = word.find('=');
    const string name = word.substr(2, equals == string::npos ? string::npos : equals - 2);
    if (name.empty()){
      throw UsageError(quoted(word) + " is not an option: options look like --name or --name=value");
    }
    if (!flags.contains(name) && !valued.contains(name)){
      throw UsageError("unknown option --" + name);
    }
    if (given_.contains(name)){
      throw UsageError("--" + name + " is given twice");
    }
    optional<string> value;
    if (equals != string::npos){
      value = word.substr(equals + 1);
      if (flags.contains(name)){
        throw UsageError("--" + name + " takes no value");
      }
    } else if (!flags.contains(name)){
      if (i + 1 >= arguments.size() || arguments[i + 1].rfind("--", 0) == 0){
        throw UsageError("--" + name + " needs a value");
      }
      value = arguments[++i];
    }
    given_.emplace(name, value);
  }
}

optional<string> Options::value(const string& name) const{
  const auto found = given_.find(name);
  if (found == given_.end()){
    return nullopt;
  }
  return found->second.value_or("");
}

uint64_t Options::integer(const string& name, uint64_t fallback, uint64_t min, uint64_t max) const{
  const optional<string> text = value(name);
  if (!text){
    return fallback;
  }
  const optional<uint64_t> value = parse_whole_number(*text);
  if (!value){
    throw UsageError("--" + name + ": " + quoted(*text) + " is not a whole number");
  }
  if (*value < min || *value > max){
    throw UsageError("--" + name + ": " + *text + " is out of range; use " + to_string(min) + " to " + to_string(max));
  }
  return *value;
}

double Options::probability(const string& name, double fallback) const{
  const optional<string> text = value(name);
  if (!text){
    return fallback;
  }
  const optional<double> value = parse_probability(*text);
  if (!value){
    throw UsageError("--" + name + ": " + quoted(*text) + " is not a chance from 0 to 1; write it as 0.2 or 20%");
  }
  return *value;
}

vector<uint64_t> Options::integers(const string& name, const vector<uint64_t>& fallback, uint64_t min, uint64_t max) const{
  const optional<string> text = value(name);
  if (!text){
    return fallback;
  }
  if (text->empty()){
    throw UsageError("--" + name + " needs a value");
  }
  vector<uint64_t> values;
  for (const string& item : split(*text)){
    const optional<uint64_t> value = parse_whole_number(item);
    if (!value){
      throw UsageError("--" + name + ": " + quoted(item) + " is not a whole number; give a list like 1,2,3");
    }
    if (*value < min || *value > max){
      throw UsageError("--" + name + ": " + item + " is out of range; use " + to_string(min) + " to " + to_string(max));
    }
    values.push_back(*value);
  }
  return values;
}

vector<double> Options::probabilities(const string& name, const vector<double>& fallback) const{
  const optional<string> text = value(name);
  if (!text){
    return fallback;
  }
  if (text->empty()){
    throw UsageError("--" + name + " needs a value");
  }
  vector<double> values;
  for (const string& item : split(*text)){
    const optional<double> value = parse_probability(item);
    if (!value){
      throw UsageError("--" + name + ": " + quoted(item) + " is not a chance from 0 to 1; give a list like 0,5%,0.2");
    }
    values.push_back(*value);
  }
  return values;
}

pair<uint64_t, uint64_t> Options::range(const string& name, pair<uint64_t, uint64_t> fallback) const{
  const optional<string> text = value(name);
  if (!text){
    return fallback;
  }
  const size_t dash = text->find('-');
  const optional<uint64_t> first = dash == string::npos ? nullopt : parse_whole_number(text->substr(0, dash));
  const optional<uint64_t> last = dash == string::npos ? nullopt : parse_whole_number(text->substr(dash + 1));
  if (!first || !last || *first == 0 || *first > *last){
    throw UsageError("--" + name + ": " + quoted(*text) + " is not a range of seqs like 101-150 (from 1 up, first <= last)");
  }
  return {*first, *last};
}

string Options::choice(const string& name, const string& fallback, const vector<string>& allowed) const{
  const optional<string> text = value(name);
  if (!text){
    return fallback;
  }
  for (const string& word : allowed){
    if (*text == word){
      return word;
    }
  }
  string list;
  for (const string& word : allowed){
    list += (list.empty() ? "" : ", ") + word;
  }
  throw UsageError("--" + name + ": " + quoted(*text) + " is not one of: " + list);
}

void Options::expect_positional(size_t max_positional) const{
  if (positional_.size() > max_positional){
    throw UsageError("unexpected word " + quoted(positional_[max_positional]));
  }
}
