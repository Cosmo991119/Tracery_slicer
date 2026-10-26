#ifndef slic3r_TraceryInteractionTiming_hpp_
#define slic3r_TraceryInteractionTiming_hpp_

#ifndef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include "TraceryPerformance.hpp"

#endif

namespace Slic3r {

#ifndef NDEBUG

// GUI-thread milestones across the stroke, deferred update, first displayed
// frame and a subsequent idle callback. This measures an event-loop readiness
// proxy, not the operating system's cursor state or completion of slicing.
class TraceryInteractionTiming
{
public:
    using Token = unsigned long long;
    static Token begin(const void* canvas, Token stroke_scope)
    {
        const char* enabled = std::getenv("TRACERY_PERF_TRACE");
        if (!enabled || std::strcmp(enabled, "1") != 0)
            return 0;
        if (state().id)
            close("superseded");
        static std::atomic<Token> next{0};
        state() = Pending{};
        state().id = ++next;
        state().stroke_scope = stroke_scope;
        state().canvas = canvas;
        state().begin = Clock::now();
        emit("stroke_started", "pending");
        return state().id;
    }
    static void generation_finished(Token token)
    {
        if (!matches(token)) return;
        state().generation_finished = true;
        emit("stroke_finished", "pending");
    }
    static void background_scheduled()
    {
        if (!state().id) return;
        state().background_pending = true;
        emit("background_request_posted", "pending");
    }
    static bool after_generation(const void* canvas = nullptr)
    {
        return state().id && state().generation_finished && (!canvas || state().canvas == canvas);
    }
    static void milestone(const char* phase)
    {
        if (state().id) emit(phase, "pending");
    }

    // Only instrument callbacks in the gap under investigation. The existing
    // performance scope supplies nesting IDs; start_us places it on the same
    // timeline as the interaction milestones, without timing every idle frame.
    class Activity
    {
    public:
        Activity(const char* phase, const void* canvas = nullptr)
        {
            if (after_generation(canvas)) {
                m_performance.emplace(phase);
                m_performance->count("interaction_id", state().id);
                m_performance->count("interaction_start_us", size_t(
                    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - state().begin).count()));
            }
        }
        void stage(const char* phase) { if (m_performance) m_performance->stage(phase); }
    private:
        std::optional<TraceryPerformanceScope> m_performance;
    };
    static Token background_started()
    {
        if (!state().id || !state().generation_finished || !state().background_pending || state().background_running)
            return 0;
        state().background_running = true;
        emit("background_started", "pending");
        return state().id;
    }
    static void background_finished(Token token)
    {
        if (!matches(token)) return;
        state().background_running = false;
        state().background_pending = false;
        emit("background_finished", "pending");
    }
    static void frame_presented(const void* canvas)
    {
        if (!state().id || state().canvas != canvas || state().frame_presented) return;
        state().frame_presented = true;
        emit("first_frame_presented", "pending");
    }
    static void idle(const void* canvas)
    {
        if (state().id && state().canvas == canvas && state().generation_finished && state().frame_presented &&
            !state().background_pending && !state().background_running)
            close("completed");
    }
    static void fail(Token token)
    {
        if (matches(token)) close("failed");
    }
    static void fail_current() { fail(state().id); }

    class Stroke
    {
    public:
        Stroke(const void* canvas, Token scope) : m_token(begin(canvas, scope)), m_exceptions(std::uncaught_exceptions()) {}
        ~Stroke()
        {
            if (std::uncaught_exceptions() > m_exceptions) fail(m_token);
            else generation_finished(m_token);
        }
        Stroke(const Stroke&) = delete;
        Stroke& operator=(const Stroke&) = delete;
    private:
        Token m_token;
        int m_exceptions;
    };
private:
    using Clock = std::chrono::steady_clock;
    struct Pending {
        Token id = 0, stroke_scope = 0;
        const void* canvas = nullptr;
        Clock::time_point begin;
        bool generation_finished = false, background_pending = false, background_running = false, frame_presented = false;
    };
    static Pending& state() { static thread_local Pending pending; return pending; }
    static bool matches(Token token) { return token && token == state().id; }
    static void emit(const char* phase, const char* status)
    {
        std::fprintf(stderr,
            "TRACERY_INTERACTION_PERF operation_id=%llu stroke_scope_id=%llu phase=%s since_start_ms=%.3f status=%s\n",
            state().id, state().stroke_scope, phase,
            std::chrono::duration<double, std::milli>(Clock::now() - state().begin).count(), status);
    }
    static void close(const char* status)
    {
        emit("interaction_until_idle", status);
        state() = Pending{};
    }
};

#else
class TraceryInteractionTiming
{
public:
    using Token = unsigned long long;
    static constexpr Token begin(const void*, Token) noexcept { return 0; }
    static void generation_finished(Token) noexcept {}
    static void background_scheduled() noexcept {}
    static constexpr bool after_generation(const void* = nullptr) noexcept { return false; }
    static void milestone(const char*) noexcept {}
    static constexpr Token background_started() noexcept { return 0; }
    static void background_finished(Token) noexcept {}
    static void frame_presented(const void*) noexcept {}
    static void idle(const void*) noexcept {}
    static void fail(Token) noexcept {}
    static void fail_current() noexcept {}
    class Activity
    {
    public:
        constexpr Activity(const char*, const void* = nullptr) noexcept {}
        ~Activity() noexcept {}
        void stage(const char*) const noexcept {}
    };
    class Stroke
    {
    public:
        constexpr Stroke(const void*, Token) noexcept {}
        ~Stroke() noexcept {}
        Stroke(const Stroke&) = delete;
        Stroke& operator=(const Stroke&) = delete;
    };
};
#endif

} // namespace Slic3r
#endif
