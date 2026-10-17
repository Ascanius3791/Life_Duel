#include "match_log.hpp"

#include <fstream>

namespace arena {

MatchLog::~MatchLog() {
    if (armies_) std::fclose(armies_);
    if (matches_) std::fclose(matches_);
}

bool MatchLog::open(const Arena& arena, const std::string& prefix) {
    arena_ = &arena;
    const std::string armiesPath = prefix + ".armies.tsv", matchesPath = prefix + ".matches.csv";

    // Resuming: known armies keep their ids, and batches continue after the last one.
    {
        std::ifstream in(armiesPath);
        std::string line;
        while (std::getline(in, line)) {
            const auto tab = line.find('\t');
            if (tab != std::string::npos) ids_.emplace(line.substr(tab + 1), static_cast<int>(ids_.size()));
        }
    }
    bool fresh = true;
    {
        std::ifstream in(matchesPath);
        std::string line, last;
        while (std::getline(in, line))
            if (!line.empty()) last = line;
        if (!last.empty()) {
            fresh = false;
            if (last[0] != 'b') batch_ = std::atoll(last.c_str());
        }
    }

    armies_ = std::fopen(armiesPath.c_str(), "a");
    matches_ = std::fopen(matchesPath.c_str(), "a");
    if (!armies_ || !matches_) return false;
    if (fresh) {
        std::fprintf(matches_, "batch,red,blue,score,generation");
        for (int g : kTraceGenerations) std::fprintf(matches_, ",red%d,blue%d", g, g);
        std::fprintf(matches_, "\n");
    }
    return true;
}

int MatchLog::armyId(const Army& army) {
    std::string key = armyKey(*arena_, army);
    if (!key.empty()) key.pop_back();  // trailing ';'
    const auto [it, inserted] = ids_.emplace(key, static_cast<int>(ids_.size()));
    if (inserted) std::fprintf(armies_, "%d\t%s\n", it->second, key.c_str());
    return it->second;
}

void MatchLog::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (armies_) std::fflush(armies_);
    if (matches_) std::fflush(matches_);
}

void MatchLog::record(const Army& red, const Army& blue, const MatchResult& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int a = armyId(red), b = armyId(blue);
    std::fprintf(matches_, "%lld,%d,%d,%.4f,%d", batch_, a, b, r.score, r.generation);
    for (std::size_t k = 0; k < kTraceGenerations.size(); ++k) std::fprintf(matches_, ",%d,%d", r.redAt[k], r.blueAt[k]);
    std::fprintf(matches_, "\n");
    ++recorded_;
}

}  // namespace arena
