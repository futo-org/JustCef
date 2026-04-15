#pragma once

#include "asio.h"

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace justcef::detail {

class AsyncSignal {
public:
    using Waiter = std::function<void(std::exception_ptr)>;

    bool IsSignaled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return signaled_;
    }

    void SignalSuccess() {
        Complete(nullptr);
    }

    void SignalFailure(std::exception_ptr exception) {
        Complete(std::move(exception));
    }

    void Wait() const {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return signaled_; });
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    asio::awaitable<void> AsyncWait(const asio::any_io_executor& fallback_executor) const {
        co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(std::exception_ptr)>(
            [this, fallback_executor](auto handler) mutable {
                using Handler = std::decay_t<decltype(handler)>;

                auto handler_ptr = std::make_shared<Handler>(std::move(handler));
                auto handler_executor = asio::get_associated_executor(*handler_ptr, fallback_executor);

                std::exception_ptr exception;
                bool dispatch_immediately = false;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (signaled_) {
                        dispatch_immediately = true;
                        exception = exception_;
                    } else {
                        waiters_.push_back([handler_ptr, handler_executor](std::exception_ptr completion_exception) mutable {
                            asio::dispatch(handler_executor, [handler_ptr, completion_exception]() mutable {
                                auto completion_handler = std::move(*handler_ptr);
                                completion_handler(completion_exception);
                            });
                        });
                    }
                }

                if (dispatch_immediately) {
                    asio::dispatch(handler_executor, [handler_ptr, exception]() mutable {
                        auto completion_handler = std::move(*handler_ptr);
                        completion_handler(exception);
                    });
                }
            },
            asio::use_awaitable);
    }

private:
    void Complete(std::exception_ptr exception) {
        std::vector<Waiter> waiters;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (signaled_) {
                return;
            }

            signaled_ = true;
            exception_ = std::move(exception);
            waiters.swap(waiters_);
        }

        condition_.notify_all();
        for (auto& waiter : waiters) {
            if (waiter) {
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

}  // namespace justcef::detail
