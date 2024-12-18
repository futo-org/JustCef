#include "thread_pool.h"
#include "ipc.h"

ThreadPool::ThreadPool() : _stop(false) { }
ThreadPool::~ThreadPool() 
{ 
    Stop(); 
}

void ThreadPool::AddWorkers(size_t count)
{
    for (size_t i = 0; i < count; ++i) 
    {
        std::thread thread([this] 
        {
            for (;;)
            {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(_queue_mutex);
                    _condition.wait(lock, [this] { return _stop || !_tasks.empty(); });
                    if (_stop && _tasks.empty())
                        return;
                    task = std::move(_tasks.front());
                    _tasks.pop();
                }

                task();
            }
        });
        thread.detach();
        _workers.emplace_back(std::move(thread));
    }
}

void ThreadPool::Enqueue(std::function<void()> task) 
{
    {
        std::unique_lock<std::mutex> lock(_queue_mutex);
        if (_stop)
            return;
            
        _tasks.emplace(std::move(task));
    }
    _condition.notify_one();
}

void ThreadPool::Stop()
{
    {
        std::unique_lock<std::mutex> lock(_queue_mutex);
        if (_stop)
            return;

        _stop = true;
    }
    _condition.notify_all();
    /* do not wait for (std::thread& worker : _workers)
    {
        if (worker.joinable()) {
            worker.join();
        }
    }*/
}

