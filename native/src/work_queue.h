#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "include/base/cef_logging.h"

class WorkQueue {
public:
    WorkQueue() : _exitFlag(false) 
    {
        
    }

    ~WorkQueue() 
    {
        Stop();
    }

    void Start()
    {
        if (_started)
            return;

        _worker = std::thread([this] { this->WorkerThread(); });
        _worker.detach();
        _started = true;
        LOG(INFO) << "Work queue started.";
    }

    void Stop()
    {
        if (!_started)
            return;

        {
            std::unique_lock lock(_mutex);
            _exitFlag = true;
            while (!_queue.empty()) {
                _queue.pop();
            }
        }

        _condition.notify_one();
        LOG(INFO) << "Worker queue exit flag set with " << _queue.size() << " queue items.";

        /*if (_worker.joinable()) {
            _worker.join();
        }

        LOG(INFO) << "Worker joined with " << _queue.size() << " queue items.";*/
    }

    void EnqueueWork(std::function<void()> work) 
    {
        {
            std::unique_lock lock(_mutex);
            _queue.push(work);
        }
        _condition.notify_one();
    }

private:
    std::mutex _mutex;
    std::condition_variable _condition;
    std::queue<std::function<void()>> _queue;
    bool _exitFlag;
    std::thread _worker;
    bool _started = false;

    void WorkerThread() 
    {
        while (true) 
        {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _condition.wait(lock, [this] { return _exitFlag || !_queue.empty(); });
                if (_exitFlag)
                {
                    LOG(INFO) << "Worker thread shutting down.";
                    return;
                }

                work = _queue.front();
                _queue.pop();
            }

            if (work)
            {
                work();
            }
        }
    }
};

#endif //WORK_QUEUE_H
