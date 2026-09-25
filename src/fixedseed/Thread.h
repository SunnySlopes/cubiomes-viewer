#ifndef FIXEDSEED_THREAD_H
#define FIXEDSEED_THREAD_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

template<typename T>
class ThreadSafeResults {
    std::vector<T> results_;
    mutable std::mutex mutex_{};

public:
    void addResult(const T &res)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back(res);
        clean();
    }

    void addResults(const std::vector<T> &newResults)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.insert(results_.end(), newResults.cbegin(), newResults.cend());
        clean();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.clear();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return results_.empty();
    }

    std::vector<T> getAllResults()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Descending via operator< only (hit types may not define operator>).
        std::sort(results_.begin(), results_.end(),
                  [](const T &a, const T &b) { return b < a; });
        return results_;
    }

private:
    void clean()
    {
        if (results_.size() > 4000) {
            std::nth_element(results_.begin(),
                    results_.begin() + 1000,
                    results_.end(),
                    [](const T &a, const T &b) { return b < a; });
            results_.resize(1000);
        }
    }
};

class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    std::condition_variable spaceAvailable;
    std::atomic<bool> stop{false};
    size_t maxQueueSize;

public:
    explicit ThreadPool(size_t threads, size_t maxQueued = 0)
        : maxQueueSize(maxQueued == 0 ? std::max<size_t>(threads * 4, 32) : maxQueued)
    {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queueMutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if (this->stop && this->tasks.empty())
                            return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                        this->spaceAvailable.notify_one();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F &&f)
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            spaceAvailable.wait(lock, [this] {
                return this->stop || this->tasks.size() < this->maxQueueSize;
            });
            if (stop)
                return;
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        spaceAvailable.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable())
                worker.join();
        }
    }
};

#endif
