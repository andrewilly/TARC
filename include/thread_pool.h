#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <stop_token>

namespace tarc {

class ThreadPool {
private:
    std::vector<std::jthread> workers;
    std::queue<std::function<void()>> tasks;
    mutable std::mutex queue_mutex;
    std::condition_variable_any condition;
    std::stop_source stop_source;
    bool stopping = false;
    
public:
    explicit ThreadPool(size_t threads = std::thread::hardware_concurrency()) {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this](const std::stop_token& stoken) {
                while (!stoken.stop_requested()) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(queue_mutex);
                        condition.wait(lock, [this, &stoken] {
                            return stopping || !tasks.empty() || stoken.stop_requested();
                        });
                        
                        if (stopping || (tasks.empty() && stoken.stop_requested())) {
                            return;
                        }
                        
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }
    
    ~ThreadPool() {
        {
            std::unique_lock lock(queue_mutex);
            stopping = true;
        }
        stop_source.request_stop();
        condition.notify_all();
    }
    
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;
        
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> result = task->get_future();
        {
            std::unique_lock lock(queue_mutex);
            if (stopping) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return result;
    }
    
    void wait_for_tasks() {
        std::unique_lock lock(queue_mutex);
        condition.wait(lock, [this] { return tasks.empty(); });
    }
    
    size_t pending_tasks() const {
        std::shared_lock lock(queue_mutex);
        return tasks.size();
    }
};

} // namespace tarc
