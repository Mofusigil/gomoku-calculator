#pragma once

#include "engine/evaluator.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gomoku {

struct SearchLimits {
    int timeMs = 1'500;
    int maxDepth = 5;
    int maxMatePlies = 9;
    std::size_t maxCandidates = 32;
    bool infinite = false;
    std::function<bool()> cancelRequested;
};

struct SearchStats {
    int completedDepth = 0;
    std::uint64_t nodes = 0;
    std::uint64_t forcingNodes = 0;
    std::uint64_t betaCutoffs = 0;
    std::uint64_t transpositionHits = 0;
    std::uint64_t nodesPerSecond = 0;
    int elapsedMs = 0;
    bool timedOut = false;
    bool cancelled = false;
};

struct CandidateResult {
    Move move;
    int score = 0;
    std::vector<Move> principalVariation;
};

struct AnalysisResult {
    std::string kind = "evaluation";
    bool proven = false;
    bool terminal = false;
    Stone winner = Stone::Empty;
    std::optional<int> matePlies;
    std::optional<Move> bestMove;
    std::vector<Move> principalVariation;
    std::vector<CandidateResult> candidates;
    int score = 0;
    double blackWinRate = 0.5;
    double whiteWinRate = 0.5;
    double drawRate = 0.0;
    std::string proof;
    SearchStats stats;
};

using AnalysisObserver = std::function<void(const AnalysisResult&)>;

class Analyzer {
public:
    [[nodiscard]] AnalysisResult analyze(
        Board board,
        RuleSet rules,
        const SearchLimits& limits = {},
        AnalysisObserver observer = {});
};

}  // namespace gomoku
