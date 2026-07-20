#include "engine/search.h"
#include "server/json.h"
#include "service/analysis_jobs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using gomoku::AnalysisResult;
using gomoku::AnalysisJobManager;
using gomoku::Analyzer;
using gomoku::Board;
using gomoku::ForbiddenReason;
using gomoku::Move;
using gomoku::RuleSet;
using gomoku::Rules;
using gomoku::SearchLimits;
using gomoku::Stone;
namespace json = gomoku::server::json;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void place(Board& board, Stone stone, std::initializer_list<Move> moves) {
    for (Move move : moves) {
        if (!board.set(move, stone)) {
            throw std::runtime_error("test setup failed");
        }
    }
}

SearchLimits quickLimits(int matePlies = 7) {
    SearchLimits limits;
    limits.timeMs = 600;
    limits.maxDepth = 3;
    limits.maxMatePlies = matePlies;
    limits.maxCandidates = 20;
    return limits;
}

void testBoardRoundTrip() {
    Board board(Stone::White);
    place(board, Stone::Black, {{2, 3}, {4, 5}});
    place(board, Stone::White, {{7, 8}});
    board.setSideToMove(Stone::Black);
    const auto cells = board.cells();
    const auto hash = board.hash();
    const Stone side = board.sideToMove();

    expect(board.makeMove({7, 7}), "makeMove accepts an empty point");
    expect(board.at(7, 7) == Stone::Black, "makeMove places the current side");
    expect(board.sideToMove() == Stone::White, "makeMove changes the side to move");
    expect(board.unmakeMove(), "unmakeMove succeeds");
    expect(board.cells() == cells, "unmakeMove restores all cells");
    expect(board.hash() == hash, "unmakeMove restores the Zobrist hash");
    expect(board.sideToMove() == side, "unmakeMove restores the side to move");
}

void testBasicWins() {
    Board board;
    place(board, Stone::Black, {{3, 7}, {4, 7}, {5, 7}, {6, 7}});
    expect(Rules::isWinningPlacement(board, {7, 7}, Stone::Black, RuleSet::Freestyle),
           "freestyle exact five wins");
    expect(Rules::isWinningPlacement(board, {7, 7}, Stone::Black, RuleSet::Renju),
           "Renju black exact five wins");

    Board overline;
    place(overline, Stone::Black, {{2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}});
    expect(Rules::isWinningPlacement(overline, {7, 7}, Stone::Black, RuleSet::Freestyle),
           "freestyle overline wins");
    expect(Rules::forbiddenReason(overline, {7, 7}, Stone::Black, RuleSet::Renju) ==
               ForbiddenReason::Overline,
           "Renju black overline is forbidden");
    expect(!Rules::isWinningPlacement(overline, {7, 7}, Stone::Black, RuleSet::Renju),
           "Renju black overline is not a win");

    Board fiveAndOverline;
    place(fiveAndOverline, Stone::Black,
          {{3, 7}, {4, 7}, {5, 7}, {6, 7},
           {7, 2}, {7, 3}, {7, 4}, {7, 5}, {7, 6}});
    expect(Rules::forbiddenReason(
               fiveAndOverline, {7, 7}, Stone::Black, RuleSet::Renju) ==
               ForbiddenReason::None,
           "an exact five takes priority over a simultaneous perpendicular overline");
    expect(Rules::isWinningPlacement(
               fiveAndOverline, {7, 7}, Stone::Black, RuleSet::Renju),
           "an exact five plus a perpendicular overline wins for black");

    Board whiteOverline;
    place(whiteOverline, Stone::White, {{2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}});
    expect(Rules::isWinningPlacement(
               whiteOverline, {7, 7}, Stone::White, RuleSet::Renju),
           "Renju white overline remains legal and wins");
}

void testForbiddenPatterns() {
    Board doubleFour;
    place(doubleFour, Stone::Black,
          {{5, 7}, {6, 7}, {8, 7}, {7, 5}, {7, 6}, {7, 8}});
    expect(Rules::forbiddenReason(
               doubleFour, {7, 7}, Stone::Black, RuleSet::Renju) ==
               ForbiddenReason::DoubleFour,
           "crossing fours are detected as double-four");
    expect(Rules::isLegalMove(doubleFour, {7, 7}, Stone::White, RuleSet::Renju),
           "the same double-four shape is legal for white");

    Board doubleThree;
    place(doubleThree, Stone::Black, {{6, 7}, {8, 7}, {7, 6}, {7, 8}});
    expect(Rules::forbiddenReason(
               doubleThree, {7, 7}, Stone::Black, RuleSet::Renju) ==
               ForbiddenReason::DoubleThree,
           "crossing open threes are detected as double-three");

    Board edgeFalseThree;
    place(edgeFalseThree, Stone::Black, {{0, 7}, {2, 7}, {1, 6}, {1, 8}});
    expect(Rules::forbiddenReason(
               edgeFalseThree, {1, 7}, Stone::Black, RuleSet::Renju) ==
               ForbiddenReason::None,
           "an edge-blocked false three is not counted twice");

    Board fiveMakingExtensions;
    place(fiveMakingExtensions, Stone::Black,
          {{6, 7}, {8, 7}, {7, 6}, {7, 8},
           {3, 5}, {4, 5}, {5, 5}, {6, 5},
           {8, 9}, {9, 9}, {10, 9}, {11, 9}});
    expect(Rules::forbiddenReason(
               fiveMakingExtensions, {7, 7}, Stone::Black, RuleSet::Renju) ==
               ForbiddenReason::None,
           "an extension that simultaneously makes five does not define a real three");

    Board fivePriorityFour;
    place(fivePriorityFour, Stone::Black,
          {{5, 7}, {6, 7}, {8, 7},
           {7, 8}, {7, 9}, {7, 11},
           {4, 10}, {5, 10}, {6, 10}, {8, 10}, {9, 10}});
    expect(Rules::forbiddenReason(
               fivePriorityFour, {7, 7}, Stone::Black, RuleSet::Renju) ==
               ForbiddenReason::DoubleFour,
           "a four stays real when its winning point also makes a perpendicular overline");
}

void testSearchImmediateWin() {
    Board board(Stone::Black);
    place(board, Stone::Black, {{3, 7}, {4, 7}, {5, 7}, {6, 7}});
    board.setSideToMove(Stone::Black);
    const AnalysisResult result = Analyzer{}.analyze(board, RuleSet::Freestyle, quickLimits());
    expect(result.proven, "an immediate win is proven");
    expect(result.winner == Stone::Black, "the immediate winner is black");
    expect(result.matePlies == 1, "an immediate win is mate in one total ply");
    expect(result.bestMove == Move{2, 7} || result.bestMove == Move{7, 7},
           "the immediate winning move is returned");
}

void testSearchVcf() {
    Board board(Stone::Black);
    place(board, Stone::Black, {{5, 7}, {6, 7}, {7, 7}});
    place(board, Stone::White, {{0, 0}});
    board.setSideToMove(Stone::Black);
    const AnalysisResult result = Analyzer{}.analyze(board, RuleSet::Freestyle, quickLimits(5));
    expect(result.proven, "an open-four construction is proven by forcing search");
    expect(result.winner == Stone::Black, "the VCF winner is black");
    expect(result.matePlies == 3, "the open-four construction is mate in three plies");
    expect(result.principalVariation.size() == 3, "the proven VCF includes a complete PV");
}

void testSearchDefenseAndLoss() {
    Board uniqueBlock(Stone::Black);
    place(uniqueBlock, Stone::White, {{0, 7}, {1, 7}, {2, 7}, {3, 7}});
    uniqueBlock.setSideToMove(Stone::Black);
    const AnalysisResult defense = Analyzer{}.analyze(
        uniqueBlock, RuleSet::Freestyle, quickLimits(3));
    expect(defense.bestMove == Move{4, 7}, "alpha-beta finds the unique forced block");

    Board doubleThreat(Stone::Black);
    place(doubleThreat, Stone::White, {{5, 7}, {6, 7}, {7, 7}, {8, 7}});
    doubleThreat.setSideToMove(Stone::Black);
    const AnalysisResult loss = Analyzer{}.analyze(
        doubleThreat, RuleSet::Freestyle, quickLimits(3));
    expect(loss.proven, "two distinct immediate wins prove a forced loss");
    expect(loss.winner == Stone::White, "the double-threat winner is white");
    expect(loss.matePlies == 2, "the double-threat loss is two plies away");

    Board forbiddenBlock(Stone::Black);
    place(forbiddenBlock, Stone::White, {{0, 7}, {1, 7}, {2, 7}, {3, 7}});
    place(forbiddenBlock, Stone::Black, {{4, 6}, {4, 8}, {3, 6}, {5, 8}});
    forbiddenBlock.setSideToMove(Stone::Black);
    const AnalysisResult trapped = Analyzer{}.analyze(
        forbiddenBlock, RuleSet::Renju, quickLimits(3));
    expect(trapped.proven, "an illegal unique block proves the opponent win");
    expect(trapped.winner == Stone::White, "the forbidden-block winner is white");
    expect(trapped.matePlies == 2, "the forbidden-block loss is two plies away");
    expect(trapped.bestMove.has_value(), "a legal waiting move is returned before the loss");
    expect(trapped.principalVariation.size() == 2,
           "the forbidden-block proof includes waiting move and winning reply");
}

void testDrawAndCancellation() {
    Board::Cells cells{};
    for (int y = 0; y < gomoku::kBoardSize; ++y) {
        for (int x = 0; x < gomoku::kBoardSize; ++x) {
            cells[static_cast<std::size_t>(y * gomoku::kBoardSize + x)] =
                ((x + 2 * y) % 4 < 2) ? Stone::Black : Stone::White;
        }
    }
    Board full(cells, Stone::Black);
    expect(!Rules::winners(full, RuleSet::Freestyle).any(),
           "the full-board draw fixture has no winning line");
    const AnalysisResult draw = Analyzer{}.analyze(full, RuleSet::Freestyle, quickLimits());
    expect(draw.kind == "draw" && draw.proven, "a full board without a winner is a proven draw");
    expect(draw.blackWinRate == 0.0 && draw.whiteWinRate == 0.0 && draw.drawRate == 1.0,
           "a proven draw reports zero win rates and draw probability one");

    Board cancellable(Stone::Black);
    place(cancellable, Stone::Black, {{7, 7}});
    SearchLimits limits = quickLimits();
    limits.cancelRequested = [] { return true; };
    const AnalysisResult cancelled = Analyzer{}.analyze(
        cancellable, RuleSet::Freestyle, limits);
    expect(cancelled.kind == "cancelled" && cancelled.stats.cancelled,
           "a cancellation callback stops analysis before search");
}

void testEstimatedRates() {
    Board board(Stone::White);
    place(board, Stone::Black, {{7, 7}, {5, 5}, {9, 9}});
    place(board, Stone::White, {{7, 8}});
    board.setSideToMove(Stone::White);
    const AnalysisResult result = Analyzer{}.analyze(board, RuleSet::Renju, quickLimits(3));
    expect(result.kind == "evaluation" || result.kind == "forced_mate",
           "an arbitrary unequal-count snapshot can be analyzed");
    expect(result.blackWinRate >= 0.0 && result.blackWinRate <= 1.0,
           "black estimated rate is bounded");
    expect(result.whiteWinRate >= 0.0 && result.whiteWinRate <= 1.0,
           "white estimated rate is bounded");
    expect(std::abs(result.blackWinRate + result.whiteWinRate - 1.0) < 1e-9,
           "estimated rates are complementary");
}

void testOpeningResponseLocality() {
    Board board(Stone::White);
    place(board, Stone::Black, {{7, 7}});
    board.setSideToMove(Stone::White);

    SearchLimits limits;
    limits.timeMs = 5'000;
    limits.maxDepth = 5;
    limits.maxMatePlies = 1;
    limits.maxCandidates = 24;
    const AnalysisResult result = Analyzer{}.analyze(
        board, RuleSet::Freestyle, limits);

    expect(result.bestMove.has_value(),
           "the opening response returns a move");
    if (!result.bestMove.has_value()) {
        return;
    }
    const int distance = std::max(
        std::abs(result.bestMove->x - 7),
        std::abs(result.bestMove->y - 7));
    expect(distance == 1,
           "the first reply stays connected to the centre stone");
}

void testOddEvenWinRateStability() {
    Board board(Stone::Black);
    place(board, Stone::Black, {{6, 7}, {7, 7}});
    place(board, Stone::White, {{6, 8}, {7, 8}});
    board.setSideToMove(Stone::Black);

    SearchLimits limits;
    limits.timeMs = 10'000;
    limits.maxDepth = 4;
    limits.maxMatePlies = 1;
    limits.maxCandidates = 16;

    std::vector<AnalysisResult> updates;
    const AnalysisResult result = Analyzer{}.analyze(
        board,
        RuleSet::Freestyle,
        limits,
        [&](const AnalysisResult& update) { updates.push_back(update); });

    expect(result.stats.completedDepth == 4,
           "the odd-even regression fixture completes depth four");
    expect(updates.size() == 4,
           "the odd-even regression fixture publishes every depth");
    if (updates.size() != 4) {
        return;
    }

    expect(std::abs(updates[0].score - updates[1].score) > 10'000 &&
               std::abs(updates[1].score - updates[2].score) > 10'000,
           "the fixture retains the raw odd-even horizon swing");

    const auto [minimum, maximum] = std::minmax_element(
        updates.begin(), updates.end(), [](const AnalysisResult& lhs,
                                           const AnalysisResult& rhs) {
            return lhs.blackWinRate < rhs.blackWinRate;
        });
    expect(maximum->blackWinRate - minimum->blackWinRate < 0.03,
           "paired horizon scores keep the displayed win rate stable");
    expect(result.winRateScore != result.score,
           "the API retains both stabilized and raw search scores");
    expect(std::all_of(result.candidates.begin(), result.candidates.end(),
                       [](const gomoku::CandidateResult& candidate) {
                           return candidate.winRateScore.has_value();
                       }),
           "candidate win rates use paired scores as well");
}

void testSearchPerformanceRegression() {
    Board board(Stone::White);
    place(board, Stone::Black,
          {{6, 6}, {8, 6}, {6, 7}, {8, 7}, {6, 8}, {7, 9}});
    place(board, Stone::White,
          {{6, 5}, {7, 6}, {5, 7}, {7, 7}, {7, 8}});
    board.setSideToMove(Stone::White);

    SearchLimits limits;
    limits.timeMs = 30'000;
    limits.maxDepth = 5;
    limits.maxMatePlies = 1;
    limits.maxCandidates = 16;
    const AnalysisResult result = Analyzer{}.analyze(
        board, RuleSet::Freestyle, limits);

    expect(result.kind == "evaluation", "the performance fixture stays heuristic");
    expect(result.stats.completedDepth == 5,
           "the performance fixture completes its fixed depth");
    expect(result.bestMove == Move{7, 4},
           "search optimization preserves the fixture's best move");
    expect(result.score == 11'850,
           "search optimization preserves the fixture's evaluation");
    expect(result.stats.nodes <= 23'500,
           "search optimization keeps the fixed-depth node count below the baseline");
}

void testStreamingAndJobs() {
    Board board(Stone::Black);
    SearchLimits limits = quickLimits();
    limits.maxDepth = 3;
    limits.timeMs = 1'000;
    std::vector<int> observedDepths;
    const AnalysisResult streamed = Analyzer{}.analyze(
        board,
        RuleSet::Freestyle,
        limits,
        [&](const AnalysisResult& update) {
            observedDepths.push_back(update.stats.completedDepth);
            expect(!update.candidates.empty(), "streaming updates include root candidates");
            if (!update.candidates.empty()) {
                expect(!update.candidates.front().principalVariation.empty(),
                       "each streamed candidate includes a concrete branch");
                expect(update.candidates.front().principalVariation.front() ==
                           update.candidates.front().move,
                       "candidate branch starts with its root move");
            }
        });
    expect(observedDepths == std::vector<int>({1, 2, 3}),
           "observer receives every completed iterative-deepening result");
    expect(streamed.stats.completedDepth == 3, "finite streaming search returns final depth");

    std::atomic<int> infiniteUpdates{0};
    SearchLimits infinite = quickLimits();
    infinite.infinite = true;
    infinite.cancelRequested = [&] {
        return infiniteUpdates.load(std::memory_order_acquire) >= 2;
    };
    const AnalysisResult stopped = Analyzer{}.analyze(
        Board(Stone::Black),
        RuleSet::Freestyle,
        infinite,
        [&](const AnalysisResult&) {
            infiniteUpdates.fetch_add(1, std::memory_order_release);
        });
    expect(stopped.kind == "cancelled" && infiniteUpdates.load() >= 2,
           "infinite analysis publishes updates until explicitly cancelled");

    AnalysisJobManager jobs(std::chrono::seconds(1), std::chrono::seconds(2));
    SearchLimits jobLimits = quickLimits();
    jobLimits.maxDepth = 3;
    jobLimits.timeMs = 1'000;
    const auto started = jobs.start(Board(Stone::Black), RuleSet::Freestyle, jobLimits);
    expect(started.jobId.has_value() && !started.busy,
           "analysis job manager starts a background search");
    if (started.jobId.has_value()) {
        AnalysisJobManager::PollResult poll;
        for (int attempt = 0; attempt < 200; ++attempt) {
            poll = jobs.poll(*started.jobId, 0);
            if (poll.found && !poll.running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(poll.found && !poll.running && poll.latest.has_value(),
               "background analysis completes with a retained result");
        expect(poll.rootSide == Stone::Black,
               "job snapshots retain the score perspective");
        if (poll.latest.has_value()) {
            expect(poll.latest->stats.completedDepth == 3,
                   "background job exposes the final completed depth");
        }
    }
}

void testJson() {
    const json::Value value = json::parse(
        R"({"board":["..............."],"side":"black","n":3,"ok":true,"text":"\u4e94\u5b50\u68cb"})");
    expect(value.is_object(), "JSON parser reads an object");
    expect(value.at("n").as_int64() == 3, "JSON integer is preserved");
    expect(value.at("text").as_string() == "五子棋", "JSON Unicode escapes are decoded");
    const std::string encoded = json::stringify(value);
    expect(json::parse(encoded) == value, "JSON stringify/parse round trip is stable");

    bool rejected = false;
    try {
        (void)json::parse(R"({"x":1,})");
    } catch (const json::ParseError&) {
        rejected = true;
    }
    expect(rejected, "JSON parser rejects trailing commas");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"board round trip", testBoardRoundTrip},
        {"basic wins", testBasicWins},
        {"forbidden patterns", testForbiddenPatterns},
        {"immediate search", testSearchImmediateWin},
        {"VCF search", testSearchVcf},
        {"defense and loss", testSearchDefenseAndLoss},
        {"draw and cancellation", testDrawAndCancellation},
        {"estimated rates", testEstimatedRates},
        {"opening response locality", testOpeningResponseLocality},
        {"odd-even win-rate stability", testOddEvenWinRateStability},
        {"search performance regression", testSearchPerformanceRegression},
        {"streaming and jobs", testStreamingAndJobs},
        {"JSON", testJson},
    };

    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << " threw: " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test groups passed\n";
    return 0;
}
