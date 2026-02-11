#pragma once

#include <functional>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <optional>
#include <intrin.h>

namespace UltraImageViewer {
namespace Core {

enum class TaskPriority : uint8_t { High = 0, Normal = 1, Low = 2 };

class ThreadPool {
public:
    explicit ThreadPool(uint32_t numThreads = 0);  // 0 = auto
    ~ThreadPool();

    // Current task's lane for the calling thread (0=High, 1=Normal, 2=Low).
    // Only meaningful inside a task callback. Returns -1 outside a task.
    static int CurrentLane() { return tl_currentLane_; }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a task to the back of the given priority lane
    void Submit(std::function<void()> fn, TaskPriority p = TaskPriority::Normal);

    // Submit a task to the front of the given priority lane (for urgent visible work)
    void SubmitFront(std::function<void()> fn, TaskPriority p = TaskPriority::High);

    // Submit a batch of tasks (single lock acquisition, notify_all)
    void SubmitBatch(std::vector<std::function<void()>>& fns, TaskPriority p);

    // Cancel all pending tasks across all lanes
    void PurgeAll();

    // Cancel all pending tasks in a specific priority lane
    void PurgePriority(TaskPriority p);

    // Stats
    uint32_t ThreadCount()    const { return threadCount_; }
    uint32_t PendingCount()   const { return pending_.load(std::memory_order_relaxed); }
    uint32_t ActiveCount()    const { return active_.load(std::memory_order_relaxed); }
    uint64_t CompletedCount() const { return completed_.load(std::memory_order_relaxed); }

    // Block until all pending + active tasks are done
    void WaitIdle();

private:
    void WorkerFunc(uint32_t index);

    struct DequeuedTask {
        std::function<void()> fn;
        int lane;  // 0=High, 1=Normal, 2=Low
    };
    std::optional<DequeuedTask> TryDequeue();

    static constexpr int kLaneCount = 3;
    static constexpr int kSpinCount = 64;
    static constexpr int kYieldCount = 256;

    // 3 priority lanes, each cache-line padded
    struct alignas(64) Lane {
        std::deque<std::function<void()>> queue;
    };
    Lane lanes_[kLaneCount];

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCV_;

    std::vector<std::jthread> threads_;
    uint32_t threadCount_ = 0;

    alignas(64) std::atomic<uint32_t> pending_{0};
    alignas(64) std::atomic<uint32_t> active_{0};
    alignas(64) std::atomic<uint64_t> completed_{0};
    std::atomic<bool> shutdown_{false};

    static thread_local int tl_currentLane_;
};

} // namespace Core
} // namespace UltraImageViewer
