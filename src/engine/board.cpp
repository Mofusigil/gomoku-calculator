#include "engine/board.h"

#include <algorithm>

namespace gomoku {
namespace {

[[nodiscard]] constexpr bool isPlayableStone(Stone stone) noexcept {
    return stone == Stone::Black || stone == Stone::White;
}

[[nodiscard]] constexpr bool isCellValue(Stone stone) noexcept {
    return stone == Stone::Empty || isPlayableStone(stone);
}

[[nodiscard]] constexpr std::uint64_t splitMix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr std::uint64_t pieceKey(int index, Stone stone) noexcept {
    if (!isPlayableStone(stone)) {
        return 0;
    }
    const auto color = static_cast<std::uint64_t>(stone);
    return splitMix64(0x6a09e667f3bcc909ULL
                      ^ (static_cast<std::uint64_t>(index) * 4ULL + color));
}

[[nodiscard]] constexpr std::uint64_t sideKey(Stone stone) noexcept {
    return splitMix64(0xbb67ae8584caa73bULL ^ static_cast<std::uint64_t>(stone));
}

}  // namespace

Board::Board(Stone sideToMove) {
    clear(isPlayableStone(sideToMove) ? sideToMove : Stone::Black);
}

Board::Board(const Cells& cells, Stone sideToMove) {
    if (!load(cells, sideToMove)) {
        clear();
    }
}

Stone Board::at(int x, int y) const noexcept {
    if (!inBounds(x, y)) {
        return Stone::Empty;
    }
    return cells_[y * kBoardSize + x];
}

Stone Board::at(Move move) const noexcept {
    return at(move.x, move.y);
}

bool Board::empty(Move move) const noexcept {
    return inBounds(move) && cells_[move.index()] == Stone::Empty;
}

bool Board::set(int x, int y, Stone stone) {
    return set(Move{x, y}, stone);
}

bool Board::set(Move move, Stone stone) {
    if (!inBounds(move) || !isCellValue(stone)) {
        return false;
    }

    const int index = move.index();
    const Stone old = cells_[index];
    history_.clear();
    if (old == stone) {
        return true;
    }

    hash_ ^= pieceKey(index, old);
    hash_ ^= pieceKey(index, stone);
    --counts_[static_cast<std::size_t>(old)];
    ++counts_[static_cast<std::size_t>(stone)];
    cells_[index] = stone;
    return true;
}

bool Board::load(const Cells& cells, Stone sideToMove) {
    if (!isPlayableStone(sideToMove)
        || std::any_of(cells.begin(), cells.end(), [](Stone stone) {
               return !isCellValue(stone);
           })) {
        return false;
    }

    cells_ = cells;
    sideToMove_ = sideToMove;
    history_.clear();
    recomputeHash();
    return true;
}

void Board::clear(Stone sideToMove) {
    cells_.fill(Stone::Empty);
    counts_.fill(0);
    counts_[static_cast<std::size_t>(Stone::Empty)] = kBoardArea;
    sideToMove_ = isPlayableStone(sideToMove) ? sideToMove : Stone::Black;
    hash_ = sideKey(sideToMove_);
    history_.clear();
}

bool Board::setSideToMove(Stone stone) {
    if (!isPlayableStone(stone)) {
        return false;
    }
    history_.clear();
    if (stone != sideToMove_) {
        hash_ ^= sideKey(sideToMove_);
        sideToMove_ = stone;
        hash_ ^= sideKey(sideToMove_);
    }
    return true;
}

void Board::recomputeHash() {
    counts_.fill(0);
    hash_ = sideKey(sideToMove_);
    for (int index = 0; index < kBoardArea; ++index) {
        const Stone stone = cells_[index];
        ++counts_[static_cast<std::size_t>(stone)];
        hash_ ^= pieceKey(index, stone);
    }
    history_.clear();
}

bool Board::makeMove(Move move) {
    return makeMove(move, sideToMove_);
}

bool Board::makeMove(Move move, Stone stone) {
    if (!empty(move) || !isPlayableStone(stone)) {
        return false;
    }

    history_.push_back(UndoState{move, sideToMove_, hash_});
    const int index = move.index();
    cells_[index] = stone;
    --counts_[static_cast<std::size_t>(Stone::Empty)];
    ++counts_[static_cast<std::size_t>(stone)];
    hash_ ^= pieceKey(index, stone);
    hash_ ^= sideKey(sideToMove_);
    sideToMove_ = opponent(stone);
    hash_ ^= sideKey(sideToMove_);
    return true;
}

bool Board::unmakeMove() {
    if (history_.empty()) {
        return false;
    }

    const UndoState undo = history_.back();
    history_.pop_back();
    const int index = undo.move.index();
    const Stone stone = cells_[index];
    cells_[index] = Stone::Empty;
    --counts_[static_cast<std::size_t>(stone)];
    ++counts_[static_cast<std::size_t>(Stone::Empty)];
    sideToMove_ = undo.previousSide;
    hash_ = undo.previousHash;
    return true;
}

int Board::count(Stone stone) const noexcept {
    if (!isCellValue(stone)) {
        return 0;
    }
    return counts_[static_cast<std::size_t>(stone)];
}

int Board::occupiedCount() const noexcept {
    return counts_[static_cast<std::size_t>(Stone::Black)]
           + counts_[static_cast<std::size_t>(Stone::White)];
}

std::optional<Move> Board::lastMove() const noexcept {
    if (history_.empty()) {
        return std::nullopt;
    }
    return history_.back().move;
}

void Board::emptyMoves(std::vector<Move>& output) const {
    output.clear();
    output.reserve(static_cast<std::size_t>(emptyCount()));
    forEachEmpty([&output](Move move) { output.push_back(move); });
}

std::vector<Move> Board::emptyMoves() const {
    std::vector<Move> output;
    emptyMoves(output);
    return output;
}

}  // namespace gomoku
