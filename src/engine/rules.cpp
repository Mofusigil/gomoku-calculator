#include "engine/rules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <unordered_map>

namespace gomoku {
namespace {

constexpr std::array<Move, 4> kDirections{{
    {1, 0},
    {0, 1},
    {1, 1},
    {1, -1},
}};

[[nodiscard]] constexpr bool isPlayableStone(Stone stone) noexcept {
    return stone == Stone::Black || stone == Stone::White;
}

[[nodiscard]] Stone cellAt(const Board::Cells& cells, int x, int y) noexcept {
    if (!Board::inBounds(x, y)) {
        return Stone::Empty;
    }
    return cells[y * kBoardSize + x];
}

[[nodiscard]] int lineLength(
    const Board::Cells& cells, Move origin, Stone stone, Move direction) noexcept {
    int length = 1;
    for (int step = 1;; ++step) {
        const int x = origin.x + direction.x * step;
        const int y = origin.y + direction.y * step;
        if (!Board::inBounds(x, y) || cellAt(cells, x, y) != stone) {
            break;
        }
        ++length;
    }
    for (int step = 1;; ++step) {
        const int x = origin.x - direction.x * step;
        const int y = origin.y - direction.y * step;
        if (!Board::inBounds(x, y) || cellAt(cells, x, y) != stone) {
            break;
        }
        ++length;
    }
    return length;
}

[[nodiscard]] bool hasOverlineAt(const Board::Cells& cells, Move move) noexcept {
    return std::any_of(kDirections.begin(), kDirections.end(), [&](Move direction) {
        return lineLength(cells, move, Stone::Black, direction) > 5;
    });
}

[[nodiscard]] bool hasExactFiveAt(const Board::Cells& cells, Move move) noexcept {
    return std::any_of(kDirections.begin(), kDirections.end(), [&](Move direction) {
        return lineLength(cells, move, Stone::Black, direction) == 5;
    });
}

[[nodiscard]] bool hasWinningLineAt(
    const Board::Cells& cells, Move move, Stone stone, RuleSet rules) noexcept {
    if (!Board::inBounds(move) || !isPlayableStone(stone)
        || cells[move.index()] != stone) {
        return false;
    }

    if (rules == RuleSet::Renju && stone == Stone::Black) {
        // RIF 9.1/9.2: attaining an exact five wins even when the same move
        // also creates an overline in another direction.
        return hasExactFiveAt(cells, move);
    }

    return std::any_of(kDirections.begin(), kDirections.end(), [&](Move direction) {
        return lineLength(cells, move, stone, direction) >= 5;
    });
}

[[nodiscard]] constexpr std::uint64_t splitMix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr std::uint64_t analysisPieceKey(int index, Stone stone) noexcept {
    return splitMix64(0x3c6ef372fe94f82bULL
                      ^ (static_cast<std::uint64_t>(index) * 4ULL
                         + static_cast<std::uint64_t>(stone)));
}

struct AnalysisContext {
    explicit AnalysisContext(const Board& board) : cells(board.cells()) {
        for (int index = 0; index < kBoardArea; ++index) {
            if (cells[index] != Stone::Empty) {
                hash ^= analysisPieceKey(index, cells[index]);
            }
        }
    }

    void placeBlack(Move move) {
        cells[move.index()] = Stone::Black;
        hash ^= analysisPieceKey(move.index(), Stone::Black);
    }

    void removeBlack(Move move) {
        cells[move.index()] = Stone::Empty;
        hash ^= analysisPieceKey(move.index(), Stone::Black);
    }

    [[nodiscard]] std::uint64_t memoKey(Move move) const noexcept {
        return hash ^ splitMix64(0xa54ff53a5f1d36f1ULL
                                 + static_cast<std::uint64_t>(move.index()));
    }

    Board::Cells cells;
    std::uint64_t hash = 0;
    std::unordered_map<std::uint64_t, ForbiddenReason> memo;
};

using PatternKey = std::array<int, 4>;
using ThreeKey = std::array<int, 3>;

[[nodiscard]] bool isLegalWinningPoint(
    AnalysisContext& context, Move move, Move direction) {
    if (!Board::inBounds(move) || context.cells[move.index()] != Stone::Empty) {
        return false;
    }
    context.placeBlack(move);
    const bool wins = lineLength(context.cells, move, Stone::Black, direction) == 5;
    context.removeBlack(move);
    return wins;
}

[[nodiscard]] int countFours(AnalysisContext& context, Move placed) {
    std::set<PatternKey> fours;

    for (Move direction : kDirections) {
        for (int startOffset = -4; startOffset <= 0; ++startOffset) {
            PatternKey blackStones{};
            int blackCount = 0;
            int emptyCount = 0;
            Move empty;
            bool inside = true;

            for (int offset = 0; offset < 5; ++offset) {
                const Move current{
                    placed.x + (startOffset + offset) * direction.x,
                    placed.y + (startOffset + offset) * direction.y,
                };
                if (!Board::inBounds(current)) {
                    inside = false;
                    break;
                }
                const Stone stone = context.cells[current.index()];
                if (stone == Stone::Black) {
                    if (blackCount < static_cast<int>(blackStones.size())) {
                        blackStones[blackCount] = current.index();
                    }
                    ++blackCount;
                } else if (stone == Stone::Empty) {
                    ++emptyCount;
                    empty = current;
                }
            }

            if (!inside || blackCount != 4 || emptyCount != 1) {
                continue;
            }
            if (!isLegalWinningPoint(context, empty, direction)) {
                continue;
            }
            std::sort(blackStones.begin(), blackStones.end());
            fours.insert(blackStones);
            if (fours.size() >= 2) {
                return 2;
            }
        }
    }
    return static_cast<int>(fours.size());
}

ForbiddenReason analyzePlacedBlack(AnalysisContext& context, Move placed);

[[nodiscard]] bool extensionIsLegal(AnalysisContext& context, Move extension) {
    context.placeBlack(extension);
    // A THREE must be extendable to a straight four without attaining five
    // in the same move (RIF definition of THREE).
    const bool attainsFive = hasExactFiveAt(context.cells, extension);
    const ForbiddenReason reason = attainsFive
                                       ? ForbiddenReason::None
                                       : analyzePlacedBlack(context, extension);
    context.removeBlack(extension);
    return !attainsFive && reason == ForbiddenReason::None;
}

[[nodiscard]] int countOpenThrees(AnalysisContext& context, Move placed) {
    std::set<ThreeKey> threes;

    for (Move direction : kDirections) {
        // In .BBBB. the placed stone must occupy one of the four middle cells.
        for (int startOffset = -4; startOffset <= -1; ++startOffset) {
            std::array<Move, 6> window{};
            bool inside = true;
            for (int offset = 0; offset < 6; ++offset) {
                window[offset] = Move{
                    placed.x + (startOffset + offset) * direction.x,
                    placed.y + (startOffset + offset) * direction.y,
                };
                if (!Board::inBounds(window[offset])) {
                    inside = false;
                    break;
                }
            }
            if (!inside || context.cells[window.front().index()] != Stone::Empty
                || context.cells[window.back().index()] != Stone::Empty) {
                continue;
            }

            ThreeKey threeStones{};
            int blackCount = 0;
            int emptyCount = 0;
            Move extension;
            for (int offset = 1; offset <= 4; ++offset) {
                const Stone stone = context.cells[window[offset].index()];
                if (stone == Stone::Black) {
                    if (blackCount < static_cast<int>(threeStones.size())) {
                        threeStones[blackCount] = window[offset].index();
                    }
                    ++blackCount;
                } else if (stone == Stone::Empty) {
                    ++emptyCount;
                    extension = window[offset];
                }
            }
            if (blackCount != 3 || emptyCount != 1) {
                continue;
            }

            // A real open three must have a legal continuation to a straight four,
            // and both ends of that four must remain legal exact-five winning points.
            context.placeBlack(extension);
            const bool openFour = isLegalWinningPoint(context, window.front(), direction)
                                  && isLegalWinningPoint(context, window.back(), direction);
            context.removeBlack(extension);
            if (!openFour || !extensionIsLegal(context, extension)) {
                continue;
            }

            std::sort(threeStones.begin(), threeStones.end());
            threes.insert(threeStones);
            if (threes.size() >= 2) {
                return 2;
            }
        }
    }
    return static_cast<int>(threes.size());
}

ForbiddenReason analyzePlacedBlack(AnalysisContext& context, Move placed) {
    const std::uint64_t key = context.memoKey(placed);
    if (const auto found = context.memo.find(key); found != context.memo.end()) {
        return found->second;
    }

    ForbiddenReason result = ForbiddenReason::None;
    if (hasExactFiveAt(context.cells, placed)) {
        // Attaining five wins immediately and takes priority over all forbidden
        // patterns made by the same move (RIF 9.1/9.2).
        result = ForbiddenReason::None;
    } else if (hasOverlineAt(context.cells, placed)) {
        result = ForbiddenReason::Overline;
    } else if (countFours(context, placed) >= 2) {
        result = ForbiddenReason::DoubleFour;
    } else if (countOpenThrees(context, placed) >= 2) {
        result = ForbiddenReason::DoubleThree;
    }

    context.memo.emplace(key, result);
    return result;
}

[[nodiscard]] bool runWins(Stone stone, int length, RuleSet rules) noexcept {
    if (rules == RuleSet::Renju && stone == Stone::Black) {
        return length == 5;
    }
    return length >= 5;
}

}  // namespace

ForbiddenReason Rules::forbiddenReason(
    const Board& board, Move move, Stone stone, RuleSet rules) {
    if (!Board::inBounds(move) || !isPlayableStone(stone)) {
        return ForbiddenReason::OutOfBounds;
    }
    if (!board.empty(move)) {
        return ForbiddenReason::Occupied;
    }
    if (rules != RuleSet::Renju || stone == Stone::White) {
        return ForbiddenReason::None;
    }

    AnalysisContext context(board);
    context.placeBlack(move);
    return analyzePlacedBlack(context, move);
}

ForbiddenReason Rules::forbiddenReason(const Board& board, Move move, RuleSet rules) {
    return forbiddenReason(board, move, board.sideToMove(), rules);
}

bool Rules::isLegalMove(const Board& board, Move move, Stone stone, RuleSet rules) {
    return forbiddenReason(board, move, stone, rules) == ForbiddenReason::None;
}

bool Rules::isLegalMove(const Board& board, Move move, RuleSet rules) {
    return forbiddenReason(board, move, board.sideToMove(), rules)
           == ForbiddenReason::None;
}

bool Rules::isWinningPlacement(
    const Board& board, Move move, Stone stone, RuleSet rules) {
    if (!Board::inBounds(move) || !isPlayableStone(stone) || !board.empty(move)) {
        return false;
    }

    Board::Cells cells = board.cells();
    cells[move.index()] = stone;
    // A Renju black move that wins with an exact five is legal unless the same
    // placement also makes an overline. hasWinningLineAt performs exactly that
    // check, while double-three and double-four are overridden by the five.
    return ::gomoku::hasWinningLineAt(cells, move, stone, rules);
}

bool Rules::hasWinningLineAt(
    const Board& board, Move move, Stone stone, RuleSet rules) {
    return ::gomoku::hasWinningLineAt(board.cells(), move, stone, rules);
}

WinnerInfo Rules::winners(const Board& board, RuleSet rules) {
    WinnerInfo result;
    const Board::Cells& cells = board.cells();

    for (int y = 0; y < kBoardSize; ++y) {
        for (int x = 0; x < kBoardSize; ++x) {
            const Stone stone = cellAt(cells, x, y);
            if (!isPlayableStone(stone)) {
                continue;
            }
            for (Move direction : kDirections) {
                const int previousX = x - direction.x;
                const int previousY = y - direction.y;
                if (Board::inBounds(previousX, previousY)
                    && cellAt(cells, previousX, previousY) == stone) {
                    continue;
                }

                int length = 0;
                int currentX = x;
                int currentY = y;
                while (Board::inBounds(currentX, currentY)
                       && cellAt(cells, currentX, currentY) == stone) {
                    ++length;
                    currentX += direction.x;
                    currentY += direction.y;
                }
                if (!runWins(stone, length, rules)) {
                    continue;
                }
                if (stone == Stone::Black) {
                    result.black = true;
                } else {
                    result.white = true;
                }
            }
        }
    }
    return result;
}

bool Rules::hasWinner(const Board& board, Stone stone, RuleSet rules) {
    const WinnerInfo result = winners(board, rules);
    return stone == Stone::Black ? result.black
                                 : stone == Stone::White ? result.white : false;
}

}  // namespace gomoku
