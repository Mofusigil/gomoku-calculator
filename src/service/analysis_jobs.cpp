#include "service/analysis_jobs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gomoku {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t clockMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct AnalysisJobManager::Impl {
    struct JobState {
        JobState(bool isInfinite, std::chrono::milliseconds lease, Stone analysisRootSide)
            : infinite(isInfinite),
              leaseMs(lease.count()),
              rootSide(analysisRootSide),
              lastAccessMs(clockMilliseconds()) {}

        std::mutex mutex;
        std::atomic<bool> cancelRequested{false};
        const bool infinite;
        const std::int64_t leaseMs;
        const Stone rootSide;
        std::atomic<std::int64_t> lastAccessMs;
        bool running = true;
        bool failed = false;
        std::uint64_t version = 0;
        std::optional<AnalysisResult> latest;
        std::string error;
        Clock::time_point completedAt{};
    };

    struct Job {
        explicit Job(std::shared_ptr<JobState> jobState)
            : state(std::move(jobState)) {}

        std::shared_ptr<JobState> state;
        std::jthread worker;
    };

    Impl(std::chrono::milliseconds retention, std::chrono::milliseconds lease)
        : completedRetention(std::max(retention, std::chrono::milliseconds::zero())),
          infiniteLease(std::max(lease, std::chrono::milliseconds(1))) {}

    ~Impl() {
        std::unordered_map<JobId, std::unique_ptr<Job>> retiring;
        {
            std::lock_guard lock(jobsMutex);
            shuttingDown = true;
            for (auto& [id, job] : jobs) {
                (void)id;
                job->state->cancelRequested.store(true, std::memory_order_release);
            }
            retiring.swap(jobs);
        }
        // Destroying the jthreads outside jobsMutex waits for every analyzer to exit.
    }

    static bool leaseExpired(const JobState& state, std::int64_t nowMs) noexcept {
        return state.infinite &&
               nowMs - state.lastAccessMs.load(std::memory_order_acquire) >=
                   state.leaseMs;
    }

    static bool shouldCancel(
        const std::shared_ptr<JobState>& state,
        const std::function<bool()>& callerCancellation) {
        if (state->cancelRequested.load(std::memory_order_acquire)) {
            return true;
        }
        if (leaseExpired(*state, clockMilliseconds())) {
            state->cancelRequested.store(true, std::memory_order_release);
            return true;
        }
        if (callerCancellation && callerCancellation()) {
            state->cancelRequested.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    static void publish(
        const std::shared_ptr<JobState>& state, const AnalysisResult& result) {
        std::lock_guard lock(state->mutex);
        if (!state->running) {
            return;
        }
        state->latest = result;
        ++state->version;
    }

    static void complete(
        const std::shared_ptr<JobState>& state, AnalysisResult result) {
        std::lock_guard lock(state->mutex);
        if (result.kind != "cancelled" || !state->latest) {
            state->latest = std::move(result);
        }
        ++state->version;
        state->running = false;
        state->completedAt = Clock::now();
    }

    static void fail(const std::shared_ptr<JobState>& state, std::string error) {
        std::lock_guard lock(state->mutex);
        state->failed = true;
        state->error = std::move(error);
        ++state->version;
        state->running = false;
        state->completedAt = Clock::now();
    }

    void collectExpiredLocked(
        std::vector<std::unique_ptr<Job>>& retiring, Clock::time_point now) {
        for (auto it = jobs.begin(); it != jobs.end();) {
            bool expired = false;
            {
                std::lock_guard stateLock(it->second->state->mutex);
                const JobState& state = *it->second->state;
                expired = !state.running &&
                          now - state.completedAt >= completedRetention;
            }
            if (!expired) {
                ++it;
                continue;
            }
            retiring.push_back(std::move(it->second));
            it = jobs.erase(it);
        }
    }

    std::size_t runningJobsLocked() const {
        std::size_t count = 0;
        for (const auto& [id, job] : jobs) {
            (void)id;
            std::lock_guard stateLock(job->state->mutex);
            count += job->state->running ? 1U : 0U;
        }
        return count;
    }

    JobId allocateIdLocked() {
        do {
            if (nextId == 0) {
                ++nextId;
            }
            const JobId candidate = nextId++;
            if (!jobs.contains(candidate)) {
                return candidate;
            }
        } while (true);
    }

    StartResult start(Board board, RuleSet rules, SearchLimits limits) {
        std::vector<std::unique_ptr<Job>> retiring;
        StartResult result;

        std::lock_guard lock(jobsMutex);
        collectExpiredLocked(retiring, Clock::now());
        if (shuttingDown || runningJobsLocked() >= kMaxRunningJobs) {
            result.busy = true;
            return result;
        }

        const JobId id = allocateIdLocked();
        auto state = std::make_shared<JobState>(
            limits.infinite, infiniteLease, board.sideToMove());
        auto job = std::make_unique<Job>(state);
        Job* insertedJob = job.get();
        jobs.emplace(id, std::move(job));

        try {
            insertedJob->worker = std::jthread(
                [state,
                 board = std::move(board),
                 rules,
                 limits = std::move(limits)]() mutable {
                    std::function<bool()> callerCancellation =
                        std::move(limits.cancelRequested);
                    limits.cancelRequested = [state, callerCancellation]() {
                        return shouldCancel(state, callerCancellation);
                    };

                    try {
                        Analyzer analyzer;
                        AnalysisResult finalResult = analyzer.analyze(
                            std::move(board),
                            rules,
                            limits,
                            [state](const AnalysisResult& intermediate) {
                                publish(state, intermediate);
                            });
                        complete(state, std::move(finalResult));
                    } catch (const std::exception& exception) {
                        fail(state, exception.what());
                    } catch (...) {
                        fail(state, "unknown analysis failure");
                    }
                });
        } catch (...) {
            jobs.erase(id);
            throw;
        }

        result.jobId = id;
        return result;
    }

    PollResult poll(JobId id, std::uint64_t afterVersion) {
        cleanup();

        std::shared_ptr<JobState> state;
        {
            std::lock_guard lock(jobsMutex);
            const auto it = jobs.find(id);
            if (it == jobs.end()) {
                return {};
            }
            state = it->second->state;
        }

        const std::int64_t nowMs = clockMilliseconds();
        const std::int64_t previousAccess =
            state->lastAccessMs.exchange(nowMs, std::memory_order_acq_rel);
        if (state->infinite && nowMs - previousAccess >= state->leaseMs) {
            state->cancelRequested.store(true, std::memory_order_release);
        }

        PollResult result;
        std::lock_guard lock(state->mutex);
        result.found = true;
        result.running = state->running;
        result.failed = state->failed;
        result.version = state->version;
        result.rootSide = state->rootSide;
        result.changed = state->version > afterVersion;
        if (result.changed && state->latest) {
            result.latest = state->latest;
        }
        result.error = state->error;
        return result;
    }

    bool stop(JobId id) {
        cleanup();

        std::shared_ptr<JobState> state;
        {
            std::lock_guard lock(jobsMutex);
            const auto it = jobs.find(id);
            if (it == jobs.end()) {
                return false;
            }
            state = it->second->state;
        }

        std::lock_guard lock(state->mutex);
        if (!state->running) {
            return false;
        }
        state->cancelRequested.store(true, std::memory_order_release);
        return true;
    }

    void cleanup() {
        std::vector<std::unique_ptr<Job>> retiring;
        {
            std::lock_guard lock(jobsMutex);
            collectExpiredLocked(retiring, Clock::now());
        }
        // Retired jthreads are joined after releasing jobsMutex.
    }

    const std::chrono::milliseconds completedRetention;
    const std::chrono::milliseconds infiniteLease;
    std::mutex jobsMutex;
    std::unordered_map<JobId, std::unique_ptr<Job>> jobs;
    JobId nextId = 1;
    bool shuttingDown = false;
};

AnalysisJobManager::AnalysisJobManager(
    std::chrono::milliseconds completedRetention,
    std::chrono::milliseconds infiniteLease)
    : impl_(std::make_unique<Impl>(completedRetention, infiniteLease)) {}

AnalysisJobManager::~AnalysisJobManager() = default;

AnalysisJobManager::StartResult AnalysisJobManager::start(
    Board board, RuleSet rules, SearchLimits limits) {
    return impl_->start(std::move(board), rules, std::move(limits));
}

AnalysisJobManager::PollResult AnalysisJobManager::poll(
    JobId id, std::uint64_t afterVersion) {
    return impl_->poll(id, afterVersion);
}

bool AnalysisJobManager::stop(JobId id) {
    return impl_->stop(id);
}

void AnalysisJobManager::cleanup() {
    impl_->cleanup();
}

}  // namespace gomoku
