#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <stdexcept>

class ThreadPool 
{
public:
    ThreadPool();
    ~ThreadPool();

    void AddWorkers(size_t count);
    void Enqueue(std::function<void()> task);

    void Stop();
private:
    std::vector<std::thread> _workers;
    std::queue<std::function<void()>> _tasks;

    std::mutex _queue_mutex;
    std::condition_variable _condition;
    bool _stop;
};

#endif //THREAD_POOL_H

