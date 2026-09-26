#pragma once

// Reader for the fixture records written by ml/make_pipeline_fixtures.py:
// blocks of `key value` lines separated by `---`; bit arrays are 0/1 strings
// ("-" when empty), "none" marks an absent value. Plus a collector so a
// differential test can compare hundreds of records and report every mismatch
// (record name + field) instead of stopping at the first.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "harness.hpp"

namespace sigtest {

using Record = std::map<std::string, std::string>;

inline std::vector<Record> read_records(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open fixture: " + path);
    std::vector<Record> records;
    Record current;
    std::string line;
    while (std::getline(f, line)) {
        if (line == "---") {
            records.push_back(std::move(current));
            current = {};
            continue;
        }
        const size_t space = line.find(' ');
        current[line.substr(0, space)] = space == std::string::npos ? "" : line.substr(space + 1);
    }
    return records;
}

inline const std::string& field(const Record& r, const std::string& key) {
    const auto it = r.find(key);
    if (it == r.end()) throw std::runtime_error("fixture record has no field '" + key + "'");
    return it->second;
}
inline bool has(const Record& r, const std::string& key) { return r.count(key) != 0; }
inline bool is_none(const std::string& v) { return v == "none"; }

inline std::vector<uint8_t> bits_of(const std::string& v) {
    std::vector<uint8_t> bits;
    if (v == "-") return bits;
    for (char c : v) {
        if (c != '0' && c != '1') throw std::runtime_error("bad bit string: " + v);
        bits.push_back(static_cast<uint8_t>(c - '0'));
    }
    return bits;
}
inline std::string bits_str(const std::vector<uint8_t>& bits) {
    if (bits.empty()) return "-";
    std::string s;
    for (uint8_t b : bits) s += b ? '1' : '0';
    return s;
}
inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    if (s == "-" || s.empty()) return out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) out.push_back(item);
    return out;
}
inline std::map<std::string, std::string> params_of(const std::string& v) {
    std::map<std::string, std::string> m;
    for (const auto& kv : split(v, ',')) {
        const size_t eq = kv.find('=');
        m[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return m;
}
inline double to_double(const std::string& v) { return std::strtod(v.c_str(), nullptr); }
inline long long to_int(const std::string& v) { return std::strtoll(v.c_str(), nullptr, 10); }

// Collects mismatches across many records; finish() fails the test with them.
class Mismatches {
public:
    void expect(bool ok, const std::string& record, const std::string& what) {
        ++checks_;
        if (!ok) list_.push_back(record + ": " + what);
    }
    template <class A, class B>
    void equal(const A& actual, const B& expected, const std::string& record, const std::string& what) {
        std::ostringstream os;
        os << what << " (got " << show(actual) << ", expected " << show(expected) << ")";
        expect(actual == expected, record, os.str());
    }
    void finish(size_t records) const {
        if (list_.empty()) return;
        std::string msg = std::to_string(list_.size()) + " of " + std::to_string(checks_) + " checks failed over " +
                          std::to_string(records) + " records:";
        for (size_t i = 0; i < list_.size() && i < 8; ++i) msg += "\n           " + list_[i].substr(0, 240);
        throw Failure{msg};
    }
    size_t checks() const { return checks_; }

private:
    template <class T>
    static std::string show(const T& v) {
        std::ostringstream os;
        os << v;
        std::string s = os.str();
        return s.size() > 80 ? s.substr(0, 77) + "..." : s;
    }
    static std::string show(const std::vector<uint8_t>& v) { auto s = bits_str(v); return s.size() > 80 ? s.substr(0, 77) + "..." : s; }
    static std::string show(const std::vector<size_t>& v) { std::string s; for (auto x : v) s += std::to_string(x) + ","; return s; }
    size_t checks_ = 0;
    std::vector<std::string> list_;
};

}  // namespace sigtest
