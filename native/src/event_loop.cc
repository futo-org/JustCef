#include "event_loop.h"

namespace ipc
{

EventLoop::~EventLoop()
{
    Stop();
}

void EventLoop::Start()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_running)
        return;
    _running = true;
    _stopping = false;
    _thread = std::thread([this] { Run(); });
}

void EventLoop::Stop()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || _stopping)
            return;
        _stopping = true;
    }
    _changed.notify_all();
    if (_thread.joinable())
    {
        if (_thread.get_id() == std::this_thread::get_id())
            _thread.detach();
        else
            _thread.join();
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _running = false;
    _tasks.clear();
    _timers.clear();
    _deadlines.clear();
}

bool EventLoop::Post(Task task)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || _stopping)
            return false;
        _tasks.push_back(std::move(task));
    }
    _changed.notify_all();
    return true;
}

EventLoop::TimerId EventLoop::PostDelayed(std::chrono::milliseconds delay, Task task)
{
    TimerId id = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || _stopping)
            return 0;
        id = ++_nextTimer;
        _timers.emplace(id, std::move(task));
        _deadlines.emplace(Clock::now() + delay, id);
    }
    _changed.notify_all();
    return id;
}

void EventLoop::CancelTimer(TimerId id)
{
    if (id == 0)
        return;
    std::lock_guard<std::mutex> lock(_mutex);
    _timers.erase(id);
}

bool EventLoop::RunsTasksOnCurrentThread() const
{
    return _threadId.load() == std::this_thread::get_id();
}

void EventLoop::Run()
{
    _threadId = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(_mutex);
    while (true)
    {
        while (!_deadlines.empty() && _deadlines.begin()->first <= Clock::now())
        {
            const TimerId id = _deadlines.begin()->second;
            _deadlines.erase(_deadlines.begin());
            auto timer = _timers.find(id);
            if (timer == _timers.end())
                continue;
            _tasks.push_back(std::move(timer->second));
            _timers.erase(timer);
        }

        if (!_tasks.empty())
        {
            Task task = std::move(_tasks.front());
            _tasks.pop_front();
            lock.unlock();
            task();
            lock.lock();
            continue;
        }

        if (_stopping)
            break;

        if (_deadlines.empty())
            _changed.wait(lock);
        else
            _changed.wait_until(lock, _deadlines.begin()->first);
    }
    _threadId = std::thread::id();
}

} // namespace ipc
