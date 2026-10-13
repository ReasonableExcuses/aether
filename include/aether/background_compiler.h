#ifndef AETHER_BACKGROUND_COMPILER_H
#define AETHER_BACKGROUND_COMPILER_H

#include "aether/common.h"
#include "aether/bytecode.h"
#include "aether/optimizing_jit.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <functional>
#include <memory>

namespace aether {

/**
 * @brief Job payload dispatched to background JIT compilation worker threads.
 */
struct BackgroundCompilationJob {
    const BytecodeArray* bytecode{nullptr};
    TypeFeedbackVector* feedback{nullptr};
    std::function<void(std::shared_ptr<ExecutableMemory>)> on_complete{nullptr};
};

/**
 * @brief Multi-threaded background JIT compiler offloading Tier 2 Sea-of-Nodes optimization.
 *
 * Prevents mutator thread stalls when functions become hot by executing Sea-of-Nodes
 * graph building, GVN, DCE, and machine code generation in parallel worker threads.
 */
class BackgroundCompiler {
public:
    explicit BackgroundCompiler(size_t thread_count = 2);
    ~BackgroundCompiler();

    static BackgroundCompiler* Current() noexcept;
    static void SetCurrent(BackgroundCompiler* compiler) noexcept;
    static bool IsEnabled() noexcept;
    static void SetEnabled(bool enabled) noexcept;

    void Start();
    void Stop();

    // Enqueue compilation job. Returns true if queued, false if rejected.
    bool Enqueue(const BytecodeArray& bytecode, TypeFeedbackVector* feedback = nullptr,
                 std::function<void(std::shared_ptr<ExecutableMemory>)> on_complete = nullptr);

    // Wait for all currently pending and active compilation tasks to finish
    void WaitForPendingTasks();

    size_t PendingTaskCount() const noexcept;
    size_t CompletedTaskCount() const noexcept { return completed_tasks_.load(std::memory_order_relaxed); }
    size_t WorkerCount() const noexcept { return workers_.size(); }
    bool IsRunning() const noexcept { return is_running_.load(std::memory_order_relaxed); }

private:
    void WorkerLoop();

    size_t thread_count_{2};
    std::vector<std::thread> workers_;
    std::queue<BackgroundCompilationJob> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_work_;
    std::condition_variable cv_idle_;

    std::atomic<bool> is_running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<size_t> active_workers_{0};
    std::atomic<size_t> completed_tasks_{0};
};

} // namespace aether

#endif // AETHER_BACKGROUND_COMPILER_H
