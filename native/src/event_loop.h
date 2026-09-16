#ifndef JUSTCEF_IPC_EVENT_LOOP_H_
#define JUSTCEF_IPC_EVENT_LOOP_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ipc
{

class EventLoop
{
public:
    using Task = std::function<void()>;
    using TimerId = uint64_t;

    EventLoop() = default;
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void Start();
    void Stop();
    bool Post(Task task);
    TimerId PostDelayed(std::chrono::milliseconds delay, Task task);
    void CancelTimer(TimerId id);
    bool RunsTasksOnCurrentThread() const;

private:
    using Clock = std::chrono::steady_clock;

    void Run();

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<Task> _tasks;
    std::multimap<Clock::time_point, TimerId> _deadlines;
    std::unordered_map<TimerId, Task> _timers;
    TimerId _nextTimer = 0;
    bool _running = false;
    bool _stopping = false;
    std::thread _thread;
    std::atomic<std::thread::id> _threadId{};
};

} // namespace ipc

#endif // JUSTCEF_IPC_EVENT_LOOP_H_
