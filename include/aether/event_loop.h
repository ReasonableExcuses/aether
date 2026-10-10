#ifndef AETHER_EVENT_LOOP_H
#define AETHER_EVENT_LOOP_H

#include "aether/common.h"
#include "aether/value.h"
#include <chrono>
#include <deque>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

namespace aether {

class Agent;

/**
 * @brief Represents a scheduled timer task (setTimeout / setInterval).
 */
struct TimerEntry {
    uint32_t id{0};
    std::chrono::steady_clock::time_point due_time;
    std::chrono::milliseconds interval{0};
    TaggedValue callback{TaggedValue::Undefined()};
    std::vector<TaggedValue> args;
    bool is_interval{false};
    bool cancelled{false};
};

/**
 * @brief Comparator for ordering timers by earliest due_time in min-heap.
 */
struct TimerCompare {
    bool operator()(const std::shared_ptr<TimerEntry>& a, const std::shared_ptr<TimerEntry>& b) const noexcept {
        return a->due_time > b->due_time;
    }
};

/**
 * @brief Represents an immediate task (setImmediate).
 */
struct ImmediateEntry {
    uint32_t id{0};
    TaggedValue callback{TaggedValue::Undefined()};
    std::vector<TaggedValue> args;
    bool cancelled{false};
};

/**
 * @brief High-throughput, specification-compliant Asynchronous Event Loop for AetherVM.
 */
class EventLoop {
public:
    EventLoop();
    ~EventLoop() = default;

    static EventLoop* Current() noexcept { return s_current_event_loop_; }
    static void SetCurrent(EventLoop* loop) noexcept { s_current_event_loop_ = loop; }

    uint32_t SetTimeout(TaggedValue callback, int64_t delay_ms, std::vector<TaggedValue> args = {});
    void ClearTimeout(uint32_t id);

    uint32_t SetInterval(TaggedValue callback, int64_t interval_ms, std::vector<TaggedValue> args = {});
    void ClearInterval(uint32_t id);

    uint32_t SetImmediate(TaggedValue callback, std::vector<TaggedValue> args = {});
    void ClearImmediate(uint32_t id);

    double PerformanceNow() const noexcept;

    bool HasPendingWork() const noexcept;
    bool Step(Agent& agent);
    void Run(Agent& agent, size_t max_ticks = 100000);
    void Stop() noexcept { stopped_ = true; }

private:
    static inline thread_local EventLoop* s_current_event_loop_{nullptr};

    std::chrono::steady_clock::time_point start_time_;
    uint32_t next_timer_id_{1};
    uint32_t next_immediate_id_{1};
    bool stopped_{false};

    std::priority_queue<std::shared_ptr<TimerEntry>,
                        std::vector<std::shared_ptr<TimerEntry>>,
                        TimerCompare> timer_queue_;
    std::unordered_map<uint32_t, std::shared_ptr<TimerEntry>> timers_by_id_;

    std::deque<std::shared_ptr<ImmediateEntry>> immediate_queue_;
    std::unordered_map<uint32_t, std::shared_ptr<ImmediateEntry>> immediates_by_id_;
};

} // namespace aether

#endif // AETHER_EVENT_LOOP_H
