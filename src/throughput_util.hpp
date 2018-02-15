#pragma once
#include <chrono>
#include <deque>
#include <algorithm>
#include <thread>
#include "cybozu/exception.hpp"

/**
 * You can get moving average throughput using this class.
 */
class ThroughputMonitor
{
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::milliseconds;

    static const size_t INTERVAL_MS = 10;
    static const size_t MIN_MS = 900;
    static const size_t MAX_MS = 1100;

    struct Record {
        typename Clock::time_point timePoint;
        uint64_t progressLb;

        bool operator<(const Record& rhs) const {
            return timePoint < rhs.timePoint;
        }
    };

    uint64_t progressLb_;
    std::deque<Record> deq_;

public:
    ThroughputMonitor() : progressLb_(0), deq_() {
    }
    /**
     * sizeLb: added size [logical block].
     * RETURN:
     *   current throughput [logical block per sec].
     */
    uint64_t addAndGetLbPerSec(uint64_t sizeLb) {
        progressLb_ += sizeLb;
        typename Clock::time_point now = Clock::now();
        if (deq_.empty() ||
            castToMs(now - deq_.back().timePoint) > INTERVAL_MS) {
            deq_.push_back({now, progressLb_});
        }
        gc(now);
        return getLbPerSecDetail(now);
    }
    uint64_t getLbPerSec() {
        typename Clock::time_point now = Clock::now();
        gc(now);
        return getLbPerSecDetail(now);
    }
private:
    template <typename Rep, typename Period>
    static size_t castToMs(const std::chrono::duration<Rep, Period>& duration) {
        return std::chrono::duration_cast<Ms>(duration).count();
    }
    void gc(const Clock::time_point& now) {
        if (deq_.empty() || castToMs(now - deq_.front().timePoint) < MAX_MS) {
            return;
        }
        const size_t ms = MIN_MS; // to avoid undefined reference.
        const Record rec{now - Ms(ms), 0 /* unused */};
        deq_.erase(deq_.begin(), std::lower_bound(deq_.begin(), deq_.end(), rec));
    }
    uint64_t getLbPerSecDetail(const Clock::time_point& now) const {
        if (deq_.size() <= 1) return 0;
        const size_t ms = castToMs(now - deq_.front().timePoint);
        if (ms == 0) return 0;
        const uint64_t sizeLb = progressLb_ - deq_.front().progressLb;
        return sizeLb * 1000 / ms;
    }
};

/**
 * Use this to keep throughput around a specified value.
 */
class ThroughputStabilizer
{
    uint64_t maxLbPerSec_;
    ThroughputMonitor thMon_;

public:
    ThroughputStabilizer()
        : maxLbPerSec_(0), thMon_() {
    }
    void setMaxLbPerSec(uint64_t maxLbPerSec) {
        maxLbPerSec_ = maxLbPerSec;
    }
    void addAndSleepIfNecessary(uint64_t sizeLb, size_t sleepMs, size_t maxSleepMs) {
        if (maxLbPerSec_ == 0) return;
        uint64_t lbPerSec = thMon_.addAndGetLbPerSec(sizeLb);
        size_t total = 0;
        while (lbPerSec > maxLbPerSec_ && total < maxSleepMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            total += sleepMs;
            lbPerSec = thMon_.getLbPerSec();
        }
    }
};


/**
 * This class is used for throughput controller.
 * You can specify a permille of max throughput.
 */
class Sleeper
{
    size_t sleepPermille_; // 0 to 999.
    size_t minMs_;
    size_t maxMs_;
    double baseTs_; // unix timestamp.
    size_t totalSleepMs_;

    static const size_t MONITOR_MS = 10000;

public:
    /**
     * sleepPermille: specify from 0 to 999.
     * minMs: minimum sleeping period at once [ms].
     * maxMs: maximum sleeping period at once [ms].
     * ts: current timestamp. monotonic timer is preferred.
     */
    void init(size_t sleepPermille, size_t minMs, size_t maxMs, double ts) {
        if (minMs == 0) throw cybozu::Exception("Sleeper:minMs must not be 0");
        if (maxMs == 0) throw cybozu::Exception("Sleeper:maxMs must not be 0");
        if (minMs > maxMs) {
            throw cybozu::Exception("Sleeper:bad minMs and maxMs") << minMs << maxMs;
        }
        if (sleepPermille >= 1000) {
            throw cybozu::Exception("Sleeper:bad sleepPerMille") << sleepPermille;
        }
        sleepPermille_ = sleepPermille;
        minMs_ = minMs;
        maxMs_ = maxMs;
        baseTs_ = ts;
        totalSleepMs_ = 0;
    }
    /**
     * ts: current timestamp. monotonic timer is preferred.
     */
    size_t sleepIfNecessary(double ts) {
        if (sleepPermille_ == 0) return 0; // fast pass.
        size_t sleepMs = 0;
        size_t elapsedMs = (ts - baseTs_) * 1000;
        /*
         * (totalSleepMs_ + minMs_) / elapsedMs < sleepPermille_ / 1000
         */
        if ((totalSleepMs_ + minMs_) * 1000 < sleepPermille_ * elapsedMs) {
            /*
             * Decide sleeping period.
             * (totalSleepMs_ + sleepMs) / elapsedMs == sleepPermille_ / 1000
             */
            sleepMs = sleepPermille_ * elapsedMs / 1000 - totalSleepMs_;
            assert(minMs_ <= sleepMs);
            sleepMs = std::min(sleepMs, maxMs_);

            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

            totalSleepMs_ += sleepMs;
            elapsedMs += sleepMs;
        }
        if (elapsedMs > MONITOR_MS * 2) {
            baseTs_ += (elapsedMs / 2) / 1000.0;
            totalSleepMs_ /= 2;
        }

        return sleepMs;
    }
};
