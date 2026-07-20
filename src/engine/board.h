#pragma once

#include "engine/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace gomoku {

class Board {
public:
    using Cells = std::array<Stone, kBoardArea>;
    using const_iterator = Cells::const_iterator;

    explicit Board(Stone sideToMove = Stone::Black);
    Board(const Cells& cells, Stone sideToMove = Stone::Black);

    [[nodiscard]] static constexpr bool inBounds(int x, int y) noexcept {
        return x >= 0 && x < kBoardSize && y >= 0 && y < kBoardSize;
    }
    [[nodiscard]] static constexpr bool inBounds(Move move) noexcept {
        return inBounds(move.x, move.y);
    }

    [[nodiscard]] Stone at(int x, int y) const noexcept;
    [[nodiscard]] Stone at(Move move) const noexcept;
    [[nodiscard]] Stone operator[](Move move) const noexcept { return at(move); }
    [[nodiscard]] bool empty(Move move) const noexcept;
    [[nodiscard]] bool full() const noexcept { return occupiedCount() == kBoardArea; }

    // Setup operations invalidate move history and can be used to load arbitrary positions.
    bool set(int x, int y, Stone stone);
    bool set(Move move, Stone stone);
    bool load(const Cells& cells, Stone sideToMove = Stone::Black);
    void clear(Stone sideToMove = Stone::Black);
    bool setSideToMove(Stone stone);
    void recomputeHash();

    // Search operations are incremental. Explicit-color makeMove sets the next side
    // to the opponent of the placed color and unmakeMove restores the exact prior state.
    bool makeMove(Move move);
    bool makeMove(Move move, Stone stone);
    bool unmakeMove();

    [[nodiscard]] Stone sideToMove() const noexcept { return sideToMove_; }
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
    [[nodiscard]] int count(Stone stone) const noexcept;
    [[nodiscard]] int occupiedCount() const noexcept;
    [[nodiscard]] int emptyCount() const noexcept { return kBoardArea - occupiedCount(); }
    [[nodiscard]] std::size_t historySize() const noexcept { return history_.size(); }
    [[nodiscard]] std::optional<Move> lastMove() const noexcept;

    [[nodiscard]] const Cells& cells() const noexcept { return cells_; }
    [[nodiscard]] const Stone* data() const noexcept { return cells_.data(); }
    [[nodiscard]] const_iterator begin() const noexcept { return cells_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return cells_.end(); }

    void emptyMoves(std::vector<Move>& output) const;
    [[nodiscard]] std::vector<Move> emptyMoves() const;

    template <class Visitor>
    void forEachCell(Visitor&& visitor) const {
        for (int index = 0; index < kBoardArea; ++index) {
            std::forward<Visitor>(visitor)(Move::fromIndex(index), cells_[index]);
        }
    }

    template <class Visitor>
    void forEachEmpty(Visitor&& visitor) const {
        for (int index = 0; index < kBoardArea; ++index) {
            if (cells_[index] == Stone::Empty) {
                std::forward<Visitor>(visitor)(Move::fromIndex(index));
            }
        }
    }

    template <class Visitor>
    void forEachOccupied(Visitor&& visitor) const {
        for (int index = 0; index < kBoardArea; ++index) {
            if (cells_[index] != Stone::Empty) {
                std::forward<Visitor>(visitor)(Move::fromIndex(index), cells_[index]);
            }
        }
    }

private:
    struct UndoState {
        Move move;
        Stone previousSide;
        std::uint64_t previousHash;
    };

    Cells cells_{};
    Stone sideToMove_ = Stone::Black;
    std::array<int, 3> counts_{};
    std::uint64_t hash_ = 0;
    std::vector<UndoState> history_;
};

}  // namespace gomoku
