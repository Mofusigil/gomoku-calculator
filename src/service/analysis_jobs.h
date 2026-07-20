#pragma once

#include "engine/search.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace gomoku {

class AnalysisJobManager final {
public:
    using JobId = std::uint64_t;

    static constexpr std::size_t kMaxRunningJobs = 4;

    struct StartResult {
        std::optional<JobId> jobId;
        bool busy = false;
    };

    struct PollResult {
        bool found = false;
        bool running = false;
        bool changed = false;
        bool failed = false;
        std::uint64_t version = 0;
        Stone rootSide = Stone::Empty;
        std::optional<AnalysisResult> latest;
        std::string error;
    };

    explicit AnalysisJobManager(
        std::chrono::milliseconds completedRetention = std::chrono::minutes(5),
        std::chrono::milliseconds infiniteLease = std::chrono::seconds(30));
    ~AnalysisJobManager();

    AnalysisJobManager(const AnalysisJobManager&) = delete;
    AnalysisJobManager& operator=(const AnalysisJobManager&) = delete;
    AnalysisJobManager(AnalysisJobManager&&) = delete;
    AnalysisJobManager& operator=(AnalysisJobManager&&) = delete;

    [[nodiscard]] StartResult start(
        Board board, RuleSet rules, SearchLimits limits = {});

    // The latest result is copied only when its version is newer than afterVersion.
    // Polling also renews the activity lease of an infinite analysis.
    [[nodiscard]] PollResult poll(JobId id, std::uint64_t afterVersion = 0);

    // Returns true only when a running job was found and cancellation was requested.
    bool stop(JobId id);

    // Removes completed jobs whose retention period has elapsed.
    void cleanup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gomoku
