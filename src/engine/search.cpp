#include "engine/search.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace gomoku {
namespace {

constexpr int kInfinity = 120'000'000;
constexpr int kMateScore = 100'000'000;
constexpr int kMateThreshold = 90'000'000;
constexpr int kMaxSearchPly = 64;
constexpr std::uint64_t kRenjuHashSalt = 0x9E3779B97F4A7C15ULL;
constexpr std::array<Move, 4> kDirections{{
    {1, 0},
    {0, 1},
    {1, 1},
    {1, -1},
}};

enum class Bound : std::uint8_t {
    Exact,
    Lower,
    Upper,
};

struct TableEntry {
    int depth = -1;
    int score = 0;
    Bound bound = Bound::Exact;
    Move bestMove{};
    std::vector<Move> pv;
};

struct RootIteration {
    int score = 0;
    Move bestMove{};
    std::vector<Move> pv;
    std::vector<CandidateResult> candidates;
};

struct ForcedLine {
    bool forced = false;
    int plies = 0;
    std::vector<Move> pv;
};

class SearchTimeout final : public std::runtime_error {
public:
    SearchTimeout() : std::runtime_error("search timed out") {}
};

class SearchCancelled final : public std::runtime_error {
public:
    SearchCancelled() : std::runtime_error("search cancelled") {}
};

class ScopedMove final {
public:
    ScopedMove(Board& board, Move move) : board_(board) {
        if (!board_.makeMove(move)) {
            throw std::logic_error("failed to make a generated move");
        }
    }

    ~ScopedMove() { (void)board_.unmakeMove(); }

    ScopedMove(const ScopedMove&) = delete;
    ScopedMove& operator=(const ScopedMove&) = delete;

private:
    Board& board_;
};

std::uint64_t positionKey(const Board& board, RuleSet rules) {
    return board.hash() ^ (rules == RuleSet::Renju ? kRenjuHashSalt : 0ULL);
}

int scoreToTable(int score, int ply) {
    if (score > kMateThreshold) {
        return score + ply;
    }
    if (score < -kMateThreshold) {
        return score - ply;
    }
    return score;
}

int scoreFromTable(int score, int ply) {
    if (score > kMateThreshold) {
        return score - ply;
    }
    if (score < -kMateThreshold) {
        return score + ply;
    }
    return score;
}

bool containsMove(const std::vector<Move>& moves, Move move) {
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

double estimatedProbability(int score) {
    const double bounded = std::clamp(static_cast<double>(score), -17'500.0, 17'500.0);
    return 1.0 / (1.0 + std::exp(-bounded / 4'500.0));
}

bool couldCreateFour(const Board& board, Move move, Stone attacker) {
    for (Move direction : kDirections) {
        const int dx = direction.x;
        const int dy = direction.y;
        for (int start = -4; start <= 0; ++start) {
            int attackers = 0;
            bool blocked = false;
            for (int offset = 0; offset < 5; ++offset) {
                const int relative = start + offset;
                const int x = move.x + relative * dx;
                const int y = move.y + relative * dy;
                if (!Board::inBounds(x, y)) {
                    blocked = true;
                    break;
                }
                if (relative == 0) {
                    continue;
                }
                const Stone cell = board.at(x, y);
                if (cell == opponent(attacker)) {
                    blocked = true;
                    break;
                }
                attackers += cell == attacker ? 1 : 0;
            }
            if (!blocked && attackers >= 3) {
                return true;
            }
        }
    }
    return false;
}

class SearchContext {
public:
    SearchContext(RuleSet rules, SearchLimits limits, AnalysisObserver observer)
        : rules_(rules), limits_(std::move(limits)), observer_(std::move(observer)) {
        limits_.timeMs = std::clamp(limits_.timeMs, 50, 30'000);
        limits_.maxDepth = std::clamp(limits_.maxDepth, 1, 16);
        limits_.maxMatePlies = std::clamp(limits_.maxMatePlies, 1, 17);
        limits_.maxCandidates = std::clamp<std::size_t>(limits_.maxCandidates, 8, 96);
        table_.reserve(262'144);
    }

    AnalysisResult run(Board& board) {
        start_ = std::chrono::steady_clock::now();
        deadline_ = start_ + std::chrono::milliseconds(limits_.timeMs);
        try {
        checkDeadline();
        const WinnerInfo existingWinners = Rules::winners(board, rules_);
        if (existingWinners.any()) {
            AnalysisResult result;
            result.kind = existingWinners.both() ? "invalid" : "forced_mate";
            result.proven = !existingWinners.both();
            result.terminal = true;
            result.winner = existingWinners.uniqueWinner();
            if (result.proven) {
                result.matePlies = 0;
                result.proof = "terminal";
                result.score = result.winner == board.sideToMove()
                                   ? kMateScore
                                   : -kMateScore;
                assignCertainRates(result, result.winner);
            }
            finishStats(result);
            return result;
        }
        if (board.full()) {
            AnalysisResult result;
            result.kind = "draw";
            result.proven = true;
            result.terminal = true;
            result.proof = "full_board";
            result.blackWinRate = 0.0;
            result.whiteWinRate = 0.0;
            result.drawRate = 1.0;
            finishStats(result);
            return result;
        }

        const Stone rootSide = board.sideToMove();
        const auto opponentThreats = Evaluator::winningMoves(
            board, opponent(rootSide), rules_);
        const auto ownWins = Evaluator::winningMoves(board, rootSide, rules_);
        if (ownWins.empty() && opponentThreats.size() >= 2) {
            AnalysisResult result = provenDoubleThreatLoss(board, rootSide, opponentThreats);
            finishStats(result);
            return result;
        }
        if (ownWins.empty() && opponentThreats.size() == 1 &&
            !Rules::isLegalMove(board, opponentThreats.front(), rootSide, rules_)) {
            AnalysisResult result = provenUnblockableThreatLoss(
                board, rootSide, opponentThreats.front());
            finishStats(result);
            return result;
        }

        if (board.occupiedCount() >= 4 && limits_.maxMatePlies > 0) {
            const int forcingBudget = limits_.infinite
                                          ? 900
                                          : std::max(
                                                30, std::min(900, limits_.timeMs * 2 / 5));
            forcingDeadline_ = start_ + std::chrono::milliseconds(forcingBudget);
            if (!limits_.infinite) {
                forcingDeadline_ = std::min(forcingDeadline_, deadline_);
            }
            ForcedLine forced = solveForced(board, rootSide, limits_.maxMatePlies);
            if (forced.forced) {
                AnalysisResult result;
                result.kind = "forced_mate";
                result.proven = true;
                result.winner = rootSide;
                result.matePlies = forced.plies;
                result.bestMove = forced.pv.empty() ? std::optional<Move>{} : forced.pv.front();
                result.principalVariation = std::move(forced.pv);
                result.proof = "vcf";
                result.score = kMateScore - forced.plies;
                assignCertainRates(result, rootSide);
                finishStats(result);
                return result;
            }
        }

        return runEstimate(board, rootSide);
        } catch (const SearchCancelled&) {
            stats_.cancelled = true;
            AnalysisResult result;
            result.kind = "cancelled";
            finishStats(result);
            return result;
        }
    }

private:
    AnalysisResult runEstimate(Board& board, Stone rootSide) {
        std::optional<Move> preferred;
        RootIteration completed;
        bool hasCompletedIteration = false;
        // Gomoku shape scores heavily reward the horizon's last move. Centering
        // adjacent horizons removes that systematic odd-even bias from win rates.
        int previousScore = Evaluator::evaluate(board, rootSide, rules_);
        int completedWinRateScore = previousScore;
        std::vector<CandidateResult> previousCandidates;

        const int iterationLimit = limits_.infinite ? kMaxSearchPly : limits_.maxDepth;
        for (int depth = 1; depth <= iterationLimit; ++depth) {
            try {
                RootIteration iteration = searchRoot(board, depth, preferred);
                completedWinRateScore = std::midpoint(previousScore, iteration.score);
                stabilizeCandidateScores(
                    iteration.candidates, previousCandidates, previousScore);
                completed = std::move(iteration);
                hasCompletedIteration = true;
                stats_.completedDepth = depth;
                preferred = completed.bestMove;
                AnalysisResult snapshot = estimateResult(
                    completed, rootSide, completedWinRateScore);
                finishStats(snapshot);
                notifyObserver(snapshot);
                previousScore = completed.score;
                previousCandidates = completed.candidates;
            } catch (const SearchTimeout&) {
                stats_.timedOut = true;
                break;
            }
        }

        if (!hasCompletedIteration) {
            const auto moves = Evaluator::candidateMoves(
                board, rootSide, rules_, std::nullopt, limits_.maxCandidates);
            if (!moves.empty()) {
                completed.bestMove = moves.front().move;
                completed.score = Evaluator::movePotential(board, moves.front().move, rootSide);
                completed.pv = {moves.front().move};
                completed.candidates.push_back({
                    moves.front().move,
                    completed.score,
                    {moves.front().move},
                    std::nullopt,
                });
            } else {
                completed.score = 0;
            }
            completedWinRateScore = std::midpoint(previousScore, completed.score);
            stabilizeCandidateScores(
                completed.candidates, previousCandidates, previousScore);
        }

        AnalysisResult result = estimateResult(
            completed, rootSide, completedWinRateScore);
        finishStats(result);
        return result;
    }

    static void stabilizeCandidateScores(
        std::vector<CandidateResult>& candidates,
        const std::vector<CandidateResult>& previousCandidates,
        int previousRootScore) {
        for (CandidateResult& candidate : candidates) {
            const auto previous = std::find_if(
                previousCandidates.begin(), previousCandidates.end(),
                [&](const CandidateResult& prior) { return prior.move == candidate.move; });
            const int previousScore = previous == previousCandidates.end()
                                          ? previousRootScore
                                          : previous->score;
            candidate.winRateScore = std::midpoint(previousScore, candidate.score);
        }
    }

    AnalysisResult estimateResult(
        const RootIteration& iteration,
        Stone rootSide,
        int winRateScore) const {
        AnalysisResult result;
        result.kind = "evaluation";
        result.score = iteration.score;
        result.winRateScore = winRateScore;
        if (Board::inBounds(iteration.bestMove)) {
            result.bestMove = iteration.bestMove;
        }
        result.principalVariation = iteration.pv;
        result.candidates = iteration.candidates;
        const double rootProbability = estimatedProbability(result.winRateScore);
        result.blackWinRate = rootSide == Stone::Black ? rootProbability : 1.0 - rootProbability;
        result.whiteWinRate = 1.0 - result.blackWinRate;
        return result;
    }

    RootIteration searchRoot(Board& board, int depth, std::optional<Move> preferred) {
        checkDeadline();
        const Stone side = board.sideToMove();
        const auto ownWins = Evaluator::winningMoves(board, side, rules_);
        std::vector<RankedMove> moves;
        if (!ownWins.empty()) {
            for (Move move : ownWins) {
                moves.push_back({move, kMateScore, true, false});
            }
        } else {
            const auto opponentWins = Evaluator::winningMoves(
                board, opponent(side), rules_);
            if (opponentWins.size() >= 2) {
                RootIteration loss;
                loss.score = -kMateScore + 2;
                loss.pv = immediateLossPv(board, side, opponentWins);
                if (!loss.pv.empty()) {
                    loss.bestMove = loss.pv.front();
                }
                return loss;
            }
            if (opponentWins.size() == 1) {
                const Move block = opponentWins.front();
                if (Rules::isLegalMove(board, block, side, rules_)) {
                    moves.push_back({block, 80'000'000, false, true});
                } else {
                    RootIteration loss;
                    loss.score = -kMateScore + 2;
                    if (const auto waiting = firstLegalMove(board, side, block);
                        waiting.has_value()) {
                        loss.bestMove = *waiting;
                        loss.pv = {*waiting, block};
                    }
                    return loss;
                }
            } else {
                moves = Evaluator::candidateMoves(
                    board, side, rules_, preferred, limits_.maxCandidates,
                    &ownWins, &opponentWins);
            }
        }

        RootIteration result;
        if (moves.empty()) {
            result.score = board.full() ? 0 : -kMateScore + 1;
            return result;
        }

        int bestScore = -kInfinity;
        Move bestMove{};
        std::vector<Move> bestPv;
        result.candidates.reserve(moves.size());

        for (const RankedMove& ranked : moves) {
            checkDeadline();
            const Move move = ranked.move;
            int score = 0;
            std::vector<Move> childPv;
            if (ranked.winsImmediately) {
                score = kMateScore - 1;
            } else {
                ScopedMove applied(board, move);
                score = -negamax(
                    board, depth - 1, 1, -kInfinity, kInfinity, childPv);
            }

            std::vector<Move> candidatePv;
            candidatePv.reserve(childPv.size() + 1);
            candidatePv.push_back(move);
            candidatePv.insert(candidatePv.end(), childPv.begin(), childPv.end());
            result.candidates.push_back({
                move,
                score,
                candidatePv,
                std::nullopt,
            });
            if (score > bestScore) {
                bestScore = score;
                bestMove = move;
                bestPv = std::move(candidatePv);
            }
        }

        std::stable_sort(result.candidates.begin(), result.candidates.end(),
                         [](const CandidateResult& lhs, const CandidateResult& rhs) {
                             if (lhs.score != rhs.score) {
                                 return lhs.score > rhs.score;
                             }
                             return lhs.move.index() < rhs.move.index();
                         });
        if (result.candidates.size() > 8) {
            result.candidates.resize(8);
        }
        result.score = bestScore;
        result.bestMove = bestMove;
        result.pv = std::move(bestPv);
        storeTable(board, depth, bestScore, Bound::Exact, bestMove, result.pv, 0);
        return result;
    }

    int negamax(
        Board& board,
        int depth,
        int ply,
        int alpha,
        int beta,
        std::vector<Move>& pv) {
        ++stats_.nodes;
        checkDeadline();
        if (ply >= kMaxSearchPly || board.full()) {
            return 0;
        }

        const int originalAlpha = alpha;
        const int originalBeta = beta;
        const std::uint64_t key = positionKey(board, rules_);
        std::optional<Move> preferred;
        if (const auto found = table_.find(key); found != table_.end()) {
            ++stats_.transpositionHits;
            if (Board::inBounds(found->second.bestMove)) {
                preferred = found->second.bestMove;
            }
            if (found->second.depth >= depth) {
                const int tableScore = scoreFromTable(found->second.score, ply);
                if (found->second.bound == Bound::Exact) {
                    pv = found->second.pv;
                    return tableScore;
                }
                if (found->second.bound == Bound::Lower) {
                    alpha = std::max(alpha, tableScore);
                } else {
                    beta = std::min(beta, tableScore);
                }
                if (alpha >= beta) {
                    return tableScore;
                }
            }
        }

        const Stone side = board.sideToMove();
        const auto ownWins = Evaluator::winningMoves(board, side, rules_);
        if (!ownWins.empty()) {
            pv = {ownWins.front()};
            const int score = kMateScore - (ply + 1);
            storeTable(
                board, kMaxSearchPly, score, Bound::Exact, pv.front(), pv, ply);
            return score;
        }
        const auto opponentWins = Evaluator::winningMoves(
            board, opponent(side), rules_);
        if (opponentWins.size() >= 2) {
            pv = immediateLossPv(board, side, opponentWins);
            const int score = -kMateScore + (ply + 2);
            const Move bestMove = pv.empty() ? Move{} : pv.front();
            storeTable(
                board, kMaxSearchPly, score, Bound::Exact, bestMove, pv, ply);
            return score;
        }
        if (depth <= 0) {
            const int score = quiescence(board, ply, alpha, beta, 3, opponentWins, pv);
            const Move bestMove = pv.empty() ? Move{} : pv.front();
            storeTable(board, 0, score, Bound::Exact, bestMove, pv, ply);
            return score;
        }

        std::vector<RankedMove> moves;
        if (opponentWins.size() == 1) {
            const Move block = opponentWins.front();
            if (!Rules::isLegalMove(board, block, side, rules_)) {
                pv = unblockableLossPv(board, side, block);
                const int score = -kMateScore + (ply + 2);
                const Move bestMove = pv.empty() ? Move{} : pv.front();
                storeTable(
                    board, kMaxSearchPly, score, Bound::Exact, bestMove, pv, ply);
                return score;
            }
            moves.push_back({block, 80'000'000, false, true});
        } else {
            const std::size_t depthLimit = depth >= 5
                                               ? std::max<std::size_t>(12, limits_.maxCandidates / 2)
                                               : limits_.maxCandidates;
            moves = Evaluator::candidateMoves(
                board, side, rules_, preferred, depthLimit, &ownWins, &opponentWins);
        }
        for (RankedMove& ranked : moves) {
            ranked.orderingScore += history_[static_cast<int>(side)][ranked.move.index()];
            if (ply < kMaxSearchPly && killers_[ply][0] == ranked.move) {
                ranked.orderingScore += 3'000'000;
            } else if (ply < kMaxSearchPly && killers_[ply][1] == ranked.move) {
                ranked.orderingScore += 1'500'000;
            }
        }
        std::stable_sort(moves.begin(), moves.end(), [](const RankedMove& lhs, const RankedMove& rhs) {
            return lhs.orderingScore > rhs.orderingScore;
        });
        if (moves.empty()) {
            return 0;
        }

        int bestScore = -kInfinity;
        Move bestMove{};
        std::vector<Move> bestPv;
        int moveNumber = 0;
        for (const RankedMove& ranked : moves) {
            ++moveNumber;
            const Move move = ranked.move;
            std::vector<Move> childPv;
            int score;
            {
                ScopedMove applied(board, move);
                if (moveNumber == 1) {
                    score = -negamax(board, depth - 1, ply + 1, -beta, -alpha, childPv);
                } else {
                    score = -negamax(board, depth - 1, ply + 1, -alpha - 1, -alpha, childPv);
                    if (score > alpha && score < beta) {
                        childPv.clear();
                        score = -negamax(board, depth - 1, ply + 1, -beta, -alpha, childPv);
                    }
                }
            }

            if (score > bestScore) {
                bestScore = score;
                bestMove = move;
                bestPv.clear();
                bestPv.push_back(move);
                bestPv.insert(bestPv.end(), childPv.begin(), childPv.end());
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                ++stats_.betaCutoffs;
                history_[static_cast<int>(side)][move.index()] += depth * depth;
                if (ply < kMaxSearchPly && killers_[ply][0] != move) {
                    killers_[ply][1] = killers_[ply][0];
                    killers_[ply][0] = move;
                }
                break;
            }
        }

        Bound bound = Bound::Exact;
        if (bestScore <= originalAlpha) {
            bound = Bound::Upper;
        } else if (bestScore >= originalBeta) {
            bound = Bound::Lower;
        }
        storeTable(board, depth, bestScore, bound, bestMove, bestPv, ply);
        pv = std::move(bestPv);
        return bestScore;
    }

    int quiescence(
        Board& board,
        int ply,
        int alpha,
        int beta,
        int remaining,
        const std::vector<Move>& opponentWins,
        std::vector<Move>& pv) {
        ++stats_.nodes;
        checkDeadline();
        if (remaining <= 0 || opponentWins.empty()) {
            return Evaluator::evaluate(board, board.sideToMove(), rules_);
        }
        if (opponentWins.size() >= 2) {
            pv = immediateLossPv(board, board.sideToMove(), opponentWins);
            return -kMateScore + (ply + 2);
        }

        const Move block = opponentWins.front();
        const Stone side = board.sideToMove();
        if (!Rules::isLegalMove(board, block, side, rules_)) {
            pv = unblockableLossPv(board, side, block);
            return -kMateScore + (ply + 2);
        }
        std::vector<Move> childPv;
        int score;
        {
            ScopedMove applied(board, block);
            const auto childOpponentWins = Evaluator::winningMoves(
                board, opponent(board.sideToMove()), rules_);
            const auto ownWins = Evaluator::winningMoves(
                board, board.sideToMove(), rules_);
            if (!ownWins.empty()) {
                score = -(kMateScore - (ply + 2));
                childPv = {ownWins.front()};
            } else {
                score = -quiescence(
                    board, ply + 1, -beta, -alpha, remaining - 1,
                    childOpponentWins, childPv);
            }
        }
        pv.clear();
        pv.push_back(block);
        pv.insert(pv.end(), childPv.begin(), childPv.end());
        return score;
    }

    ForcedLine solveForced(Board& board, Stone attacker, int remaining) {
        ++stats_.forcingNodes;
        if (limits_.cancelRequested && limits_.cancelRequested()) {
            throw SearchCancelled{};
        }
        if (std::chrono::steady_clock::now() >= forcingDeadline_) {
            forceTimedOut_ = true;
            return {};
        }
        if (forceTimedOut_ || remaining <= 0) {
            return {};
        }

        const Stone side = board.sideToMove();
        const auto sideWins = Evaluator::winningMoves(board, side, rules_);
        if (!sideWins.empty()) {
            if (side == attacker && remaining >= 1) {
                return {true, 1, {sideWins.front()}};
            }
            return {};
        }

        if (side == attacker) {
            if (remaining < 3) {
                return {};
            }
            std::vector<RankedMove> forcingMoves;
            board.forEachEmpty([&](Move move) {
                if (!couldCreateFour(board, move, attacker) ||
                    !Rules::isLegalMove(board, move, attacker, rules_)) {
                    return;
                }
                forcingMoves.push_back({
                    move,
                    Evaluator::movePotential(board, move, attacker),
                    false,
                    false,
                });
            });
            std::stable_sort(forcingMoves.begin(), forcingMoves.end(),
                             [](const RankedMove& lhs, const RankedMove& rhs) {
                                 return lhs.orderingScore > rhs.orderingScore;
                             });

            ForcedLine best;
            for (const RankedMove& ranked : forcingMoves) {
                if (forceTimedOut_) {
                    break;
                }
                ForcedLine child;
                {
                    ScopedMove applied(board, ranked.move);
                    const auto threats = Evaluator::winningMoves(board, attacker, rules_);
                    if (threats.empty()) {
                        continue;
                    }
                    child = solveForced(board, attacker, remaining - 1);
                }
                if (!child.forced) {
                    continue;
                }
                child.plies += 1;
                child.pv.insert(child.pv.begin(), ranked.move);
                if (!best.forced || child.plies < best.plies) {
                    best = std::move(child);
                }
            }
            return best;
        }

        const auto defenderWins = Evaluator::winningMoves(board, side, rules_);
        if (!defenderWins.empty()) {
            return {};
        }
        const auto threats = Evaluator::winningMoves(board, attacker, rules_);
        if (threats.empty() || remaining < 2) {
            return {};
        }

        ForcedLine longestDefense{true, 0, {}};
        bool foundLegalBlock = false;
        for (Move block : threats) {
            if (!Rules::isLegalMove(board, block, side, rules_)) {
                continue;
            }
            foundLegalBlock = true;
            ForcedLine child;
            {
                ScopedMove applied(board, block);
                child = solveForced(board, attacker, remaining - 1);
            }
            if (!child.forced) {
                return {};
            }
            child.plies += 1;
            child.pv.insert(child.pv.begin(), block);
            if (child.plies > longestDefense.plies) {
                longestDefense = std::move(child);
            }
        }
        if (foundLegalBlock) {
            return longestDefense;
        }

        Move waitingMove{};
        bool hasWaitingMove = false;
        board.forEachEmpty([&](Move move) {
            if (!hasWaitingMove && !containsMove(threats, move) &&
                Rules::isLegalMove(board, move, side, rules_)) {
                waitingMove = move;
                hasWaitingMove = true;
            }
        });
        if (!hasWaitingMove) {
            return {true, 1, {threats.front()}};
        }
        std::vector<Move> remainingWins;
        {
            ScopedMove applied(board, waitingMove);
            remainingWins = Evaluator::winningMoves(board, attacker, rules_);
        }
        if (remainingWins.empty()) {
            return {};
        }
        return {true, 2, {waitingMove, remainingWins.front()}};
    }

    AnalysisResult provenDoubleThreatLoss(
        const Board& board, Stone rootSide, const std::vector<Move>& threats) {
        AnalysisResult result;
        result.kind = "forced_mate";
        result.proven = true;
        result.winner = opponent(rootSide);
        result.matePlies = 2;
        result.proof = "double_threat";
        result.score = -kMateScore + 2;

        for (Move block : threats) {
            if (Rules::isLegalMove(board, block, rootSide, rules_)) {
                result.bestMove = block;
                result.principalVariation.push_back(block);
                for (Move win : threats) {
                    if (win != block) {
                        result.principalVariation.push_back(win);
                        break;
                    }
                }
                break;
            }
        }
        if (!result.bestMove.has_value()) {
            if (const auto waiting = firstLegalMove(board, rootSide, std::nullopt);
                waiting.has_value()) {
                result.bestMove = *waiting;
                result.principalVariation = {*waiting, threats.front()};
            }
        }
        assignCertainRates(result, result.winner);
        return result;
    }

    AnalysisResult provenUnblockableThreatLoss(
        const Board& board, Stone rootSide, Move winningPoint) {
        AnalysisResult result;
        result.kind = "forced_mate";
        result.proven = true;
        result.winner = opponent(rootSide);
        result.matePlies = 2;
        result.proof = "forbidden_block";
        result.score = -kMateScore + 2;
        if (const auto waiting = firstLegalMove(board, rootSide, winningPoint);
            waiting.has_value()) {
            result.bestMove = *waiting;
            result.principalVariation = {*waiting, winningPoint};
        }
        assignCertainRates(result, result.winner);
        return result;
    }

    std::optional<Move> firstLegalMove(
        const Board& board, Stone side, std::optional<Move> excluded) const {
        std::optional<Move> result;
        board.forEachEmpty([&](Move move) {
            if (!result.has_value() && (!excluded.has_value() || move != *excluded) &&
                Rules::isLegalMove(board, move, side, rules_)) {
                result = move;
            }
        });
        return result;
    }

    std::vector<Move> immediateLossPv(
        const Board& board,
        Stone side,
        const std::vector<Move>& threats) const {
        for (Move block : threats) {
            if (!Rules::isLegalMove(board, block, side, rules_)) {
                continue;
            }
            for (Move win : threats) {
                if (win != block) {
                    return {block, win};
                }
            }
        }
        if (const auto waiting = firstLegalMove(board, side, std::nullopt);
            waiting.has_value()) {
            return {*waiting, threats.front()};
        }
        return {};
    }

    std::vector<Move> unblockableLossPv(
        const Board& board, Stone side, Move winningPoint) const {
        if (const auto waiting = firstLegalMove(board, side, winningPoint);
            waiting.has_value()) {
            return {*waiting, winningPoint};
        }
        return {};
    }

    void storeTable(
        const Board& board,
        int depth,
        int score,
        Bound bound,
        Move bestMove,
        const std::vector<Move>& pv,
        int ply) {
        if (table_.size() > 500'000) {
            table_.clear();
        }
        const std::uint64_t key = positionKey(board, rules_);
        const auto found = table_.find(key);
        if (found == table_.end() || depth >= found->second.depth) {
            table_[key] = TableEntry{
                depth,
                scoreToTable(score, ply),
                bound,
                bestMove,
                pv,
            };
        }
    }

    void checkDeadline() const {
        if (limits_.cancelRequested && limits_.cancelRequested()) {
            throw SearchCancelled{};
        }
        if (!limits_.infinite && std::chrono::steady_clock::now() >= deadline_) {
            throw SearchTimeout{};
        }
    }

    void notifyObserver(const AnalysisResult& snapshot) noexcept {
        if (!observer_) {
            return;
        }
        try {
            observer_(snapshot);
        } catch (...) {
            observer_ = nullptr;
        }
    }

    void assignCertainRates(AnalysisResult& result, Stone winner) const {
        result.winRateScore = result.score;
        result.blackWinRate = winner == Stone::Black ? 1.0 : 0.0;
        result.whiteWinRate = winner == Stone::White ? 1.0 : 0.0;
    }

    void finishStats(AnalysisResult& result) {
        stats_.elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count());
        if (stats_.elapsedMs > 0) {
            const std::uint64_t totalNodes = stats_.nodes + stats_.forcingNodes;
            stats_.nodesPerSecond = totalNodes * 1'000ULL /
                                   static_cast<std::uint64_t>(stats_.elapsedMs);
        }
        result.stats = stats_;
    }

    RuleSet rules_;
    SearchLimits limits_;
    AnalysisObserver observer_;
    SearchStats stats_;
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point forcingDeadline_{};
    bool forceTimedOut_ = false;
    std::unordered_map<std::uint64_t, TableEntry> table_;
    std::array<std::array<Move, 2>, kMaxSearchPly> killers_{};
    std::array<std::array<int, kBoardArea>, 3> history_{};
};

}  // namespace

AnalysisResult Analyzer::analyze(
    Board board,
    RuleSet rules,
    const SearchLimits& limits,
    AnalysisObserver observer) {
    SearchContext context(rules, limits, std::move(observer));
    return context.run(board);
}

}  // namespace gomoku
