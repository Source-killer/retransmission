#pragma once

// Names must be string literals. Disabled builds have no timing overhead.
// Stamp and Interval change size with TR_ENABLE_BLOCK_PROFILING, which only
// libtransmission's own sources define. Keep them out of public headers.
#ifdef TR_ENABLE_BLOCK_PROFILING

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tr::profile
{
using Clock = std::chrono::steady_clock;

struct Totals {
    uint64_t calls = 0;
    int64_t total_ns = 0;
    int64_t self_ns = 0;
    int64_t min_ns = std::numeric_limits<int64_t>::max();
    int64_t max_ns = 0;
};

class Collector
{
public:
    Collector()
    {
        if (auto const* path = std::getenv("TR_PROFILE_OUTPUT"); path != nullptr) {
            path_ = path;
        }
    }

    ~Collector()
    {
        if (path_.empty()) {
            return;
        }
        auto* out = std::fopen(path_.c_str(), "w");
        if (out == nullptr) {
            std::fprintf(stderr, "Cannot write block profile: %s\n", path_.c_str());
            return;
        }
        // os_thread matches /proc/<pid>/task/<tid> on Linux and is 0 elsewhere.
        std::fputs("thread,scope,calls,total_ms,self_ms,min_ms,max_ms,os_thread\n", out);
        for (auto const& [key, value] : totals_) {
            std::fprintf(out, "%zu,\"", key.first);
            for (char const c : key.second) {
                std::fputc(c, out);
                if (c == '"') {
                    std::fputc(c, out);
                }
            }
            std::fprintf(out, "\",%llu", static_cast<unsigned long long>(value.calls));
            for (auto const ns : { value.total_ns, value.self_ns, value.min_ns, value.max_ns }) {
                write_ms(out, ns);
            }
            std::fprintf(out, ",%lld\n", static_cast<long long>(os_threads_[key.first]));
        }
        if (std::fclose(out) != 0) {
            std::fprintf(stderr, "Cannot finish block profile: %s\n", path_.c_str());
        }
    }

    static Collector& instance()
    {
        static auto collector = Collector{};
        return collector;
    }

    bool enabled() const noexcept
    {
        return !path_.empty();
    }

    void record(char const* name, int64_t total, int64_t self)
    {
        auto const lock = std::scoped_lock{ mutex_ };
        thread_local auto const thread = register_thread();
        auto& row = totals_[{ thread, name }];
        ++row.calls;
        row.total_ns += total;
        row.self_ns += self;
        row.min_ns = std::min(row.min_ns, total);
        row.max_ns = std::max(row.max_ns, total);
    }

private:
    // Integer arithmetic only: clients set a locale, so %f could print a
    // decimal comma and split the CSV field.
    static void write_ms(std::FILE* out, int64_t const ns)
    {
        std::fprintf(out, ",%lld.%06lld", static_cast<long long>(ns / 1000000), static_cast<long long>(ns % 1000000));
    }

    // Call with mutex_ held.
    size_t register_thread()
    {
#ifdef __linux__
        os_threads_.push_back(static_cast<int64_t>(syscall(SYS_gettid)));
#else
        os_threads_.push_back(0);
#endif
        return std::size(os_threads_) - 1U;
    }

    std::string path_;
    std::mutex mutex_;
    std::vector<int64_t> os_threads_;
    std::map<std::pair<size_t, std::string_view>, Totals> totals_;
};

class Scope
{
public:
    explicit Scope(char const* name)
        : name_{ name }
    {
        if (Collector::instance().enabled()) {
            parent_ = current_;
            current_ = this;
            active_ = true;
            start_ = Clock::now();
        }
    }

    ~Scope() noexcept
    {
        if (!active_) {
            return;
        }
        auto const end = Clock::now();
        auto const total = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        current_ = parent_;
        try {
            Collector::instance().record(name_, total, std::max<int64_t>(0, total - children_ns_));
        } catch (...) {
            // Profiling must not turn a completed operation into a failure.
        }
        if (parent_ != nullptr) {
            // Exclude this scope's bookkeeping from its parent's self time.
            parent_->children_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
        }
    }

    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;

private:
    inline static thread_local Scope* current_ = nullptr;
    char const* name_;
    Scope* parent_ = nullptr;
    Clock::time_point start_{};
    int64_t children_ns_ = 0;
    bool active_ = false;
};
// Records a duration measured outside a lexical scope.
inline void record(char const* name, int64_t const ns) noexcept
{
    if (!Collector::instance().enabled()) {
        return;
    }
    try {
        Collector::instance().record(name, ns, ns);
    } catch (...) {
        // Profiling must not turn a completed operation into a failure.
    }
}

// A creation time, kept so a later step can record how long an item
// waited, e.g. in a queue or for another thread.
class Stamp
{
public:
    void record_since(char const* name) const noexcept
    {
        if (taken_ != Clock::time_point{}) {
            record(name, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - taken_).count());
        }
    }

private:
    Clock::time_point taken_ = Collector::instance().enabled() ? Clock::now() : Clock::time_point{};
};

// Times how long a condition holds: from the first start() to the next
// stop(). A condition that is still holding at exit is not recorded.
class Interval
{
public:
    explicit Interval(char const* name) noexcept
        : name_{ name }
    {
    }

    void start() noexcept
    {
        if (!since_ && Collector::instance().enabled()) {
            since_ = Clock::now();
        }
    }

    void stop() noexcept
    {
        if (since_) {
            record(name_, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - *since_).count());
            since_.reset();
        }
    }

private:
    char const* name_;
    std::optional<Clock::time_point> since_;
};
} // namespace tr::profile

#define TR_PROFILE_JOIN_IMPL(a, b) a##b
#define TR_PROFILE_JOIN(a, b) TR_PROFILE_JOIN_IMPL(a, b)
#define TR_PROFILE_SCOPE(name) \
    auto const TR_PROFILE_JOIN(tr_profile_scope_, __LINE__) = tr::profile::Scope \
    { \
        name \
    }

#else
namespace tr::profile
{
class Stamp
{
public:
    void record_since(char const* /*name*/) const noexcept
    {
    }
};

class Interval
{
public:
    explicit constexpr Interval(char const* /*name*/) noexcept
    {
    }

    void start() noexcept
    {
    }

    void stop() noexcept
    {
    }
};
} // namespace tr::profile

#define TR_PROFILE_SCOPE(name) static_cast<void>(0)
#endif
