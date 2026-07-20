#include "engine/search.h"
#include "server/http_server.h"
#include "service/analysis_jobs.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using gomoku::AnalysisResult;
using gomoku::AnalysisJobManager;
using gomoku::Analyzer;
using gomoku::Board;
using gomoku::CandidateResult;
using gomoku::ForbiddenReason;
using gomoku::Move;
using gomoku::RuleSet;
using gomoku::Rules;
using gomoku::SearchLimits;
using gomoku::Stone;
using gomoku::kBoardArea;
using gomoku::kBoardSize;
using gomoku::server::HttpRequest;
using gomoku::server::HttpResponse;
using gomoku::server::HttpServer;
using gomoku::server::HttpServerOptions;
namespace json = gomoku::server::json;

class ApiError final : public std::runtime_error {
public:
    ApiError(int status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    [[nodiscard]] int status() const noexcept { return status_; }

private:
    int status_;
};

struct PositionInput {
    Board board;
    RuleSet rules = RuleSet::Freestyle;
};

std::counting_semaphore<4> gAnalysisSlots(4);

class AnalysisSlot final {
public:
    AnalysisSlot() : acquired_(gAnalysisSlots.try_acquire()) {}
    ~AnalysisSlot() {
        if (acquired_) {
            gAnalysisSlots.release();
        }
    }

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    bool acquired_ = false;
};

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    });
    return value;
}

const json::Value::Object& requireObject(const json::Value& value, std::string_view name) {
    if (!value.is_object()) {
        throw ApiError(422, std::string(name) + " 必须是 JSON 对象");
    }
    return value.as_object();
}

const json::Value* findAny(
    const json::Value::Object& object,
    std::initializer_list<std::string_view> keys) {
    for (std::string_view key : keys) {
        const auto found = object.find(key);
        if (found != object.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

int integerValue(const json::Value& value, std::string_view name) {
    if (!value.is_number()) {
        throw ApiError(422, std::string(name) + " 必须是整数");
    }
    const double number = value.as_double();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(std::numeric_limits<int>::min()) ||
        number > static_cast<double>(std::numeric_limits<int>::max())) {
        throw ApiError(422, std::string(name) + " 超出整数范围");
    }
    return static_cast<int>(number);
}

bool booleanValue(const json::Value& value, std::string_view name) {
    if (!value.is_bool()) {
        throw ApiError(422, std::string(name) + " 必须是布尔值");
    }
    return value.as_bool();
}

Stone parseStoneCell(const json::Value& value, int x, int y) {
    if (value.is_null()) {
        return Stone::Empty;
    }
    if (value.is_number()) {
        const int number = integerValue(value, "棋盘格");
        if (number >= 0 && number <= 2) {
            return static_cast<Stone>(number);
        }
    } else if (value.is_string()) {
        const std::string cell = lowerAscii(value.as_string());
        if (cell.empty() || cell == "." || cell == "_" || cell == "-" || cell == "0") {
            return Stone::Empty;
        }
        if (cell == "b" || cell == "black" || cell == "x" || cell == "1") {
            return Stone::Black;
        }
        if (cell == "w" || cell == "white" || cell == "o" || cell == "2") {
            return Stone::White;
        }
    }
    throw ApiError(422, "棋盘第 " + std::to_string(y + 1) + " 行第 " +
                            std::to_string(x + 1) + " 列包含无效棋子");
}

Stone parseSide(const json::Value::Object& object) {
    const json::Value* value = findAny(object, {"sideToMove", "side_to_move", "turn"});
    if (value == nullptr) {
        throw ApiError(422, "缺少 sideToMove，任意局面必须显式指定行棋方");
    }
    if (value->is_number()) {
        const int side = integerValue(*value, "sideToMove");
        if (side == 1 || side == 2) {
            return static_cast<Stone>(side);
        }
    } else if (value->is_string()) {
        const std::string side = lowerAscii(value->as_string());
        if (side == "black" || side == "b" || side == "1") {
            return Stone::Black;
        }
        if (side == "white" || side == "w" || side == "2") {
            return Stone::White;
        }
    }
    throw ApiError(422, "sideToMove 只能是 black 或 white");
}

RuleSet parseRules(const json::Value::Object& object) {
    const json::Value* value = findAny(object, {"rules", "rule", "forbidden"});
    if (value == nullptr) {
        return RuleSet::Freestyle;
    }
    if (value->is_bool()) {
        return value->as_bool() ? RuleSet::Renju : RuleSet::Freestyle;
    }
    if (value->is_string()) {
        const std::string rule = lowerAscii(value->as_string());
        if (rule == "renju" || rule == "forbidden" || rule == "restricted") {
            return RuleSet::Renju;
        }
        if (rule == "freestyle" || rule == "free" || rule == "gomoku") {
            return RuleSet::Freestyle;
        }
    }
    throw ApiError(422, "rules 只能是 freestyle 或 renju");
}

Board::Cells parseBoardCells(const json::Value::Object& object) {
    const json::Value* boardValue = findAny(object, {"board", "position"});
    if (boardValue == nullptr || !boardValue->is_array()) {
        throw ApiError(422, "board 必须是包含 15 行的数组");
    }
    const auto& rows = boardValue->as_array();
    if (rows.size() != static_cast<std::size_t>(kBoardSize)) {
        throw ApiError(422, "board 必须恰好包含 15 行");
    }

    Board::Cells cells{};
    cells.fill(Stone::Empty);
    for (int y = 0; y < kBoardSize; ++y) {
        const json::Value& row = rows[static_cast<std::size_t>(y)];
        if (row.is_string()) {
            const std::string& text = row.as_string();
            if (text.size() != static_cast<std::size_t>(kBoardSize)) {
                throw ApiError(422, "board 第 " + std::to_string(y + 1) + " 行必须包含 15 个字符");
            }
            for (int x = 0; x < kBoardSize; ++x) {
                const std::string cell(1, text[static_cast<std::size_t>(x)]);
                cells[static_cast<std::size_t>(y * kBoardSize + x)] =
                    parseStoneCell(json::Value(cell), x, y);
            }
            continue;
        }
        if (!row.is_array() || row.as_array().size() != static_cast<std::size_t>(kBoardSize)) {
            throw ApiError(422, "board 第 " + std::to_string(y + 1) + " 行必须包含 15 个格点");
        }
        for (int x = 0; x < kBoardSize; ++x) {
            cells[static_cast<std::size_t>(y * kBoardSize + x)] = parseStoneCell(
                row.as_array()[static_cast<std::size_t>(x)], x, y);
        }
    }
    return cells;
}

PositionInput parsePosition(const json::Value& body) {
    const auto& object = requireObject(body, "请求体");
    if (const json::Value* size = findAny(object, {"size"}); size != nullptr &&
        integerValue(*size, "size") != kBoardSize) {
        throw ApiError(422, "size 必须是 15");
    }
    const Stone side = parseSide(object);
    PositionInput input{Board(parseBoardCells(object), side), parseRules(object)};
    return input;
}

int optionInt(
    const json::Value::Object& root,
    std::initializer_list<std::string_view> keys,
    int fallback) {
    const json::Value::Object* source = &root;
    if (const json::Value* options = findAny(root, {"options", "limits"});
        options != nullptr && options->is_object()) {
        source = &options->as_object();
    }
    if (const json::Value* value = findAny(*source, keys); value != nullptr) {
        return integerValue(*value, *keys.begin());
    }
    if (source != &root) {
        if (const json::Value* value = findAny(root, keys); value != nullptr) {
            return integerValue(*value, *keys.begin());
        }
    }
    return fallback;
}

bool optionBool(
    const json::Value::Object& root,
    std::initializer_list<std::string_view> keys,
    bool fallback) {
    const json::Value::Object* source = &root;
    if (const json::Value* options = findAny(root, {"options", "limits"});
        options != nullptr && options->is_object()) {
        source = &options->as_object();
    }
    if (const json::Value* value = findAny(*source, keys); value != nullptr) {
        return booleanValue(*value, *keys.begin());
    }
    if (source != &root) {
        if (const json::Value* value = findAny(root, keys); value != nullptr) {
            return booleanValue(*value, *keys.begin());
        }
    }
    return fallback;
}

SearchLimits parseLimits(const json::Value& body) {
    const auto& object = requireObject(body, "请求体");
    if (const json::Value* options = findAny(object, {"options", "limits"});
        options != nullptr && !options->is_object()) {
        throw ApiError(422, "options 必须是对象");
    }
    SearchLimits limits;
    limits.timeMs = optionInt(object, {"timeLimitMs", "time_limit_ms", "timeMs"}, limits.timeMs);
    limits.maxDepth = optionInt(object, {"maxDepth", "max_depth", "depth"}, limits.maxDepth);
    limits.maxMatePlies = optionInt(
        object, {"threatDepth", "threat_depth", "maxMatePlies", "max_mate_plies"},
        limits.maxMatePlies);
    limits.maxCandidates = static_cast<std::size_t>(std::max(
        1, optionInt(object, {"candidateLimit", "candidate_limit"},
                     static_cast<int>(limits.maxCandidates))));
    limits.infinite = optionBool(object, {"infinite"}, false);
    if (limits.timeMs < 50 || limits.timeMs > 30'000) {
        throw ApiError(422, "timeLimitMs 必须在 50 到 30000 之间");
    }
    if (limits.maxDepth < 1 || limits.maxDepth > 16) {
        throw ApiError(422, "maxDepth 必须在 1 到 16 之间");
    }
    if (limits.maxMatePlies < 1 || limits.maxMatePlies > 17) {
        throw ApiError(422, "threatDepth 必须在 1 到 17 之间");
    }
    if (limits.maxCandidates < 8 || limits.maxCandidates > 96) {
        throw ApiError(422, "candidateLimit 必须在 8 到 96 之间");
    }
    return limits;
}

Move parseMove(const json::Value::Object& object) {
    const json::Value* value = findAny(object, {"move", "point"});
    if (value == nullptr || !value->is_object()) {
        throw ApiError(422, "move 必须是包含 x、y 的对象");
    }
    const auto& moveObject = value->as_object();
    const json::Value* xValue = findAny(moveObject, {"x", "col", "column"});
    const json::Value* yValue = findAny(moveObject, {"y", "row"});
    if (xValue == nullptr || yValue == nullptr) {
        throw ApiError(422, "move 必须同时包含 x 和 y");
    }
    const Move move{integerValue(*xValue, "move.x"), integerValue(*yValue, "move.y")};
    if (!Board::inBounds(move)) {
        throw ApiError(422, "move 坐标必须位于 0 到 14");
    }
    return move;
}

AnalysisJobManager::JobId parseJobId(const json::Value::Object& object) {
    const json::Value* value = findAny(object, {"jobId", "job_id"});
    if (value == nullptr || !value->is_number()) {
        throw ApiError(422, "jobId 必须是正整数");
    }
    try {
        const std::uint64_t id = value->as_uint64();
        if (id == 0) {
            throw ApiError(422, "jobId 必须是正整数");
        }
        return id;
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception&) {
        throw ApiError(422, "jobId 必须是正整数");
    }
}

std::uint64_t parseAfterVersion(const json::Value::Object& object) {
    const json::Value* value = findAny(object, {"version", "afterVersion", "after_version"});
    if (value == nullptr) {
        return 0;
    }
    if (!value->is_number()) {
        throw ApiError(422, "version 必须是非负整数");
    }
    try {
        return value->as_uint64();
    } catch (const std::exception&) {
        throw ApiError(422, "version 必须是非负整数");
    }
}

std::string stoneName(Stone stone) {
    if (stone == Stone::Black) {
        return "black";
    }
    if (stone == Stone::White) {
        return "white";
    }
    return "none";
}

json::Value moveJson(Move move) {
    json::Value::Object value;
    value.emplace("x", move.x);
    value.emplace("y", move.y);
    return json::Value(std::move(value));
}

double candidateProbability(int score) {
    const double bounded = std::clamp(static_cast<double>(score), -17'500.0, 17'500.0);
    return 1.0 / (1.0 + std::exp(-bounded / 4'500.0));
}

json::Value analysisJson(const AnalysisResult& result, Stone rootSide) {
    json::Value::Object response;
    response.emplace("kind", result.kind);
    response.emplace("proven", result.proven);
    response.emplace("terminal", result.terminal);
    response.emplace("winner", result.winner == Stone::Empty
                                   ? json::Value(nullptr)
                                   : json::Value(stoneName(result.winner)));
    response.emplace("mateIn", result.matePlies.has_value()
                                   ? json::Value(*result.matePlies)
                                   : json::Value(nullptr));
    response.emplace("bestMove", result.bestMove.has_value()
                                    ? moveJson(*result.bestMove)
                                    : json::Value(nullptr));
    response.emplace("score", result.score);
    response.emplace("winRateScore", result.winRateScore);
    response.emplace("scorePerspective", stoneName(rootSide));
    response.emplace("blackWinRate", result.blackWinRate);
    response.emplace("whiteWinRate", result.whiteWinRate);
    response.emplace("drawRate", result.drawRate);
    response.emplace("proof", result.proof.empty() ? json::Value(nullptr)
                                                    : json::Value(result.proof));

    if (result.kind == "forced_mate" && result.matePlies.has_value()) {
        const std::string winner = result.winner == Stone::Black ? "黑方" : "白方";
        response.emplace("message", winner + "已证明在 " +
                                        std::to_string(*result.matePlies) +
                                        " 个总落子内完成杀棋");
    } else if (result.kind == "draw") {
        response.emplace("message", "棋盘已满，局面为和棋");
    } else {
        response.emplace("message", "胜率由相邻搜索深度的中心评分映射，仅作为启发式估计");
    }

    json::Value::Array pv;
    pv.reserve(result.principalVariation.size());
    for (Move move : result.principalVariation) {
        pv.push_back(moveJson(move));
    }
    response.emplace("pv", json::Value(std::move(pv)));

    json::Value::Array candidates;
    candidates.reserve(result.candidates.size());
    int rank = 1;
    for (const CandidateResult& candidate : result.candidates) {
        json::Value::Object item;
        item.emplace("rank", rank++);
        item.emplace("move", moveJson(candidate.move));
        item.emplace("score", candidate.score);
        const int winRateScore = candidate.winRateScore.value_or(candidate.score);
        item.emplace("winRateScore", winRateScore);
        item.emplace("winRate", candidateProbability(winRateScore));
        json::Value::Array candidatePv;
        candidatePv.reserve(candidate.principalVariation.size());
        for (Move move : candidate.principalVariation) {
            candidatePv.push_back(moveJson(move));
        }
        item.emplace("pv", json::Value(std::move(candidatePv)));
        candidates.emplace_back(std::move(item));
    }
    response.emplace("candidates", json::Value(std::move(candidates)));

    json::Value::Object stats;
    stats.emplace("depth", result.stats.completedDepth);
    stats.emplace("completedDepth", result.stats.completedDepth);
    stats.emplace("nodes", result.stats.nodes + result.stats.forcingNodes);
    stats.emplace("alphaBetaNodes", result.stats.nodes);
    stats.emplace("forcingNodes", result.stats.forcingNodes);
    stats.emplace("elapsedMs", result.stats.elapsedMs);
    stats.emplace("timedOut", result.stats.timedOut);
    stats.emplace("cancelled", result.stats.cancelled);
    stats.emplace("cutoffs", result.stats.betaCutoffs);
    stats.emplace("ttHits", result.stats.transpositionHits);
    stats.emplace("nps", result.stats.nodesPerSecond);
    response.emplace("stats", json::Value(std::move(stats)));
    return json::Value(std::move(response));
}

std::pair<std::string, std::string> forbiddenDescription(ForbiddenReason reason) {
    switch (reason) {
        case ForbiddenReason::None: return {"none", "合法落点"};
        case ForbiddenReason::OutOfBounds: return {"out_of_bounds", "坐标越界"};
        case ForbiddenReason::Occupied: return {"occupied", "该点已有棋子"};
        case ForbiddenReason::Overline: return {"overline", "禁手：长连"};
        case ForbiddenReason::DoubleFour: return {"double_four", "禁手：四四"};
        case ForbiddenReason::DoubleThree: return {"double_three", "禁手：三三"};
    }
    return {"unknown", "未知规则结果"};
}

HttpResponse apiError(const ApiError& error) {
    json::Value::Object body;
    body.emplace("error", "invalid_request");
    body.emplace("message", error.what());
    return HttpResponse::json(error.status(), json::Value(std::move(body)));
}

HttpResponse handleAnalyze(const HttpRequest& request, const json::Value& body) {
    try {
        PositionInput position = parsePosition(body);
        const auto winners = Rules::winners(position.board, position.rules);
        if (winners.both()) {
            throw ApiError(422, "局面中黑白双方同时存在胜线，无法进行有效分析");
        }
        AnalysisSlot slot;
        if (!slot.acquired()) {
            json::Value::Object response;
            response.emplace("error", "analysis_busy");
            response.emplace("message", "分析任务已满，请稍后重试");
            return HttpResponse::json(429, json::Value(std::move(response)));
        }
        const Stone rootSide = position.board.sideToMove();
        SearchLimits limits = parseLimits(body);
        if (limits.infinite) {
            throw ApiError(422, "无限分析请使用 /api/analyze/start");
        }
        limits.cancelRequested = request.client_disconnected;
        Analyzer analyzer;
        const AnalysisResult result = analyzer.analyze(
            std::move(position.board), position.rules, limits);
        return HttpResponse::json(200, analysisJson(result, rootSide));
    } catch (const ApiError& error) {
        return apiError(error);
    } catch (const std::exception& error) {
        json::Value::Object response;
        response.emplace("error", "analysis_failed");
        response.emplace("message", error.what());
        return HttpResponse::json(500, json::Value(std::move(response)));
    }
}

HttpResponse handleAnalyzeStart(
    AnalysisJobManager& jobs, const HttpRequest&, const json::Value& body) {
    try {
        PositionInput position = parsePosition(body);
        if (Rules::winners(position.board, position.rules).both()) {
            throw ApiError(422, "局面中黑白双方同时存在胜线，无法进行有效分析");
        }
        SearchLimits limits = parseLimits(body);
        limits.cancelRequested = {};
        const auto started = jobs.start(
            std::move(position.board), position.rules, std::move(limits));
        if (started.busy || !started.jobId.has_value()) {
            json::Value::Object response;
            response.emplace("error", "analysis_busy");
            response.emplace("message", "分析任务已满，请稍后重试");
            return HttpResponse::json(429, json::Value(std::move(response)));
        }

        json::Value::Object response;
        response.emplace("jobId", *started.jobId);
        response.emplace("running", true);
        response.emplace("version", 0);
        return HttpResponse::json(202, json::Value(std::move(response)));
    } catch (const ApiError& error) {
        return apiError(error);
    }
}

HttpResponse handleAnalyzePoll(
    AnalysisJobManager& jobs, const HttpRequest&, const json::Value& body) {
    try {
        const auto& object = requireObject(body, "请求体");
        const auto polled = jobs.poll(parseJobId(object), parseAfterVersion(object));
        if (!polled.found) {
            json::Value::Object response;
            response.emplace("error", "job_not_found");
            response.emplace("message", "分析任务不存在或已过期");
            return HttpResponse::json(404, json::Value(std::move(response)));
        }

        json::Value::Object response;
        response.emplace("running", polled.running);
        response.emplace("changed", polled.changed);
        response.emplace("failed", polled.failed);
        response.emplace("version", polled.version);
        response.emplace("error", polled.error.empty()
                                      ? json::Value(nullptr)
                                      : json::Value(polled.error));
        response.emplace("result", polled.latest.has_value()
                                       ? analysisJson(*polled.latest, polled.rootSide)
                                       : json::Value(nullptr));
        return HttpResponse::json(200, json::Value(std::move(response)));
    } catch (const ApiError& error) {
        return apiError(error);
    }
}

HttpResponse handleAnalyzeStop(
    AnalysisJobManager& jobs, const HttpRequest&, const json::Value& body) {
    try {
        const auto& object = requireObject(body, "请求体");
        const AnalysisJobManager::JobId id = parseJobId(object);
        const auto current = jobs.poll(id, 0);
        if (!current.found) {
            json::Value::Object response;
            response.emplace("error", "job_not_found");
            response.emplace("message", "分析任务不存在或已过期");
            return HttpResponse::json(404, json::Value(std::move(response)));
        }
        const bool stopped = jobs.stop(id);
        json::Value::Object response;
        response.emplace("stopped", stopped);
        response.emplace("running", current.running && !stopped);
        return HttpResponse::json(200, json::Value(std::move(response)));
    } catch (const ApiError& error) {
        return apiError(error);
    }
}

HttpResponse handleForbidden(const HttpRequest&, const json::Value& body) {
    try {
        PositionInput position = parsePosition(body);
        const auto& object = body.as_object();
        const Move move = parseMove(object);
        Stone stone = position.board.sideToMove();
        if (const json::Value* value = findAny(object, {"stone", "color"}); value != nullptr) {
            json::Value::Object sideObject;
            sideObject.emplace("sideToMove", *value);
            stone = parseSide(sideObject);
        }
        const ForbiddenReason reason = Rules::forbiddenReason(
            position.board, move, stone, position.rules);
        const auto [type, description] = forbiddenDescription(reason);
        const bool ruleForbidden = reason == ForbiddenReason::Overline ||
                                   reason == ForbiddenReason::DoubleFour ||
                                   reason == ForbiddenReason::DoubleThree;

        json::Value::Object response;
        response.emplace("forbidden", ruleForbidden);
        response.emplace("legal", reason == ForbiddenReason::None);
        response.emplace("type", type);
        response.emplace("reason", description);
        response.emplace("winning", Rules::isWinningPlacement(
            position.board, move, stone, position.rules));
        return HttpResponse::json(200, json::Value(std::move(response)));
    } catch (const ApiError& error) {
        return apiError(error);
    }
}

struct CommandLine {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 8080;
    std::filesystem::path webRoot = GOMOKU_DEFAULT_WEB_ROOT;
};

std::uint16_t parsePort(std::string_view value) {
    unsigned port = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), port);
    if (error != std::errc{} || end != value.data() + value.size() || port > 65'535) {
        throw std::invalid_argument("端口必须是 0 到 65535 的整数");
    }
    return static_cast<std::uint16_t>(port);
}

CommandLine parseArguments(int argc, char** argv) {
    CommandLine command;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto requireValue = [&](std::string_view option) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(option) + " 缺少参数");
            }
            return argv[++index];
        };
        if (argument == "--port") {
            command.port = parsePort(requireValue(argument));
        } else if (argument == "--bind") {
            command.bindAddress = requireValue(argument);
        } else if (argument == "--web-root") {
            command.webRoot = requireValue(argument);
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "用法: gomoku_server [--bind 127.0.0.1] [--port 8080] "
                         "[--web-root ./web]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("未知参数: " + std::string(argument));
        }
    }
    return command;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const CommandLine command = parseArguments(argc, argv);
        HttpServerOptions options;
        options.bind_address = command.bindAddress;
        options.port = command.port;
        options.io_timeout = std::chrono::milliseconds(35'000);

        AnalysisJobManager jobs;
        HttpServer server(options);
        server.add_post_route("/api/analyze", handleAnalyze);
        server.add_post_route(
            "/api/analyze/start",
            [&jobs](const HttpRequest& request, const json::Value& body) {
                return handleAnalyzeStart(jobs, request, body);
            });
        server.add_post_route(
            "/api/analyze/poll",
            [&jobs](const HttpRequest& request, const json::Value& body) {
                return handleAnalyzePoll(jobs, request, body);
            });
        server.add_post_route(
            "/api/analyze/stop",
            [&jobs](const HttpRequest& request, const json::Value& body) {
                return handleAnalyzeStop(jobs, request, body);
            });
        server.add_post_route("/api/forbidden", handleForbidden);
        server.add_static_file("/", command.webRoot / "index.html");
        server.add_static_file("/index.html", command.webRoot / "index.html");
        server.add_static_file("/styles.css", command.webRoot / "styles.css");
        server.add_static_file("/app.js", command.webRoot / "app.js");

        server.start();
        std::cout << "Gomoku analyzer listening on http://" << command.bindAddress
                  << ':' << server.bound_port() << '\n';
        std::cout.flush();
        server.wait();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gomoku_server: " << error.what() << '\n';
        return 1;
    }
}
