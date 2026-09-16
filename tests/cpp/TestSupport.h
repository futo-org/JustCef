#pragma once

#include "doctest.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace justcef::tests
{

class Watchdog
{
public:
    explicit Watchdog(std::chrono::seconds limit, std::string name) : name_(std::move(name))
    {
        thread_ = std::thread(
            [this, limit]()
            {
                const auto deadline = std::chrono::steady_clock::now() + limit;
                while (!done_.load())
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        std::cerr << "Watchdog: test '" << name_ << "' exceeded its hard limit; aborting." << std::endl;
                        std::abort();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
    }

    ~Watchdog()
    {
        done_ = true;
        thread_.join();
    }

private:
    std::string name_;
    std::atomic<bool> done_ = false;
    std::thread thread_;
};

inline std::string DescribeException(std::exception_ptr exception)
{
    if (!exception)
    {
        return "no exception";
    }
    try
    {
        std::rethrow_exception(exception);
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    catch (...)
    {
        return "unknown exception";
    }
}

struct RunState
{
    std::atomic<bool> done = false;
    std::exception_ptr error;
};

template <typename Awaitable> std::shared_ptr<RunState> Spawn(asio::io_context& io, Awaitable awaitable)
{
    auto state = std::make_shared<RunState>();
    asio::co_spawn(io, std::move(awaitable),
                   [state](std::exception_ptr exception, auto&&...)
                   {
                       state->error = exception;
                       state->done = true;
                   });
    return state;
}

template <typename Awaitable> bool RunFor(asio::io_context& io, Awaitable awaitable, std::chrono::milliseconds limit, std::exception_ptr* failure = nullptr)
{
    auto state = Spawn(io, std::move(awaitable));
    {
        auto guard = asio::make_work_guard(io);
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!state->done.load() && std::chrono::steady_clock::now() < deadline)
        {
            io.run_for(std::chrono::milliseconds(10));
        }
    }
    io.restart();
    io.poll();
    io.restart();

    if (failure)
    {
        *failure = state->error;
    }
    else if (state->error)
    {
        FAIL_CHECK("Scenario failed: " << DescribeException(state->error));
    }
    return state->done.load();
}

template <typename Awaitable> bool RunUntilIdle(asio::io_context& io, Awaitable awaitable)
{
    auto state = Spawn(io, std::move(awaitable));
    io.run();
    io.restart();
    if (state->error)
    {
        FAIL_CHECK("Scenario failed: " << DescribeException(state->error));
    }
    return state->done.load();
}

inline std::vector<std::uint8_t> PatternBytes(std::size_t size, std::uint32_t seed = 0)
{
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        bytes[index] = static_cast<std::uint8_t>((index + seed) % 251);
    }
    return bytes;
}

std::string Sha256Hex(const std::uint8_t* data, std::size_t size);

class Sha256
{
public:
    Sha256();
    void Update(const std::uint8_t* data, std::size_t size);
    std::string FinishHex();

private:
    void Transform(const std::uint8_t* block);

    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t buffer_size_ = 0;
    std::uint64_t total_ = 0;
};

} // namespace justcef::tests
