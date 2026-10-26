#ifndef slic3r_TraceryPerformance_hpp_
#define slic3r_TraceryPerformance_hpp_

#include <cstddef>

#ifndef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

#endif

namespace Slic3r {

#ifndef NDEBUG

// Opt-in diagnostics only. Parent durations include children; do not sum levels.
// Loop samples are accumulated and emitted once, not logged per triangle.
class TraceryPerformanceScope
{
public:
    explicit TraceryPerformanceScope(const char* phase) : m_phase(phase)
    {
        const char* setting = std::getenv("TRACERY_PERF_TRACE");
        m_enabled = setting != nullptr && std::strcmp(setting, "1") == 0;
        if (m_enabled) {
            m_parent = s_active;
            m_id = ++s_next_id;
            m_exceptions = std::uncaught_exceptions();
            m_begin = Clock::now();
            s_active = this;
        }
    }
    TraceryPerformanceScope(const TraceryPerformanceScope&) = delete;
    TraceryPerformanceScope& operator=(const TraceryPerformanceScope&) = delete;
    ~TraceryPerformanceScope() { finish(); }

    bool enabled() const { return m_enabled; }
    unsigned long long id() const { return m_id; }
    void mark_failed() const
    {
        for (auto* scope = this; scope; scope = scope->m_parent)
            scope->m_failed = true;
    }
    void count(const char* name, size_t value) const
    {
        if (m_enabled)
            std::fprintf(stderr, "TRACERY_UI_PERF scope_id=%llu metric=%s value=%zu\n", m_id, name, value);
    }
    void stage(const char* phase)
    {
        if (!m_enabled)
            return;
        end_stage(false);
        m_stage = phase;
        m_stage_begin = Clock::now();
    }
    // Record a batch under this scope, e.g. all per-facet triangulations.
    class Sample
    {
    public:
        Sample(TraceryPerformanceScope& owner, const char* phase) : m_owner(owner), m_phase(phase)
        {
            if (owner.enabled()) {
                m_begin = Clock::now();
                m_exceptions = std::uncaught_exceptions();
            }
        }
        ~Sample()
        {
            if (m_owner.enabled())
                m_owner.record(m_phase, milliseconds(m_begin), std::uncaught_exceptions() > m_exceptions,
                               m_owner.m_stage ? m_owner.m_stage : m_owner.m_phase);
        }
        Sample(const Sample&) = delete;
        Sample& operator=(const Sample&) = delete;
    private:
        TraceryPerformanceScope& m_owner;
        const char* m_phase;
        std::chrono::steady_clock::time_point m_begin;
        int m_exceptions = 0;
    };
    void finish()
    {
        if (!m_enabled)
            return;
        const bool exception = std::uncaught_exceptions() > m_exceptions;
        const double elapsed = milliseconds(m_begin);
        end_stage(exception);
        if (exception)
            mark_failed();
        for (const Entry& entry : m_entries)
            std::fprintf(stderr,
                "TRACERY_UI_PERF phase=%s elapsed_ms=%.3f scope_id=%llu parent_id=%llu "
                "calls=%zu status=%s kind=detail parent_phase=%s\n",
                entry.phase, entry.ms, m_id, m_parent ? m_parent->m_id : 0, entry.calls,
                entry.failed ? "exception" : "completed", entry.parent_phase);
        std::fprintf(stderr,
            "TRACERY_UI_PERF phase=%s elapsed_ms=%.3f scope_id=%llu parent_id=%llu status=%s kind=total\n",
            m_phase, elapsed, m_id, m_parent ? m_parent->m_id : 0, exception ? "exception" : (m_failed ? "failed" : "completed"));
        if (s_active == this)
            s_active = m_parent;
        m_enabled = false;
    }
private:
    using Clock = std::chrono::steady_clock;
    struct Entry
    {
        const char* phase;
        double ms;
        size_t calls;
        bool failed;
        const char* parent_phase;
    };
    static double milliseconds(Clock::time_point begin)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    }
    void record(const char* phase, double ms, bool failed, const char* parent_phase)
    {
        for (Entry& entry : m_entries)
            if (std::strcmp(entry.phase, phase) == 0 && std::strcmp(entry.parent_phase, parent_phase) == 0) {
                entry.ms += ms;
                ++entry.calls;
                entry.failed |= failed;
                return;
            }
        m_entries.push_back({phase, ms, 1, failed, parent_phase});
    }
    void end_stage(bool failed)
    {
        if (m_stage)
            record(m_stage, milliseconds(m_stage_begin), failed, m_phase);
        m_stage = nullptr;
    }
    inline static std::atomic<unsigned long long> s_next_id{0};
    inline static thread_local TraceryPerformanceScope* s_active = nullptr;
    const char* m_phase;
    const char* m_stage = nullptr;
    bool m_enabled = false;
    mutable bool m_failed = false;
    int m_exceptions = 0;
    unsigned long long m_id = 0;
    TraceryPerformanceScope* m_parent = nullptr;
    Clock::time_point m_begin, m_stage_begin;
    std::vector<Entry> m_entries;
};

#else
// Preserve the call-site API without clocks, environment reads or trace state
// in release builds. Guard expensive diagnostic arguments at their call sites.
class TraceryPerformanceScope
{
public:
    explicit constexpr TraceryPerformanceScope(const char*) noexcept {}
    ~TraceryPerformanceScope() noexcept {}
    TraceryPerformanceScope(const TraceryPerformanceScope&) = delete;
    TraceryPerformanceScope& operator=(const TraceryPerformanceScope&) = delete;
    constexpr bool enabled() const noexcept { return false; }
    constexpr unsigned long long id() const noexcept { return 0; }
    void mark_failed() const noexcept {}
    void count(const char*, size_t) const noexcept {}
    void stage(const char*) const noexcept {}
    void finish() const noexcept {}
    class Sample
    {
    public:
        constexpr Sample(TraceryPerformanceScope&, const char*) noexcept {}
        ~Sample() noexcept {}
        Sample(const Sample&) = delete;
        Sample& operator=(const Sample&) = delete;
    };
};
#endif

} // namespace Slic3r
#endif
