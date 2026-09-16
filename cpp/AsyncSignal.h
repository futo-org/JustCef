#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace justcef::detail
{

template <typename Signature> class Completion;

template <typename... Args> class Completion<void(Args...)>
{
public:
    Completion() = default;

    template <typename Handler>
    Completion(Handler&& handler, const asio::any_io_executor& fallback)
        : work_(asio::prefer(asio::any_io_executor(asio::get_associated_executor(handler, fallback)), asio::execution::outstanding_work.tracked)),
          handler_(std::forward<Handler>(handler))
    {
    }

    Completion(Completion&&) noexcept = default;
    Completion& operator=(Completion&&) noexcept = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;

    explicit operator bool() const { return static_cast<bool>(handler_); }

    asio::cancellation_slot Slot() const { return asio::get_associated_cancellation_slot(handler_); }

    template <typename... Values> void Post(Values&&... values)
    {
        if (!handler_)
        {
            return;
        }

        auto executor = work_;
        asio::post(executor,
                   [handler = std::move(handler_), work = std::move(work_), arguments = std::make_tuple(std::decay_t<Args>(std::forward<Values>(values))...)]() mutable
                   {
                       auto slot = asio::get_associated_cancellation_slot(handler);
                       if (slot.is_connected())
                       {
                           slot.clear();
                       }
                       std::apply(
                           [&handler](auto&&... unpacked)
                           {
                               std::move(handler)(std::forward<decltype(unpacked)>(unpacked)...);
                           },
                           std::move(arguments));
                       work = asio::any_io_executor();
                   });
    }

private:
    asio::any_io_executor work_;
    asio::any_completion_handler<void(Args...)> handler_;
};

class AsyncSignal
{
public:
    using Waiter = Completion<void(std::exception_ptr)>;

    AsyncSignal() : state_(std::make_shared<State>()) {}

    ~AsyncSignal() { Complete(std::make_exception_ptr(asio::system_error(asio::error::operation_aborted))); }

    AsyncSignal(const AsyncSignal&) = delete;
    AsyncSignal& operator=(const AsyncSignal&) = delete;

    bool IsSignaled() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->signaled;
    }

    void SignalSuccess() { Complete(nullptr); }

    void SignalFailure(std::exception_ptr exception) { Complete(std::move(exception)); }

    void Wait() const
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->condition.wait(lock,
                               [this]()
                               {
                                   return state_->signaled;
                               });
        if (state_->exception)
        {
            std::rethrow_exception(state_->exception);
        }
    }

    bool WaitFor(std::chrono::milliseconds timeout) const
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        return state_->condition.wait_for(lock, timeout,
                                          [this]()
                                          {
                                              return state_->signaled;
                                          });
    }

    asio::awaitable<void> AsyncWait(const asio::any_io_executor& fallback_executor) const { return Await(state_, fallback_executor); }

private:
    struct State
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool signaled = false;
        std::exception_ptr exception;
        std::uint64_t next_identifier = 0;
        std::vector<std::pair<std::uint64_t, Waiter>> waiters;
    };

    static asio::awaitable<void> Await(std::shared_ptr<State> state, asio::any_io_executor fallback_executor)
    {
        co_await asio::async_initiate<decltype(asio::use_awaitable), void(std::exception_ptr)>(
            [state, fallback_executor](auto handler)
            {
                Initiate(state, Waiter(std::move(handler), fallback_executor));
            },
            asio::use_awaitable);
    }

    static void Initiate(const std::shared_ptr<State>& state, Waiter waiter)
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->signaled)
        {
            auto exception = state->exception;
            lock.unlock();
            waiter.Post(exception);
            return;
        }

        const std::uint64_t identifier = ++state->next_identifier;
        auto slot = waiter.Slot();
        state->waiters.emplace_back(identifier, std::move(waiter));
        if (slot.is_connected())
        {
            slot.assign(
                [weak_state = std::weak_ptr<State>(state), identifier](asio::cancellation_type)
                {
                    auto locked = weak_state.lock();
                    if (!locked)
                    {
                        return;
                    }

                    Waiter removed;
                    {
                        std::lock_guard<std::mutex> guard(locked->mutex);
                        for (auto iterator = locked->waiters.begin(); iterator != locked->waiters.end(); ++iterator)
                        {
                            if (iterator->first == identifier)
                            {
                                removed = std::move(iterator->second);
                                locked->waiters.erase(iterator);
                                break;
                            }
                        }
                    }

                    if (removed)
                    {
                        removed.Post(std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)));
                    }
                });
        }
    }

    void Complete(std::exception_ptr exception)
    {
        std::vector<std::pair<std::uint64_t, Waiter>> waiters;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->signaled)
            {
                return;
            }

            state_->signaled = true;
            state_->exception = exception;
            waiters.swap(state_->waiters);
        }

        state_->condition.notify_all();
        for (auto& [_, waiter] : waiters)
        {
            waiter.Post(exception);
        }
    }

    std::shared_ptr<State> state_;
};

} // namespace justcef::detail
