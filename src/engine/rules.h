#pragma once

#include "engine/board.h"

namespace gomoku {

class Rules {
public:
    // The target must be empty. Invalid or occupied targets are reported explicitly.
    [[nodiscard]] static ForbiddenReason forbiddenReason(
        const Board& board, Move move, Stone stone, RuleSet rules);
    [[nodiscard]] static ForbiddenReason forbiddenReason(
        const Board& board, Move move, RuleSet rules = RuleSet::Renju);

    [[nodiscard]] static bool isLegalMove(
        const Board& board, Move move, Stone stone, RuleSet rules);
    [[nodiscard]] static bool isLegalMove(
        const Board& board, Move move, RuleSet rules = RuleSet::Renju);

    // Tests an empty target without modifying board.
    [[nodiscard]] static bool isWinningPlacement(
        const Board& board, Move move, Stone stone, RuleSet rules);

    // Tests a stone already present on board, normally the last move made.
    [[nodiscard]] static bool hasWinningLineAt(
        const Board& board, Move move, Stone stone, RuleSet rules);

    // Compatibility aliases for callers that use move-oriented terminology.
    [[nodiscard]] static bool isWinningMove(
        const Board& board, Move move, Stone stone, RuleSet rules) {
        return isWinningPlacement(board, move, stone, rules);
    }
    [[nodiscard]] static bool isWinAfterMove(
        const Board& board, Move move, RuleSet rules) {
        return hasWinningLineAt(board, move, board.at(move), rules);
    }

    [[nodiscard]] static WinnerInfo winners(const Board& board, RuleSet rules);
    [[nodiscard]] static bool hasWinner(
        const Board& board, Stone stone, RuleSet rules);
    // Returns Empty when there is no winner or an arbitrary loaded position has both.
    [[nodiscard]] static Stone winner(const Board& board, RuleSet rules) {
        return winners(board, rules).uniqueWinner();
    }
};

}  // namespace gomoku
