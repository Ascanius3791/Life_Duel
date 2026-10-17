#pragma once

// Records every battle played during training, as training data for a model
// that predicts match results. Two files share a prefix:
//   PREFIX.armies.tsv   one army per line, "id<TAB>pieces" (pieces sorted, so
//                       equal armies get one id)
//   PREFIX.matches.csv  one battle per line: batch, red id, blue id, the
//                       red-side score, the end generation, and the red/blue
//                       cell counts at each of kTraceGenerations
// A batch is one call of playJobs (for example one search generation), so the
// candidates of a search generation can be compared with each other. Opening an
// existing log appends to it and keeps the army ids.

#include "arena.hpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace arena {

class MatchLog {
public:
    MatchLog() = default;
    MatchLog(const MatchLog&) = delete;
    MatchLog& operator=(const MatchLog&) = delete;
    ~MatchLog();

    bool open(const Arena& arena, const std::string& prefix);
    bool isOpen() const { return matches_ != nullptr; }
    void beginBatch() { ++batch_; }
    void flush();
    // Thread-safe.
    void record(const Army& red, const Army& blue, const MatchResult& r);
    long long recorded() const { return recorded_; }

private:
    int armyId(const Army& army);  // caller holds mutex_

    const Arena* arena_ = nullptr;
    std::FILE* armies_ = nullptr;
    std::FILE* matches_ = nullptr;
    std::unordered_map<std::string, int> ids_;
    long long batch_ = 0;
    long long recorded_ = 0;
    std::mutex mutex_;
};

}  // namespace arena
