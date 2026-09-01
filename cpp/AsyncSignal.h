#pragma once

#include <asio.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace justcef::detail
{

class AsyncSignal
{
public:
    using Waiter = std::function<void(std::exception_ptr)>;

    bool IsSignaled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return signaled_;
    }

    void SignalSuccess() { Complete(nullptr); }

    void SignalFailure(std::exception_ptr exception) { Complete(std::move(exception)); }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        signaled_ = false;
        exception_ = nullptr;
    }

    void Wait() const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock,
                        [this]()
                        {
                            return signaled_;
                        });
        if (exception_)
        {
            std::rethrow_exception(exception_);
        }
    }

    asio::awaitable<void> AsyncWait(const asio::any_io_executor& fallback_executor) const
    {
        co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(std::exception_ptr)>(
            [this, fallback_executor](auto handler) mutable
            {
                using Handler = std::decay_t<decltype(handler)>;

                auto handler_ptr = std::make_shared<Handler>(std::move(handler));
                auto handler_executor = asio::get_associated_executor(*handler_ptr, fallback_executor);
                auto cancel_slot = asio::get_associated_cancellation_slot(*handler_ptr);
                auto fired = std::make_shared<std::atomic<bool>>(false);

                auto fire = [handler_ptr, handler_executor, fired](std::exception_ptr ex)
                {
                    if (fired->exchange(true))
                    {
                        return;
                    }
                    asio::dispatch(handler_executor,
                                   [handler_ptr, ex]() mutable
                                   {
                                       auto completion_handler = std::move(*handler_ptr);
                                       completion_handler(ex);
                                   });
                };

                if (cancel_slot.is_connected())
                {
                    cancel_slot.assign(
                        [fire](asio::cancellation_type) mutable
                        {
                            fire(std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)));
                        });
                }

                std::exception_ptr immediate_exception;
                bool dispatch_immediately = false;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (signaled_)
                    {
                        dispatch_immediately = true;
                        immediate_exception = exception_;
                    }
                    else
                    {
                        waiters_.push_back(fire);
                    }
                }

                if (dispatch_immediately)
                {
                    fire(immediate_exception);
                }
            },
            asio::use_awaitable);
    }

private:
    void Complete(std::exception_ptr exception)
    {
        std::vector<Waiter> waiters;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (signaled_)
            {
                return;
            }

            signaled_ = true;
            exception_ = std::move(exception);
            waiters.swap(waiters_);
        }

        condition_.notify_all();
        for (auto& waiter : waiters)
        {
            if (waiter)
            {
                waiter(exception_);
            }
        }
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::vector<Waiter> waiters_;
    bool signaled_ = false;
    std::exception_ptr exception_;
};

} // namespace justcef::detail
