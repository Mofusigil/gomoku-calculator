#pragma once

#include "engine/rules.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace gomoku {

struct RankedMove {
    Move move;
    int orderingScore = 0;
    bool winsImmediately = false;
    bool blocksImmediateWin = false;
};

class Evaluator {
public:
    [[nodiscard]] static int evaluate(
        const Board& board, Stone perspective, RuleSet rules);

    [[nodiscard]] static int movePotential(
        const Board& board, Move move, Stone stone);

    [[nodiscard]] static std::vector<Move> winningMoves(
        const Board& board, Stone stone, RuleSet rules);

    [[nodiscard]] static std::vector<RankedMove> candidateMoves(
        const Board& board,
        Stone stone,
        RuleSet rules,
        std::optional<Move> preferred = std::nullopt,
        std::size_t limit = 32,
        const std::vector<Move>* knownOwnWins = nullptr,
        const std::vector<Move>* knownOpponentWins = nullptr);
};

}  // namespace gomoku
