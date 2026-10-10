#include "aether/event_loop.h"
#include "aether/runtime.h"
#include <thread>
#include <algorithm>

namespace aether {

EventLoop::EventLoop()
    : start_time_(std::chrono::steady_clock::now()) {}

uint32_t EventLoop::SetTimeout(TaggedValue callback, int64_t delay_ms, std::vector<TaggedValue> args) {
    if (delay_ms < 0) delay_ms = 0;
    auto entry = std::make_shared<TimerEntry>();
    entry->id = next_timer_id_++;
    entry->due_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    entry->interval = std::chrono::milliseconds(0);
    entry->callback = callback;
    entry->args = std::move(args);
    entry->is_interval = false;
    entry->cancelled = false;

    timers_by_id_[entry->id] = entry;
    timer_queue_.push(entry);
    return entry->id;
}

void EventLoop::ClearTimeout(uint32_t id) {
    auto it = timers_by_id_.find(id);
    if (it != timers_by_id_.end()) {
        it->second->cancelled = true;
        timers_by_id_.erase(it);
    }
}

uint32_t EventLoop::SetInterval(TaggedValue callback, int64_t interval_ms, std::vector<TaggedValue> args) {
    if (interval_ms <= 0) interval_ms = 1;
    auto entry = std::make_shared<TimerEntry>();
    entry->id = next_timer_id_++;
    entry->interval = std::chrono::milliseconds(interval_ms);
    entry->due_time = std::chrono::steady_clock::now() + entry->interval;
    entry->callback = callback;
    entry->args = std::move(args);
    entry->is_interval = true;
    entry->cancelled = false;

    timers_by_id_[entry->id] = entry;
    timer_queue_.push(entry);
    return entry->id;
}

void EventLoop::ClearInterval(uint32_t id) {
    ClearTimeout(id);
}

uint32_t EventLoop::SetImmediate(TaggedValue callback, std::vector<TaggedValue> args) {
    auto entry = std::make_shared<ImmediateEntry>();
    entry->id = next_immediate_id_++;
    entry->callback = callback;
    entry->args = std::move(args);
    entry->cancelled = false;

    immediates_by_id_[entry->id] = entry;
    immediate_queue_.push_back(entry);
    return entry->id;
}

void EventLoop::ClearImmediate(uint32_t id) {
    auto it = immediates_by_id_.find(id);
    if (it != immediates_by_id_.end()) {
        it->second->cancelled = true;
        immediates_by_id_.erase(it);
    }
}

double EventLoop::PerformanceNow() const noexcept {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start_time_).count();
}

bool EventLoop::HasPendingWork() const noexcept {
    return !timers_by_id_.empty() || !immediates_by_id_.empty();
}

bool EventLoop::Step(Agent& agent) {
    bool did_work = false;

    // 1. Process Immediates phase (Node.js check phase)
    if (!immediate_queue_.empty()) {
        size_t count = immediate_queue_.size();
        for (size_t i = 0; i < count && !immediate_queue_.empty(); ++i) {
            auto imm = immediate_queue_.front();
            immediate_queue_.pop_front();
            if (!imm->cancelled) {
                immediates_by_id_.erase(imm->id);
                did_work = true;
                CallFunction(imm->callback, TaggedValue::Undefined(), imm->args);
                agent.GetMicrotasks().PerformMicrotaskCheckpoint();
            }
        }
    }

    // 2. Check and fire expired timers
    auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<TimerEntry>> to_fire;

    while (!timer_queue_.empty()) {
        auto top = timer_queue_.top();
        if (top->cancelled) {
            timer_queue_.pop();
            continue;
        }
        if (top->due_time <= now) {
            timer_queue_.pop();
            to_fire.push_back(top);
        } else {
            break;
        }
    }

    for (const auto& timer : to_fire) {
        if (timer->cancelled) continue;
        did_work = true;

        if (!timer->is_interval) {
            timers_by_id_.erase(timer->id);
        }

        CallFunction(timer->callback, TaggedValue::Undefined(), timer->args);
        agent.GetMicrotasks().PerformMicrotaskCheckpoint();

        if (timer->is_interval && !timer->cancelled) {
            timer->due_time = std::chrono::steady_clock::now() + timer->interval;
            timer_queue_.push(timer);
        }
    }

    // 3. If there is work remaining in future timers, sleep shortly to avoid busy spinning
    if (!did_work && !timers_by_id_.empty()) {
        while (!timer_queue_.empty() && timer_queue_.top()->cancelled) {
            timer_queue_.pop();
        }
        if (!timer_queue_.empty()) {
            auto next_due = timer_queue_.top()->due_time;
            auto curr = std::chrono::steady_clock::now();
            if (next_due > curr) {
                auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_due - curr);
                auto sleep_duration = std::min(wait_ms, std::chrono::milliseconds(5));
                if (sleep_duration.count() > 0) {
                    std::this_thread::sleep_for(sleep_duration);
                }
            }
        }
    }

    return did_work || HasPendingWork();
}

void EventLoop::Run(Agent& agent, size_t max_ticks) {
    stopped_ = false;
    size_t ticks = 0;
    while (HasPendingWork() && !stopped_ && ticks < max_ticks) {
        Step(agent);
        ticks++;
    }
}

} // namespace aether
