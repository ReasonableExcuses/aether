#include "aether/background_compiler.h"
#include <iostream>

namespace aether {

static thread_local BackgroundCompiler* s_current_background_compiler_{nullptr};
static std::atomic<bool> s_background_compiler_enabled_{false};

BackgroundCompiler* BackgroundCompiler::Current() noexcept {
    return s_current_background_compiler_;
}

void BackgroundCompiler::SetCurrent(BackgroundCompiler* compiler) noexcept {
    s_current_background_compiler_ = compiler;
}

bool BackgroundCompiler::IsEnabled() noexcept {
    return s_background_compiler_enabled_.load(std::memory_order_relaxed);
}

void BackgroundCompiler::SetEnabled(bool enabled) noexcept {
    s_background_compiler_enabled_.store(enabled, std::memory_order_relaxed);
}

BackgroundCompiler::BackgroundCompiler(size_t thread_count)
    : thread_count_(thread_count > 0 ? thread_count : 1) {
    if (!s_current_background_compiler_) {
        s_current_background_compiler_ = this;
    }
}

BackgroundCompiler::~BackgroundCompiler() {
    Stop();
    if (s_current_background_compiler_ == this) {
        s_current_background_compiler_ = nullptr;
    }
}

void BackgroundCompiler::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (is_running_.load(std::memory_order_acquire)) {
        return;
    }

    stop_requested_.store(false, std::memory_order_release);
    is_running_.store(true, std::memory_order_release);

    for (size_t i = 0; i < thread_count_; ++i) {
        workers_.emplace_back(&BackgroundCompiler::WorkerLoop, this);
    }
}

void BackgroundCompiler::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!is_running_.load(std::memory_order_acquire)) {
        return;
    }

    stop_requested_.store(true, std::memory_order_release);
    cv_work_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    is_running_.store(false, std::memory_order_release);
}

bool BackgroundCompiler::Enqueue(std::shared_ptr<const BytecodeArray> bytecode,
                                 const TypeFeedbackVector* feedback,
                                 std::function<void(std::shared_ptr<ExecutableMemory>)> on_complete) {
    if (!bytecode) return false;
    if (!is_running_.load(std::memory_order_acquire)) {
        Start();
    }

    BackgroundCompilationJob job;
    job.bytecode_shared = std::move(bytecode);
    if (feedback) {
        job.feedback_snapshot = feedback->Snapshot();
    }
    job.on_complete = std::move(on_complete);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(std::move(job));
    }
    cv_work_.notify_one();
    return true;
}

bool BackgroundCompiler::Enqueue(const BytecodeArray& bytecode, const TypeFeedbackVector* feedback,
                                 std::function<void(std::shared_ptr<ExecutableMemory>)> on_complete) {
    if (!is_running_.load(std::memory_order_acquire)) {
        Start();
    }

    BackgroundCompilationJob job;
    job.bytecode_shared = bytecode.GetShared();
    job.bytecode_raw = &bytecode;
    if (feedback) {
        job.feedback_snapshot = feedback->Snapshot();
    }
    job.on_complete = std::move(on_complete);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(std::move(job));
    }
    cv_work_.notify_one();
    return true;
}

void BackgroundCompiler::WaitForPendingTasks() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    cv_idle_.wait(lock, [this]() {
        return queue_.empty() && active_workers_.load(std::memory_order_relaxed) == 0;
    });
}

size_t BackgroundCompiler::PendingTaskCount() const noexcept {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size() + active_workers_.load(std::memory_order_relaxed);
}

void BackgroundCompiler::WorkerLoop() {
    while (true) {
        BackgroundCompilationJob job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_work_.wait(lock, [this]() {
                return stop_requested_.load(std::memory_order_relaxed) || !queue_.empty();
            });

            if (stop_requested_.load(std::memory_order_relaxed) && queue_.empty()) {
                break;
            }

            if (queue_.empty()) {
                continue;
            }

            job = std::move(queue_.front());
            queue_.pop();
            active_workers_.fetch_add(1, std::memory_order_relaxed);
        }

        // Perform Sea-of-Nodes Tier 2 compilation in background worker
        const BytecodeArray* bcode = job.GetBytecode();
        if (bcode) {
            auto opt_code = OptimizingJIT::Compile(*bcode, job.feedback_snapshot.get());
            if (opt_code) {
                std::shared_ptr<ExecutableMemory> shared = std::move(opt_code);
                bcode->SetTier2Code(shared);
                if (job.bytecode_raw && job.bytecode_raw != bcode) {
                    job.bytecode_raw->SetTier2Code(shared);
                }
                if (job.on_complete) {
                    job.on_complete(shared);
                }
            } else {
                bcode->FinishTier2Compilation();
                if (job.bytecode_raw && job.bytecode_raw != bcode) {
                    job.bytecode_raw->FinishTier2Compilation();
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            completed_tasks_.fetch_add(1, std::memory_order_relaxed);
            active_workers_.fetch_sub(1, std::memory_order_seq_cst);
            if (queue_.empty() && active_workers_.load(std::memory_order_seq_cst) == 0) {
                cv_idle_.notify_all();
            }
        }
    }
}

} // namespace aether
