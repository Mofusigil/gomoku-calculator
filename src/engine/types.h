#pragma once

#include <compare>
#include <cstdint>

namespace gomoku {

inline constexpr int kBoardSize = 15;
inline constexpr int kBoardArea = kBoardSize * kBoardSize;

enum class Stone : std::uint8_t {
    Empty = 0,
    Black = 1,
    White = 2,
};

[[nodiscard]] constexpr Stone opponent(Stone stone) noexcept {
    return stone == Stone::Black ? Stone::White
                                 : stone == Stone::White ? Stone::Black : Stone::Empty;
}

enum class RuleSet : std::uint8_t {
    Freestyle,
    Renju,
};

using Ruleset = RuleSet;

enum class ForbiddenReason : std::uint8_t {
    None,
    OutOfBounds,
    Occupied,
    Overline,
    DoubleFour,
    DoubleThree,
};

struct Move {
    int x = -1;
    int y = -1;

    [[nodiscard]] constexpr int index() const noexcept {
        return y * kBoardSize + x;
    }

    [[nodiscard]] static constexpr Move fromIndex(int index) noexcept {
        return Move{index % kBoardSize, index / kBoardSize};
    }

    auto operator<=>(const Move&) const = default;
};

struct WinnerInfo {
    bool black = false;
    bool white = false;

    [[nodiscard]] constexpr bool any() const noexcept { return black || white; }
    [[nodiscard]] constexpr bool both() const noexcept { return black && white; }
    [[nodiscard]] constexpr Stone uniqueWinner() const noexcept {
        if (black == white) {
            return Stone::Empty;
        }
        return black ? Stone::Black : Stone::White;
    }
};

}  // namespace gomoku
