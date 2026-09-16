#include "FakeNative.h"

#include <cstdlib>
#include <future>
#include <iostream>

namespace justcef::tests
{

FakeNativeControl::FakeNativeControl() : acceptor_(io_), socket_(io_)
{
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
    acceptor_.open(endpoint.protocol());
    acceptor_.bind(endpoint);
    acceptor_.listen(1);
    port_ = acceptor_.local_endpoint().port();

    acceptor_.async_accept(socket_,
                           [this](const asio::error_code& error)
                           {
                               if (error)
                               {
                                   return;
                               }
                               {
                                   std::lock_guard<std::mutex> lock(mutex_);
                                   connected_ = true;
                               }
                               condition_.notify_all();
                               StartRead();
                           });

    thread_ = std::thread(
        [this]()
        {
            io_.run();
        });
}

FakeNativeControl::~FakeNativeControl()
{
    asio::post(io_,
               [this]()
               {
                   asio::error_code ignored;
                   acceptor_.close(ignored);
                   socket_.close(ignored);
               });
    io_.stop();
    thread_.join();
}

std::filesystem::path FakeNativeControl::Executable()
{
    if (const char* overridden = std::getenv("JUSTCEF_FAKE_NATIVE"))
    {
        return overridden;
    }
#ifdef _WIN32
    const char* name = "fake-native.exe";
#else
    const char* name = "fake-native";
#endif
    return std::filesystem::path(JUSTCEF_TESTS_REPO_ROOT) / "tests" / "fake-native" / "bin" / "Debug" / "net8.0" / name;
}

StartOptions FakeNativeControl::Options(const std::string& extra_arguments) const
{
    StartOptions options;
    options.native_executable_path = Executable();
    options.arguments = "--fake-control-port " + std::to_string(port_);
    if (!extra_arguments.empty())
    {
        options.arguments += " " + extra_arguments;
    }
    return options;
}

void FakeNativeControl::StartRead()
{
    asio::async_read_until(socket_, buffer_, '\n',
                           [this](const asio::error_code& error, std::size_t size)
                           {
                               if (error)
                               {
                                   return;
                               }

                               std::string line(asio::buffers_begin(buffer_.data()), asio::buffers_begin(buffer_.data()) + static_cast<std::ptrdiff_t>(size));
                               buffer_.consume(size);
                               while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                               {
                                   line.pop_back();
                               }

                               if (!line.empty())
                               {
                                   try
                                   {
                                       auto event = Json::parse(line);
                                       std::lock_guard<std::mutex> lock(mutex_);
                                       events_.push_back(std::move(event));
                                   }
                                   catch (const std::exception& exception)
                                   {
                                       std::cerr << "FakeNative sent invalid JSON: " << exception.what() << std::endl;
                                   }
                                   condition_.notify_all();
                               }
                               StartRead();
                           });
}

void FakeNativeControl::Write(std::string line)
{
    std::promise<void> written;
    auto future = written.get_future();
    asio::post(io_,
               [this, &line, &written]()
               {
                   asio::error_code error;
                   asio::write(socket_, asio::buffer(line), error);
                   written.set_value();
               });
    future.wait();
}

bool FakeNativeControl::WaitConnected(std::chrono::milliseconds timeout)
{
    return Wait(Named("hello"), timeout).has_value();
}

std::string FakeNativeControl::Send(Json command)
{
    std::string tag;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tag = "t" + std::to_string(++next_tag_);
    }
    if (!command.contains("tag"))
    {
        command["tag"] = tag;
    }
    else
    {
        tag = command["tag"].get<std::string>();
    }
    Write(command.dump() + "\n");
    return tag;
}

Json FakeNativeControl::Command(Json command, const std::string& expected_event, std::chrono::milliseconds timeout)
{
    const std::string name = command.value("cmd", std::string());
    const std::string tag = Send(std::move(command));
    auto event = Wait(
        [&](const Json& candidate)
        {
            const auto event_name = candidate.value("event", std::string());
            return candidate.contains("tag") && candidate["tag"] == tag && (event_name == expected_event || event_name == "error");
        },
        timeout);
    if (!event)
    {
        throw std::runtime_error("FakeNative did not answer command '" + name + "' with '" + expected_event + "'.");
    }
    if (event->value("event", std::string()) == "error" && expected_event != "error")
    {
        throw std::runtime_error("FakeNative rejected command '" + name + "': " + event->dump());
    }
    return *event;
}

std::optional<Json> FakeNativeControl::Find(const EventPredicate& predicate) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& event : events_)
    {
        if (predicate(event))
        {
            return event;
        }
    }
    return std::nullopt;
}

std::optional<Json> FakeNativeControl::Wait(const EventPredicate& predicate, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    std::size_t checked = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        for (; checked < events_.size(); ++checked)
        {
            if (predicate(events_[checked]))
            {
                return events_[checked];
            }
        }
        if (condition_.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            for (; checked < events_.size(); ++checked)
            {
                if (predicate(events_[checked]))
                {
                    return events_[checked];
                }
            }
            return std::nullopt;
        }
    }
}

asio::awaitable<std::optional<Json>> FakeNativeControl::WaitAsync(EventPredicate predicate, std::chrono::milliseconds timeout)
{
    asio::steady_timer timer(co_await asio::this_coro::executor);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        if (auto event = Find(predicate))
        {
            co_return event;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            co_return std::nullopt;
        }
        timer.expires_after(std::chrono::milliseconds(5));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

std::vector<Json> FakeNativeControl::Events(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Json> matching;
    for (const auto& event : events_)
    {
        if (event.value("event", std::string()) == name)
        {
            matching.push_back(event);
        }
    }
    return matching;
}

int FakeNativeControl::ViolationCount() const
{
    return static_cast<int>(Events("violation").size());
}

Json FakeNativeControl::State()
{
    return Command(Json{{"cmd", "state"}}, "state");
}

EventPredicate FakeNativeControl::Named(const std::string& name)
{
    return [name](const Json& event)
    {
        return event.value("event", std::string()) == name;
    };
}

EventPredicate FakeNativeControl::Tagged(const std::string& name, const std::string& tag)
{
    return [name, tag](const Json& event)
    {
        return event.value("event", std::string()) == name && event.contains("tag") && event["tag"] == tag;
    };
}

} // namespace justcef::tests
