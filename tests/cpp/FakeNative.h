#pragma once

#include "JustCefProcess.h"
#include "json.hpp"

#include <asio.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace justcef::tests
{

using Json = nlohmann::json;
using EventPredicate = std::function<bool(const Json&)>;

class FakeNativeControl
{
public:
    FakeNativeControl();
    ~FakeNativeControl();

    FakeNativeControl(const FakeNativeControl&) = delete;
    FakeNativeControl& operator=(const FakeNativeControl&) = delete;

    static std::filesystem::path Executable();

    unsigned short Port() const { return port_; }
    StartOptions Options(const std::string& extra_arguments = {}) const;

    bool WaitConnected(std::chrono::milliseconds timeout = std::chrono::seconds(20));
    std::string Send(Json command);
    Json Command(Json command, const std::string& expected_event = "ack", std::chrono::milliseconds timeout = std::chrono::seconds(10));

    std::optional<Json> Find(const EventPredicate& predicate) const;
    std::optional<Json> Wait(const EventPredicate& predicate, std::chrono::milliseconds timeout);
    asio::awaitable<std::optional<Json>> WaitAsync(EventPredicate predicate, std::chrono::milliseconds timeout);
    std::vector<Json> Events(const std::string& name) const;
    int ViolationCount() const;
    Json State();

    static EventPredicate Named(const std::string& name);
    static EventPredicate Tagged(const std::string& name, const std::string& tag);

private:
    void StartRead();
    void Write(std::string line);

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::streambuf buffer_;
    unsigned short port_ = 0;
    std::thread thread_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Json> events_;
    bool connected_ = false;
    std::uint64_t next_tag_ = 0;
};

} // namespace justcef::tests
