#include "engine/evaluator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gomoku {
namespace {

constexpr std::array<std::array<int, 2>, 4> kDirections{{
    {{1, 0}},
    {{0, 1}},
    {{1, 1}},
    {{1, -1}},
}};

int runValue(int length, int openEnds, Stone stone, RuleSet rules) {
    if (length >= 5) {
        if (rules == RuleSet::Renju && stone == Stone::Black && length > 5) {
            return 0;
        }
        return 2'000'000;
    }
    if (openEnds == 0) {
        return 0;
    }

    switch (length) {
        case 4:
            return openEnds == 2 ? 180'000 : 32'000;
        case 3:
            return openEnds == 2 ? 9'000 : 1'200;
        case 2:
            return openEnds == 2 ? 600 : 90;
        case 1:
            return openEnds == 2 ? 24 : 5;
        default:
            return 0;
    }
}

int windowValue(int stones, int openEnds) {
    switch (stones) {
        case 4:
            return openEnds == 2 ? 28'000 : 15'000;
        case 3:
            return openEnds == 2 ? 2'800 : 1'300;
        case 2:
            return openEnds == 2 ? 240 : 110;
        case 1:
            return 12;
        default:
            return 0;
    }
}

struct PositionScores {
    int black = 0;
    int white = 0;

    int& forStone(Stone stone) noexcept {
        return stone == Stone::Black ? black : white;
    }
};

PositionScores scorePosition(const Board& board, RuleSet rules) {
    PositionScores scores;
    const Board::Cells& cells = board.cells();

    for (int y = 0; y < kBoardSize; ++y) {
        for (int x = 0; x < kBoardSize; ++x) {
            const Stone stone = cells[Move{x, y}.index()];
            if (stone == Stone::Empty) {
                continue;
            }

            int& score = scores.forStone(stone);
            const int distanceToCentre = std::abs(x - kBoardSize / 2) +
                                         std::abs(y - kBoardSize / 2);
            score += std::max(0, 14 - distanceToCentre);

            for (const auto& direction : kDirections) {
                const int dx = direction[0];
                const int dy = direction[1];
                const int previousX = x - dx;
                const int previousY = y - dy;
                if (Board::inBounds(previousX, previousY) &&
                    cells[Move{previousX, previousY}.index()] == stone) {
                    continue;
                }

                int length = 0;
                int cursorX = x;
                int cursorY = y;
                while (Board::inBounds(cursorX, cursorY) &&
                       cells[Move{cursorX, cursorY}.index()] == stone) {
                    ++length;
                    cursorX += dx;
                    cursorY += dy;
                }

                int openEnds = 0;
                if (Board::inBounds(previousX, previousY) &&
                    cells[Move{previousX, previousY}.index()] == Stone::Empty) {
                    ++openEnds;
                }
                if (Board::inBounds(cursorX, cursorY) &&
                    cells[Move{cursorX, cursorY}.index()] == Stone::Empty) {
                    ++openEnds;
                }
                score += runValue(length, openEnds, stone, rules);
            }
        }
    }

    // A five-cell window can contribute to at most one side, so score both
    // colors in the same traversal instead of rescanning every window.
    for (const auto& direction : kDirections) {
        const int dx = direction[0];
        const int dy = direction[1];
        for (int y = 0; y < kBoardSize; ++y) {
            for (int x = 0; x < kBoardSize; ++x) {
                const int endX = x + 4 * dx;
                const int endY = y + 4 * dy;
                if (!Board::inBounds(endX, endY)) {
                    continue;
                }

                int black = 0;
                int white = 0;
                for (int offset = 0; offset < 5; ++offset) {
                    const Stone cell =
                        cells[Move{x + offset * dx, y + offset * dy}.index()];
                    if (cell == Stone::Black) {
                        ++black;
                    } else if (cell == Stone::White) {
                        ++white;
                    }
                }
                if ((black == 0) == (white == 0)) {
                    continue;
                }

                int openEnds = 0;
                const int beforeX = x - dx;
                const int beforeY = y - dy;
                const int afterX = endX + dx;
                const int afterY = endY + dy;
                if (Board::inBounds(beforeX, beforeY) &&
                    cells[Move{beforeX, beforeY}.index()] == Stone::Empty) {
                    ++openEnds;
                }
                if (Board::inBounds(afterX, afterY) &&
                    cells[Move{afterX, afterY}.index()] == Stone::Empty) {
                    ++openEnds;
                }
                if (white == 0 && black < 5) {
                    scores.black += windowValue(black, openEnds);
                } else if (black == 0 && white < 5) {
                    scores.white += windowValue(white, openEnds);
                }
            }
        }
    }

    return scores;
}

[[nodiscard]] bool isWinningPlacement(
    const Board& board, Move move, Stone stone, RuleSet rules) noexcept {
    const Board::Cells& cells = board.cells();
    for (const auto& direction : kDirections) {
        const int dx = direction[0];
        const int dy = direction[1];
        int length = 1;
        for (int step = 1;; ++step) {
            const int x = move.x - step * dx;
            const int y = move.y - step * dy;
            if (!Board::inBounds(x, y) || cells[Move{x, y}.index()] != stone) {
                break;
            }
            ++length;
        }
        for (int step = 1;; ++step) {
            const int x = move.x + step * dx;
            const int y = move.y + step * dy;
            if (!Board::inBounds(x, y) || cells[Move{x, y}.index()] != stone) {
                break;
            }
            ++length;
        }
        if (rules == RuleSet::Renju && stone == Stone::Black) {
            if (length == 5) {
                return true;
            }
        } else if (length >= 5) {
            return true;
        }
    }
    return false;
}

using MoveMask = std::array<bool, kBoardArea>;

MoveMask neighbourMask(const Board& board, int radius) {
    MoveMask mask{};
    board.forEachOccupied([&](Move occupied, Stone) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int x = occupied.x + dx;
                const int y = occupied.y + dy;
                if ((dx != 0 || dy != 0) && Board::inBounds(x, y)) {
                    mask[Move{x, y}.index()] = true;
                }
            }
        }
    });
    return mask;
}

MoveMask winningNeighbourMask(const Board& board, Stone stone) {
    MoveMask mask{};
    board.forEachOccupied([&](Move occupied, Stone cell) {
        if (cell != stone) {
            return;
        }
        for (const auto& direction : kDirections) {
            for (int sign : {-1, 1}) {
                const int x = occupied.x + sign * direction[0];
                const int y = occupied.y + sign * direction[1];
                if (Board::inBounds(x, y)) {
                    mask[Move{x, y}.index()] = true;
                }
            }
        }
    });
    return mask;
}

struct MovePotentials {
    int black = 0;
    int white = 0;

    [[nodiscard]] int forStone(Stone stone) const noexcept {
        return stone == Stone::Black ? black : white;
    }

    int& forStone(Stone stone) noexcept {
        return stone == Stone::Black ? black : white;
    }
};

void addRunPotential(int& score, int length, int openEnds) {
    if (length >= 5) {
        score += 4'000'000;
    } else if (length == 4) {
        score += openEnds == 2 ? 400'000 : 100'000;
    } else if (length == 3) {
        score += openEnds == 2 ? 30'000 : 5'000;
    } else if (length == 2) {
        score += openEnds == 2 ? 2'000 : 350;
    }
}

MovePotentials movePotentials(const Board& board, Move move) {
    MovePotentials scores;
    const Board::Cells& cells = board.cells();
    for (const auto& direction : kDirections) {
        const int dx = direction[0];
        const int dy = direction[1];
        std::array<int, 3> left{};
        std::array<int, 3> right{};

        for (int sign : {-1, 1}) {
            std::array<int, 3>& counts = sign < 0 ? left : right;
            const int firstX = move.x + sign * dx;
            const int firstY = move.y + sign * dy;
            if (!Board::inBounds(firstX, firstY)) {
                continue;
            }
            const Stone adjacent = cells[Move{firstX, firstY}.index()];
            if (adjacent == Stone::Empty) {
                continue;
            }
            for (int step = 1; step < 5; ++step) {
                const int x = move.x + sign * step * dx;
                const int y = move.y + sign * step * dy;
                if (!Board::inBounds(x, y) || cells[Move{x, y}.index()] != adjacent) {
                    break;
                }
                ++counts[static_cast<std::size_t>(adjacent)];
            }
        }

        for (Stone stone : {Stone::Black, Stone::White}) {
            const auto index = static_cast<std::size_t>(stone);
            const int leftCount = left[index];
            const int rightCount = right[index];
            int openEnds = 0;
            const int leftX = move.x - (leftCount + 1) * dx;
            const int leftY = move.y - (leftCount + 1) * dy;
            const int rightX = move.x + (rightCount + 1) * dx;
            const int rightY = move.y + (rightCount + 1) * dy;
            if (Board::inBounds(leftX, leftY) &&
                cells[Move{leftX, leftY}.index()] == Stone::Empty) {
                ++openEnds;
            }
            if (Board::inBounds(rightX, rightY) &&
                cells[Move{rightX, rightY}.index()] == Stone::Empty) {
                ++openEnds;
            }
            addRunPotential(
                scores.forStone(stone), leftCount + 1 + rightCount, openEnds);
        }

        for (int start = -4; start <= 0; ++start) {
            int black = 0;
            int white = 0;
            bool inside = true;
            for (int offset = 0; offset < 5; ++offset) {
                const int relative = start + offset;
                const int x = move.x + relative * dx;
                const int y = move.y + relative * dy;
                if (!Board::inBounds(x, y)) {
                    inside = false;
                    break;
                }
                if (relative == 0) {
                    continue;
                }
                const Stone cell = cells[Move{x, y}.index()];
                black += cell == Stone::Black ? 1 : 0;
                white += cell == Stone::White ? 1 : 0;
            }
            if (!inside) {
                continue;
            }
            if (white == 0) {
                scores.black += windowValue(black + 1, 1);
            }
            if (black == 0) {
                scores.white += windowValue(white + 1, 1);
            }
        }
    }

    const int centreDistance = std::abs(move.x - kBoardSize / 2) +
                               std::abs(move.y - kBoardSize / 2);
    const int centreScore = std::max(0, 28 - centreDistance * 2);
    scores.black += centreScore;
    scores.white += centreScore;
    return scores;
}

}  // namespace

int Evaluator::evaluate(const Board& board, Stone perspective, RuleSet rules) {
    const PositionScores scores = scorePosition(board, rules);
    int score = perspective == Stone::Black
                    ? scores.black - scores.white
                    : scores.white - scores.black;
    score += board.sideToMove() == perspective ? 24 : -24;
    return std::clamp(score, -2'500'000, 2'500'000);
}

int Evaluator::movePotential(const Board& board, Move move, Stone stone) {
    if (!Board::inBounds(move) ||
        board.cells()[move.index()] != Stone::Empty ||
        stone == Stone::Empty) {
        return std::numeric_limits<int>::min() / 4;
    }

    return movePotentials(board, move).forStone(stone);
}

std::vector<Move> Evaluator::winningMoves(
    const Board& board, Stone stone, RuleSet rules) {
    std::vector<Move> moves;
    if (stone == Stone::Empty) {
        return moves;
    }
    if (board.count(stone) < 4) {
        return moves;
    }
    moves.reserve(4);
    const MoveMask nearby = winningNeighbourMask(board, stone);
    board.forEachEmpty([&](Move move) {
        if (nearby[move.index()] && isWinningPlacement(board, move, stone, rules)) {
            moves.push_back(move);
        }
    });
    return moves;
}

std::vector<RankedMove> Evaluator::candidateMoves(
    const Board& board,
    Stone stone,
    RuleSet rules,
    std::optional<Move> preferred,
    std::size_t limit,
    const std::vector<Move>* knownOwnWins,
    const std::vector<Move>* knownOpponentWins) {
    std::vector<RankedMove> ranked;
    if (stone == Stone::Empty || board.full()) {
        return ranked;
    }

    if (board.occupiedCount() == 0) {
        const Move centre{kBoardSize / 2, kBoardSize / 2};
        return {{centre, 1'000, false, false}};
    }

    const std::vector<Move> computedOwnWins = knownOwnWins == nullptr
                                                  ? winningMoves(board, stone, rules)
                                                  : std::vector<Move>{};
    const std::vector<Move> computedOpponentWins = knownOpponentWins == nullptr
                                                       ? winningMoves(board, opponent(stone), rules)
                                                       : std::vector<Move>{};
    const std::vector<Move>& ownWins = knownOwnWins == nullptr
                                           ? computedOwnWins
                                           : *knownOwnWins;
    const std::vector<Move>& opponentWins = knownOpponentWins == nullptr
                                                ? computedOpponentWins
                                                : *knownOpponentWins;
    MoveMask ownWinMask{};
    MoveMask opponentWinMask{};
    for (Move move : ownWins) {
        if (Board::inBounds(move)) {
            ownWinMask[move.index()] = true;
        }
    }
    for (Move move : opponentWins) {
        if (Board::inBounds(move)) {
            opponentWinMask[move.index()] = true;
        }
    }
    // In the opening, disconnected radius-two moves create severe horizon
    // artifacts. Keep the first replies connected, then widen in the middlegame.
    const int neighbourhoodRadius = board.occupiedCount() < 4 ? 1 : 2;
    const MoveMask nearbyMask = neighbourMask(board, neighbourhoodRadius);
    ranked.reserve(64);
    board.forEachEmpty([&](Move move) {
        const bool preferredMove = preferred.has_value() && preferred.value() == move;
        const bool blocksWin = opponentWinMask[move.index()];
        const bool nearby = nearbyMask[move.index()];
        const bool wins = ownWinMask[move.index()];
        if (!wins && !blocksWin && !nearby && !preferredMove) {
            return;
        }
        if (rules == RuleSet::Renju && stone == Stone::Black &&
            !Rules::isLegalMove(board, move, stone, rules)) {
            return;
        }

        const MovePotentials potentials = movePotentials(board, move);
        int ordering = potentials.forStone(stone) * 2;
        const Stone other = opponent(stone);
        const bool opponentCouldPlay = rules != RuleSet::Renju || other != Stone::Black ||
                                       Rules::isLegalMove(board, move, other, rules);
        if (opponentCouldPlay) {
            ordering += potentials.forStone(other);
        }
        if (blocksWin) {
            ordering += 70'000'000;
        }
        if (wins) {
            ordering += 140'000'000;
        }
        if (preferredMove) {
            ordering += 220'000'000;
        }
        ranked.push_back({move, ordering, wins, blocksWin});
    });

    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedMove& lhs, const RankedMove& rhs) {
        if (lhs.orderingScore != rhs.orderingScore) {
            return lhs.orderingScore > rhs.orderingScore;
        }
        return lhs.move.index() < rhs.move.index();
    });
    if (limit > 0 && ranked.size() > limit) {
        ranked.resize(limit);
    }
    return ranked;
}

}  // namespace gomoku
