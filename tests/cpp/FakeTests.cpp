#include "FakeNative.h"
#include "TestSupport.h"

#include "JustCefProcess.h"
#include "Transport.h"

#include <cstdlib>
#include <future>
#include <latch>
#include <map>
#include <mutex>
#include <set>

#ifndef _WIN32
#include <cerrno>
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>

namespace
{

std::atomic<bool> g_fail_reap = false;
std::atomic<int> g_reap_failures = 0;

bool FailReap()
{
    if (!g_fail_reap.load())
    {
        return false;
    }

    ++g_reap_failures;
    errno = EINVAL;
    return true;
}

} // namespace

extern "C" __attribute__((weak)) int waitid(idtype_t idtype, id_t id, siginfo_t* info, int options)
{
    static const auto real = reinterpret_cast<int (*)(idtype_t, id_t, siginfo_t*, int)>(::dlsym(RTLD_NEXT, "waitid"));
    return FailReap() ? -1 : real(idtype, id, info, options);
}

extern "C" __attribute__((weak)) pid_t waitpid(pid_t pid, int* status, int options)
{
    static const auto real = reinterpret_cast<pid_t (*)(pid_t, int*, int)>(::dlsym(RTLD_NEXT, "waitpid"));
    return FailReap() ? -1 : real(pid, status, options);
}
#endif

using namespace justcef;
using namespace justcef::tests;
using namespace std::chrono_literals;

namespace
{

struct Harness
{
    FakeNativeControl fake;
    std::unique_ptr<JustCefProcess> process;

    void Start(asio::any_io_executor executor, const std::function<void(StartOptions&)>& configure = {}, const std::string& extra_arguments = {})
    {
        REQUIRE_MESSAGE(std::filesystem::exists(FakeNativeControl::Executable()),
                        "FakeNative is not built; run dotnet build tests/fake-native/FakeNative.csproj (" << FakeNativeControl::Executable().string() << ")");
        auto options = fake.Options(extra_arguments);
        if (configure)
        {
            configure(options);
        }
        process = std::make_unique<JustCefProcess>(std::move(executor));
        process->Start(options);
        REQUIRE(fake.WaitConnected());
    }
};

template <typename T> T Await(asio::any_io_executor executor, asio::awaitable<T> awaitable, std::chrono::seconds limit = 30s)
{
    auto future = asio::co_spawn(executor, std::move(awaitable), asio::use_future);
    REQUIRE(future.wait_for(limit) == std::future_status::ready);
    return future.get();
}

inline void Await(asio::any_io_executor executor, asio::awaitable<void> awaitable, std::chrono::seconds limit = 30s)
{
    auto future = asio::co_spawn(executor, std::move(awaitable), asio::use_future);
    REQUIRE(future.wait_for(limit) == std::future_status::ready);
    future.get();
}

std::size_t EchoSize()
{
    if (const char* value = std::getenv("JUSTCEF_TEST_ECHO_MB"))
    {
        return static_cast<std::size_t>(std::stoul(value)) * 1024 * 1024;
    }
    return 200 * 1024 * 1024;
}

class CountingStream final : public ByteStream
{
public:
    explicit CountingStream(std::size_t limit) : limit_(limit) {}

    std::size_t Read(std::uint8_t* buffer, std::size_t size) override
    {
        const std::size_t count = std::min(size, limit_ - std::min(limit_, position_));
        for (std::size_t index = 0; index < count; ++index)
        {
            buffer[index] = static_cast<std::uint8_t>((position_ + index) % 251);
        }
        position_ += count;
        return count;
    }

    void Close() override { ++closes; }

    std::atomic<int> closes = 0;

private:
    std::size_t limit_;
    std::size_t position_ = 0;
};

Json Configure(const std::string& opcode, const std::string& behavior, Json extra = Json::object())
{
    Json command = {{"cmd", "configure"}, {"opcode", opcode}, {"behavior", behavior}};
    for (auto& [key, value] : extra.items())
    {
        command[key] = value;
    }
    return command;
}

std::string ToHex(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : bytes)
    {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

std::string RawRequest(detail::OpcodeClient opcode, std::uint32_t request_id, detail::PacketWriter writer)
{
    return ToHex(detail::MakePacket(detail::PacketType::Request, static_cast<std::uint8_t>(opcode), request_id, writer.Release()).Flatten());
}

EventPredicate TracedResponse(std::uint32_t request_id, Status status)
{
    return [request_id, status](const Json& event)
    {
        return event.value("event", std::string()) == "packet" && event.value("type", std::string()) == "Response" &&
               event.value("requestId", std::uint32_t{0}) == request_id && event.value("status", -1) == static_cast<int>(status);
    };
}

void ClobberStack()
{
    volatile std::uint8_t scratch[8192];
    for (std::size_t index = 0; index < sizeof(scratch); ++index)
    {
        scratch[index] = static_cast<std::uint8_t>(0xA5);
    }
}

bool IsSystemError(std::exception_ptr exception, asio::error_code code)
{
    try
    {
        std::rethrow_exception(exception);
    }
    catch (const asio::system_error& error)
    {
        return error.code() == code;
    }
    catch (...)
    {
        return false;
    }
}

bool IsDisconnected(std::exception_ptr exception)
{
    try
    {
        std::rethrow_exception(exception);
    }
    catch (const RemoteError&)
    {
        return false;
    }
    catch (const std::system_error&)
    {
        return false;
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

TEST_CASE("fake C10 WaitUntilLoadedAsync and NavigateAsync complete on a single-threaded io_context")
{
    Watchdog watchdog(60s, "C10");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    bool finished = false;
    const bool done = RunUntilIdle(io,
                                   [&]() -> asio::awaitable<void>
                                   {
                                       co_await harness.process->WaitForReadyAsync();
                                       WindowCreateOptions options;
                                       options.url = "https://fake.invalid/start";
                                       auto window = co_await harness.process->CreateWindowAsync(options);
                                       co_await window->WaitUntilLoadedAsync();
                                       CHECK_FALSE(window->IsLoading());
                                       co_await window->NavigateAsync("https://fake.invalid/next");
                                       CHECK_FALSE(window->IsLoading());
                                       co_await window->WaitUntilLoadedAsync();
                                       co_await window->LoadUrlAsync("https://fake.invalid/third");
                                       co_await window->WaitUntilLoadedAsync();
                                       finished = true;
                                   }());
    CHECK(done);
    CHECK(finished);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake sync waits block only the caller and never need the executor")
{
    Watchdog watchdog(60s, "sync waits");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    harness.process->WaitForReady();

    std::shared_ptr<JustCefWindow> window;
    REQUIRE(RunFor(io,
                   [&]() -> asio::awaitable<void>
                   {
                       WindowCreateOptions options;
                       options.url = "https://fake.invalid/start";
                       window = co_await harness.process->CreateWindowAsync(options);
                   }(),
                   20s));
    REQUIRE(window);

    window->WaitUntilLoaded();
    CHECK_FALSE(window->IsLoading());

    std::thread disposer(
        [&]()
        {
            harness.process->Dispose();
        });
    window->WaitForExit();
    harness.process->WaitForExit();
    disposer.join();
    CHECK(harness.process->HasExited());
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake C11 a 20 MB DevTools result arrives on a single thread")
{
    Watchdog watchdog(60s, "C11");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());
    harness.fake.Command(Configure("WindowExecuteDevToolsMethod", "ok", {{"payloadSize", 20000000}}));

    const bool done = RunUntilIdle(io,
                                   [&]() -> asio::awaitable<void>
                                   {
                                       co_await harness.process->WaitForReadyAsync();
                                       auto window = co_await harness.process->CreateWindowAsync(WindowCreateOptions{});
                                       const auto result = co_await window->ExecuteDevToolsMethodAsync("Runtime.evaluate", std::string("{}"));
                                       CHECK(result.success);
                                       CHECK(result.data.size() == 20000000);
                                       CHECK(result.data.front() == '"');
                                       CHECK(result.data.back() == '"');
                                   }());
    CHECK(done);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake C12 Dispose completes against a peer that stopped reading")
{
    Watchdog watchdog(60s, "C12");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor(),
                  [](StartOptions& options)
                  {
                      options.shutdown_grace_period = 1s;
                  });

    std::exception_ptr echo_error;
    std::atomic<bool> echo_done = false;
    std::chrono::steady_clock::duration elapsed{};

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 harness.fake.Command({{"cmd", "stopReading"}});
                                 asio::co_spawn(io, harness.process->EchoAsync(PatternBytes(16 * 1024 * 1024)),
                                                [&](std::exception_ptr exception)
                                                {
                                                    echo_error = exception;
                                                    echo_done = true;
                                                });
                                 asio::steady_timer timer(io, 300ms);
                                 co_await timer.async_wait(asio::use_awaitable);

                                 const auto start = std::chrono::steady_clock::now();
                                 harness.process->Dispose();
                                 elapsed = std::chrono::steady_clock::now() - start;

                                 for (int attempt = 0; attempt < 100 && !echo_done; ++attempt)
                                 {
                                     timer.expires_after(10ms);
                                     co_await timer.async_wait(asio::use_awaitable);
                                 }
                             }(),
                             20s);
    CHECK(done);
    CHECK(elapsed < 2s);
    CHECK(elapsed >= 900ms);
    CHECK(harness.process->HasExited());
    REQUIRE(echo_done.load());
    CHECK(IsDisconnected(echo_error));
}

TEST_CASE("fake C13 eight threads dispose concurrently")
{
    Watchdog watchdog(60s, "C13");
    asio::thread_pool pool(8);
    Harness harness;
    harness.Start(pool.get_executor(),
                  [](StartOptions& options)
                  {
                      options.shutdown_grace_period = 2s;
                  });

    Await(pool.get_executor(), harness.process->WaitForReadyAsync());
    std::vector<std::shared_ptr<JustCefWindow>> windows;
    std::atomic<int> closes = 0;
    for (int index = 0; index < 4; ++index)
    {
        auto window = Await(pool.get_executor(), harness.process->CreateWindowAsync(WindowCreateOptions{}));
        window->OnClose.Connect(
            [&]()
            {
                ++closes;
            });
        windows.push_back(std::move(window));
    }

    harness.fake.Command(Configure("WindowShow", "hold"));
    auto held = asio::co_spawn(pool.get_executor(), windows.front()->ShowAsync(), asio::use_future);

    std::latch start(8);
    std::vector<std::thread> threads;
    std::vector<std::future<void>> shutdowns(4);
    for (int index = 0; index < 4; ++index)
    {
        threads.emplace_back(
            [&]()
            {
                start.arrive_and_wait();
                harness.process->Dispose();
            });
        threads.emplace_back(
            [&, index]()
            {
                start.arrive_and_wait();
                shutdowns[static_cast<std::size_t>(index)] = asio::post(pool.get_executor(), asio::use_future(
                                                                                                 [&]()
                                                                                                 {
                                                                                                     harness.process->Dispose();
                                                                                                 }));
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    for (auto& shutdown : shutdowns)
    {
        REQUIRE(shutdown.wait_for(10s) == std::future_status::ready);
        shutdown.get();
    }

    REQUIRE(held.wait_for(10s) == std::future_status::ready);
    std::exception_ptr held_error;
    try
    {
        held.get();
    }
    catch (...)
    {
        held_error = std::current_exception();
    }
    CHECK(IsDisconnected(held_error));
    CHECK(harness.process->HasExited());

    for (int attempt = 0; attempt < 200 && closes.load() < 4; ++attempt)
    {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(closes.load() == 4);
    harness.process.reset();
    pool.join();
    CHECK(closes.load() == 4);
}

TEST_CASE("fake C14 a window call after the process object was dropped fails cleanly")
{
    Watchdog watchdog(60s, "C14");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    std::shared_ptr<JustCefWindow> window;
    std::exception_ptr held_error;
    std::atomic<bool> held_done = false;

    REQUIRE(RunFor(io,
                   [&]() -> asio::awaitable<void>
                   {
                       co_await harness.process->WaitForReadyAsync();
                       window = co_await harness.process->CreateWindowAsync(WindowCreateOptions{});
                       harness.fake.Command(Configure("WindowShow", "hold"));
                       asio::co_spawn(io, window->ShowAsync(),
                                      [&](std::exception_ptr exception)
                                      {
                                          held_error = exception;
                                          held_done = true;
                                      });
                       co_await harness.fake.WaitAsync(FakeNativeControl::Named("held"), 5s);
                   }(),
                   20s));

    auto stale = window->MaximizeAsync();
    harness.process.reset();

    std::exception_ptr first;
    std::exception_ptr second;
    std::exception_ptr third;
    REQUIRE(RunFor(io,
                   [&]() -> asio::awaitable<void>
                   {
                       try
                       {
                           co_await std::move(stale);
                       }
                       catch (...)
                       {
                           first = std::current_exception();
                       }
                       asio::steady_timer timer(io);
                       for (int attempt = 0; attempt < 100 && !held_done; ++attempt)
                       {
                           timer.expires_after(10ms);
                           co_await timer.async_wait(asio::use_awaitable);
                       }
                       try
                       {
                           co_await window->SetTitleAsync("after");
                       }
                       catch (...)
                       {
                           second = std::current_exception();
                       }
                       try
                       {
                           co_await window->NavigateAsync("https://fake.invalid/after");
                       }
                       catch (...)
                       {
                           third = std::current_exception();
                       }
                   }(),
                   20s));

    CHECK(IsDisconnected(first));
    CHECK(held_done.load());
    CHECK(IsDisconnected(held_error));
    CHECK(IsDisconnected(second));
    CHECK(IsDisconnected(third));
    CHECK_THROWS_AS(window->Views(), std::runtime_error);
    CHECK_FALSE(window->IsLoading());
}

TEST_CASE("fake C15 handlers and events never run on the reader thread")
{
    Watchdog watchdog(90s, "C15");

    const auto run = [](asio::any_io_executor executor)
    {
        asio::io_context io;
        Harness harness;
        harness.Start(executor);

        std::atomic<int> calls = 0;
        std::atomic<int> on_reader = 0;
        const auto record = [&]()
        {
            ++calls;
            if (detail::Transport::IsTransportThread())
            {
                ++on_reader;
            }
        };

        std::shared_ptr<JustCefWindow> window;
        REQUIRE(RunFor(io,
                       [&]() -> asio::awaitable<void>
                       {
                           co_await harness.process->WaitForReadyAsync();
                           WindowCreateOptions options;
                           options.proxy_requests = true;
                           options.request_proxy = [&](JustCefWindow&, const IPCRequest&) -> asio::awaitable<std::optional<IPCResponse>>
                           {
                               record();
                               IPCResponse response;
                               response.status_code = 200;
                               response.status_text = "OK";
                               response.headers["Content-Length"].push_back("2");
                               response.body_stream = std::make_shared<MemoryByteStream>(std::vector<std::uint8_t>{'o', 'k'});
                               co_return response;
                           };
                           options.modify_requests = true;
                           options.request_modifier = [&](JustCefWindow&, const IPCRequest& request) -> asio::awaitable<std::optional<IPCRequest>>
                           {
                               record();
                               co_return request;
                           };
                           options.bridge_rpc_handler = [&](JustCefWindow&, std::string, std::string json) -> asio::awaitable<std::optional<std::string>>
                           {
                               record();
                               co_return json;
                           };
                           options.view_created_handler = [&](JustCefView&) -> asio::awaitable<void>
                           {
                               record();
                               co_return;
                           };
                           window = co_await harness.process->CreateWindowAsync(options);
                           window->OnFocused.Connect(record);
                           window->OnLoadingStateChanged.Connect(
                               [&](const LoadingStateChangedInfo&)
                               {
                                   record();
                               });
                           window->OnDevToolsEvent.Connect(
                               [&](const std::optional<std::string>&, const std::vector<std::uint8_t>&)
                               {
                                   record();
                               });

                           const int id = window->Identifier();
                           harness.fake.Command({{"cmd", "notify"}, {"opcode", "WindowFocused"}, {"browserId", id}});
                           harness.fake.Command({{"cmd", "notify"}, {"opcode", "WindowLoadingStateChanged"}, {"browserId", id}, {"isLoading", true}});
                           harness.fake.Command({{"cmd", "notify"}, {"opcode", "WindowDevToolsEvent"}, {"browserId", id}});
                           const auto proxy = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", id}});
                           const auto modify = harness.fake.Send({{"cmd", "modifyRequest"}, {"browserId", id}});
                           const auto bridge = harness.fake.Send({{"cmd", "bridgeRpc"}, {"browserId", id}, {"json", "[1]"}});
                           const auto view = harness.fake.Send({{"cmd", "viewCreated"}, {"parentId", id}, {"viewId", 500}});
                           const auto proxy_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("proxyResponse", proxy), 10s);
                           const auto modify_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("modifyResponse", modify), 10s);
                           const auto bridge_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("bridgeRpcResponse", bridge), 10s);
                           const auto view_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("viewCreatedResponse", view), 10s);
                           CHECK(proxy_response.has_value());
                           CHECK(modify_response.has_value());
                           CHECK(bridge_response.has_value());
                           CHECK(view_response.has_value());

                           asio::steady_timer timer(io);
                           for (int attempt = 0; attempt < 200 && calls.load() < 7; ++attempt)
                           {
                               timer.expires_after(10ms);
                               co_await timer.async_wait(asio::use_awaitable);
                           }
                       }(),
                       30s));

        CHECK(calls.load() >= 7);
        CHECK(on_reader.load() == 0);
        CHECK(harness.fake.ViolationCount() == 0);
    };

    SUBCASE("single-threaded io_context")
    {
        asio::io_context context;
        auto guard = asio::make_work_guard(context);
        std::thread thread(
            [&]()
            {
                context.run();
            });
        run(context.get_executor());
        guard.reset();
        context.stop();
        thread.join();
    }

    SUBCASE("thread_pool")
    {
        asio::thread_pool pool(3);
        run(pool.get_executor());
        pool.join();
    }

    SUBCASE("system_executor")
    {
        run(asio::system_executor());
    }
}

TEST_CASE("fake C16 200 MB echo in both directions at once")
{
    Watchdog watchdog(240s, "C16");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor(),
                  [](StartOptions& options)
                  {
                      options.default_call_timeout = 180s;
                  });

    const std::size_t size = EchoSize();
    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 const auto tag = harness.fake.Send({{"cmd", "echo"}, {"size", size}});
                                 auto payload = PatternBytes(size, 7);
                                 co_await harness.process->EchoAsync(std::move(payload));

                                 auto response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("echoResponse", tag), 120s);
                                 REQUIRE(response.has_value());
                                 CHECK((*response)["status"] == 0);
                                 CHECK((*response)["match"] == true);
                                 CHECK((*response)["receivedSize"] == size);
                             }(),
                             200s);
    CHECK(done);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake C1 ten thousand loading events keep wire order on a multi-threaded executor")
{
    Watchdog watchdog(90s, "C1");
    asio::thread_pool pool(4);
    Harness harness;
    harness.Start(pool.get_executor());

    Await(pool.get_executor(), harness.process->WaitForReadyAsync());

    std::mutex mutex;
    std::map<int, std::vector<bool>> sequences;
    std::atomic<int> total = 0;
    std::vector<std::shared_ptr<JustCefWindow>> windows;
    Json ids = Json::array();
    for (int index = 0; index < 20; ++index)
    {
        auto window = Await(pool.get_executor(), harness.process->CreateWindowAsync(WindowCreateOptions{}));
        const int id = window->Identifier();
        window->OnLoadingStateChanged.Connect(
            [&, id](const LoadingStateChangedInfo& info)
            {
                std::this_thread::yield();
                std::lock_guard<std::mutex> lock(mutex);
                sequences[id].push_back(info.is_loading);
                ++total;
            });
        ids.push_back(id);
        windows.push_back(std::move(window));
    }

    harness.fake.Command({{"cmd", "loadingBurst"}, {"browserIds", ids}, {"count", 10000}}, "loadingBurstDone", 30s);
    for (int attempt = 0; attempt < 3000 && total.load() < 10000; ++attempt)
    {
        std::this_thread::sleep_for(10ms);
    }

    CHECK(total.load() == 10000);
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& window : windows)
    {
        const auto& sequence = sequences[window->Identifier()];
        REQUIRE(sequence.size() == 500);
        CHECK_FALSE(sequence.back());
        bool alternating = true;
        for (std::size_t index = 1; index < sequence.size(); ++index)
        {
            alternating = alternating && sequence[index] != sequence[index - 1];
        }
        CHECK(alternating);
        CHECK_FALSE(window->IsLoading());
    }
}

TEST_CASE("fake traffic right after the create reply is routed to the new window")
{
    Watchdog watchdog(60s, "early traffic");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    harness.fake.Command(Configure("WindowCreate", "ok",
                                   {{"afterReply", Json::array({
                                                       {{"action", "notify"}, {"opcode", "WindowLoadingStateChanged"}, {"browserId", "$new"}, {"isLoading", true}},
                                                       {{"action", "notify"}, {"opcode", "WindowFrameLoadStart"}, {"browserId", "$new"}, {"url", "https://app/"}},
                                                       {{"action", "proxyRequest"}, {"browserId", "$new"}, {"url", "https://app/"}, {"tag", "early"}},
                                                       {{"action", "notify"}, {"opcode", "WindowLoadingStateChanged"}, {"browserId", "$new"}, {"isLoading", false}},
                                                   })}}));

    std::vector<bool> loading;
    int frame_starts = 0;
    std::atomic<int> proxied = 0;

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.proxy_requests = true;
                                 options.request_proxy = [&](JustCefWindow&, const IPCRequest& request) -> asio::awaitable<std::optional<IPCResponse>>
                                 {
                                     CHECK(request.url == "https://app/");
                                     ++proxied;
                                     IPCResponse response;
                                     response.status_code = 200;
                                     response.status_text = "OK";
                                     response.headers["Content-Length"].push_back("5");
                                     response.body_stream = std::make_shared<MemoryByteStream>(std::vector<std::uint8_t>{'e', 'a', 'r', 'l', 'y'});
                                     co_return response;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);
                                 window->OnLoadingStateChanged.Connect(
                                     [&](const LoadingStateChangedInfo& info)
                                     {
                                         loading.push_back(info.is_loading);
                                     });
                                 window->OnFrameLoadStart.Connect(
                                     [&](const FrameLoadStartInfo&)
                                     {
                                         ++frame_starts;
                                     });

                                 auto response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("proxyResponse", "early"), 10s);
                                 REQUIRE(response.has_value());
                                 CHECK((*response)["status"] == 0);
                                 CHECK((*response)["statusCode"] == 200);
                                 CHECK((*response)["bodyType"] == 1);
                                 CHECK((*response)["length"] == 5);

                                 asio::steady_timer timer(io);
                                 for (int attempt = 0; attempt < 100 && loading.size() < 2; ++attempt)
                                 {
                                     timer.expires_after(10ms);
                                     co_await timer.async_wait(asio::use_awaitable);
                                 }
                                 CHECK_FALSE(window->IsLoading());
                             }(),
                             20s);
    CHECK(done);
    CHECK(proxied.load() == 1);
    CHECK(loading == std::vector<bool>{true, false});
    CHECK(frame_starts == 1);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake proxy stream of 50 MB respects credits")
{
    Watchdog watchdog(120s, "proxy stream");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    const std::size_t size = 50 * 1024 * 1024;
    const auto payload = PatternBytes(size, 3);
    const auto expected = Sha256Hex(payload.data(), payload.size());

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.proxy_requests = true;
                                 options.request_proxy = [&](JustCefWindow&, const IPCRequest& request) -> asio::awaitable<std::optional<IPCResponse>>
                                 {
                                     IPCResponse response;
                                     response.status_code = 200;
                                     response.status_text = "OK";
                                     response.headers["Content-Type"].push_back("application/octet-stream");
                                     if (request.url == "https://app/known")
                                     {
                                         response.headers["Content-Length"].push_back(std::to_string(size));
                                     }
                                     response.body_stream = std::make_shared<MemoryByteStream>(payload);
                                     co_return response;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);

                                 for (const std::string url : {"https://app/unknown", "https://app/known"})
                                 {
                                     const auto tag = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", window->Identifier()}, {"url", url}, {"readDelayMs", 1}});
                                     auto response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("proxyResponse", tag), 10s);
                                     REQUIRE(response.has_value());
                                     CHECK((*response)["status"] == 0);
                                     CHECK((*response)["bodyType"] == 2);
                                     if (url == "https://app/known")
                                     {
                                         CHECK((*response)["length"] == size);
                                     }
                                     else
                                     {
                                         CHECK((*response)["length"] == -1);
                                     }

                                     auto stream_done = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("streamDone", tag), 90s);
                                     REQUIRE(stream_done.has_value());
                                     CHECK((*stream_done)["result"] == "end");
                                     CHECK((*stream_done)["bytes"] == size);
                                     CHECK((*stream_done)["totalReported"] == size);
                                     CHECK((*stream_done)["sha256"] == expected);
                                 }
                             }(),
                             110s);
    CHECK(done);
    CHECK(harness.fake.Events("creditViolation").empty());
    CHECK(harness.fake.ViolationCount() == 0);
    CHECK(harness.fake.State()["violations"] == 0);
}

TEST_CASE("fake proxy stream canceled by native closes the source exactly once")
{
    Watchdog watchdog(60s, "stream cancel");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    auto source = std::make_shared<CountingStream>(512 * 1024 * 1024);
    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.proxy_requests = true;
                                 options.request_proxy = [&](JustCefWindow&, const IPCRequest&) -> asio::awaitable<std::optional<IPCResponse>>
                                 {
                                     IPCResponse response;
                                     response.status_code = 200;
                                     response.status_text = "OK";
                                     response.body_stream = source;
                                     co_return response;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);

                                 const auto tag = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", window->Identifier()}, {"cancelAfterBytes", 600000}, {"readDelayMs", 2}});
                                 auto stream_done = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("streamDone", tag), 20s);
                                 REQUIRE(stream_done.has_value());
                                 CHECK((*stream_done)["result"] == "canceled");

                                 asio::steady_timer timer(io);
                                 for (int attempt = 0; attempt < 300 && source->closes.load() == 0; ++attempt)
                                 {
                                     timer.expires_after(10ms);
                                     co_await timer.async_wait(asio::use_awaitable);
                                 }
                                 timer.expires_after(300ms);
                                 co_await timer.async_wait(asio::use_awaitable);
                             }(),
                             40s);
    CHECK(done);
    CHECK(source->closes.load() == 1);
    CHECK(harness.fake.Events("creditViolation").empty());
}

class FailingStream final : public ByteStream
{
public:
    explicit FailingStream(std::size_t fail_after) : fail_after_(fail_after) {}

    std::size_t Read(std::uint8_t* buffer, std::size_t size) override
    {
        if (position_ >= fail_after_)
        {
            throw std::runtime_error("source failed");
        }
        const std::size_t count = std::min(size, fail_after_ - position_);
        std::fill_n(buffer, count, static_cast<std::uint8_t>(7));
        position_ += count;
        return count;
    }

    void Close() override { ++closes; }

    std::atomic<int> closes = 0;

private:
    std::size_t fail_after_;
    std::size_t position_ = 0;
};

TEST_CASE("fake proxy stream failures surface as StreamError")
{
    Watchdog watchdog(60s, "stream error");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    auto failing = std::make_shared<FailingStream>(3 * 1024 * 1024);
    auto short_source = std::make_shared<CountingStream>(3 * 1024 * 1024);
    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.proxy_requests = true;
                                 options.request_proxy = [&](JustCefWindow&, const IPCRequest& request) -> asio::awaitable<std::optional<IPCResponse>>
                                 {
                                     IPCResponse response;
                                     response.status_code = 200;
                                     response.status_text = "OK";
                                     if (request.url == "https://app/short")
                                     {
                                         response.headers["Content-Length"].push_back(std::to_string(5 * 1024 * 1024));
                                         response.body_stream = short_source;
                                     }
                                     else
                                     {
                                         response.body_stream = failing;
                                     }
                                     co_return response;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);

                                 const auto failed_tag = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", window->Identifier()}, {"url", "https://app/fail"}});
                                 auto failed = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("streamDone", failed_tag), 20s);
                                 REQUIRE(failed.has_value());
                                 CHECK((*failed)["result"] == "error");
                                 CHECK((*failed)["message"] == "source failed");
                                 CHECK((*failed)["bytes"] == 3 * 1024 * 1024);

                                 const auto short_tag = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", window->Identifier()}, {"url", "https://app/short"}});
                                 auto shortened = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("streamDone", short_tag), 20s);
                                 REQUIRE(shortened.has_value());
                                 CHECK((*shortened)["result"] == "error");

                                 asio::steady_timer timer(io, 100ms);
                                 co_await timer.async_wait(asio::use_awaitable);
                             }(),
                             40s);
    CHECK(done);
    CHECK(failing->closes.load() == 1);
    CHECK(short_source->closes.load() == 1);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake bridge calls in both directions")
{
    Watchdog watchdog(60s, "bridge");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.bridge_rpc_handler = [](JustCefWindow&, std::string method, std::string json) -> asio::awaitable<std::optional<std::string>>
                                 {
                                     if (method == "fail")
                                     {
                                         throw std::runtime_error("bridge failed");
                                     }
                                     if (method == "none")
                                     {
                                         co_return std::nullopt;
                                     }
                                     co_return "{\"echo\":" + json + "}";
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);
                                 const int id = window->Identifier();

                                 const auto ok = harness.fake.Send({{"cmd", "bridgeRpc"}, {"browserId", id}, {"method", "add"}, {"json", "[1,2]"}});
                                 auto ok_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("bridgeRpcResponse", ok), 10s);
                                 REQUIRE(ok_response.has_value());
                                 CHECK((*ok_response)["status"] == 0);
                                 CHECK((*ok_response)["json"] == "{\"echo\":[1,2]}");

                                 const auto fail = harness.fake.Send({{"cmd", "bridgeRpc"}, {"browserId", id}, {"method", "fail"}});
                                 auto fail_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("bridgeRpcResponse", fail), 10s);
                                 REQUIRE(fail_response.has_value());
                                 CHECK((*fail_response)["status"] == 1);
                                 CHECK((*fail_response)["message"] == "bridge failed");

                                 const auto none = harness.fake.Send({{"cmd", "bridgeRpc"}, {"browserId", id}, {"method", "none"}});
                                 auto none_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("bridgeRpcResponse", none), 10s);
                                 REQUIRE(none_response.has_value());
                                 CHECK((*none_response)["status"] == 0);
                                 CHECK((*none_response)["json"] == "null");

                                 const auto missing = harness.fake.Send({{"cmd", "bridgeRpc"}, {"browserId", 999}});
                                 auto missing_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("bridgeRpcResponse", missing), 10s);
                                 REQUIRE(missing_response.has_value());
                                 CHECK((*missing_response)["status"] == 3);

                                 const auto controller_result = co_await window->CallBridgeRpcAsync("controller", std::string("[1]"));
                                 CHECK(controller_result == "null");
                                 harness.fake.Command(Configure("WindowBridgeRpc", "error", {{"message", "js threw"}}));
                                 try
                                 {
                                     (void)co_await window->CallBridgeRpcAsync("controller");
                                     FAIL_CHECK("CallBridgeRpcAsync should have failed");
                                 }
                                 catch (const RemoteError& error)
                                 {
                                     CHECK(error.GetStatus() == Status::Error);
                                     CHECK(std::string(error.what()) == "js threw");
                                 }
                             }(),
                             30s);
    CHECK(done);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake view created handler allows and denies views")
{
    Watchdog watchdog(60s, "views");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.view_created_handler = [](JustCefView& view) -> asio::awaitable<void>
                                 {
                                     if (view.Identifier() == 101)
                                     {
                                         throw std::runtime_error("view denied");
                                     }
                                     if (view.Identifier() == 102)
                                     {
                                         throw std::runtime_error("handler failed");
                                     }
                                     co_return;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);
                                 const int id = window->Identifier();

                                 std::map<int, Json> responses;
                                 for (const int view_id : {100, 101, 102})
                                 {
                                     const auto tag = harness.fake.Send({{"cmd", "viewCreated"}, {"parentId", id}, {"viewId", view_id}, {"src", "https://app/view"}});
                                     auto response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("viewCreatedResponse", tag), 10s);
                                     REQUIRE(response.has_value());
                                     responses[view_id] = *response;
                                 }
                                 CHECK(responses[100]["status"] == 0);
                                 CHECK(responses[100]["allow"] == true);
                                 CHECK(responses[101]["status"] == 0);
                                 CHECK(responses[101]["allow"] == false);
                                 CHECK(responses[102]["status"] == 0);
                                 CHECK(responses[102]["allow"] == false);

                                 const auto views = window->Views();
                                 REQUIRE(views.size() == 1);
                                 CHECK(views.front()->Identifier() == 100);
                                 CHECK(views.front()->Parent() == window);
                                 const auto duplicate = harness.fake.Send({{"cmd", "viewCreated"}, {"parentId", id}, {"viewId", 100}});
                                 auto duplicate_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("viewCreatedResponse", duplicate), 10s);
                                 REQUIRE(duplicate_response.has_value());
                                 CHECK((*duplicate_response)["allow"] == false);
                                 REQUIRE(window->Views().size() == 1);
                                 CHECK(window->Views().front() == views.front());
                                 const double zoom = co_await views.front()->GetZoomAsync();
                                 CHECK(zoom == 0.0);

                                 const auto orphan = harness.fake.Send({{"cmd", "viewCreated"}, {"parentId", 999}, {"viewId", 103}});
                                 auto orphan_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("viewCreatedResponse", orphan), 10s);
                                 REQUIRE(orphan_response.has_value());
                                 CHECK((*orphan_response)["status"] == 3);

                                 bool view_closed = false;
                                 views.front()->OnClose.Connect(
                                     [&]()
                                     {
                                         view_closed = true;
                                     });
                                 co_await window->CloseAsync();
                                 co_await window->WaitForExitAsync();
                                 co_await views.front()->WaitForExitAsync();
                                 asio::steady_timer timer(io, 50ms);
                                 co_await timer.async_wait(asio::use_awaitable);
                                 CHECK(view_closed);
                                 CHECK(window->Views().empty());
                             }(),
                             30s);
    CHECK(done);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake calls time out or cancel and send a Cancel packet")
{
    Watchdog watchdog(60s, "timeouts");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor(),
                  [](StartOptions& options)
                  {
                      options.default_call_timeout = 300ms;
                  });

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 auto window = co_await harness.process->CreateWindowAsync(WindowCreateOptions{});

                                 harness.fake.Command(Configure("WindowShow", "hold"));
                                 const auto start = std::chrono::steady_clock::now();
                                 try
                                 {
                                     co_await window->ShowAsync();
                                     FAIL_CHECK("ShowAsync should have timed out");
                                 }
                                 catch (const asio::system_error& error)
                                 {
                                     CHECK(error.code() == asio::error::timed_out);
                                 }
                                 CHECK(std::chrono::steady_clock::now() - start < 2s);
                                 auto show_cancel = co_await harness.fake.WaitAsync(
                                     [](const Json& event)
                                     {
                                         return event.value("event", std::string()) == "cancelReceived" && event["opcode"] == 20;
                                     },
                                     5s);
                                 REQUIRE(show_cancel.has_value());
                                 CHECK((*show_cancel)["matched"] == true);

                                 harness.fake.Command(Configure("WindowHide", "hold"));
                                 asio::cancellation_signal cancel;
                                 std::exception_ptr hide_error;
                                 bool hide_done = false;
                                 asio::co_spawn(io, window->HideAsync(),
                                                asio::bind_cancellation_slot(cancel.slot(),
                                                                             [&](std::exception_ptr exception)
                                                                             {
                                                                                 hide_error = exception;
                                                                                 hide_done = true;
                                                                             }));
                                 co_await harness.fake.WaitAsync(FakeNativeControl::Named("held"), 5s);
                                 cancel.emit(asio::cancellation_type::terminal);
                                 asio::steady_timer timer(io);
                                 for (int attempt = 0; attempt < 100 && !hide_done; ++attempt)
                                 {
                                     timer.expires_after(5ms);
                                     co_await timer.async_wait(asio::use_awaitable);
                                 }
                                 CHECK(hide_done);
                                 CHECK(IsSystemError(hide_error, asio::error::operation_aborted));
                                 auto hide_cancel = co_await harness.fake.WaitAsync(
                                     [](const Json& event)
                                     {
                                         return event.value("event", std::string()) == "cancelReceived" && event["opcode"] == 21;
                                     },
                                     5s);
                                 CHECK(hide_cancel.has_value());

                                 const auto pick_tag = harness.fake.Command(Configure("PickDirectory", "hold"));
                                 (void)pick_tag;
                                 std::exception_ptr pick_error;
                                 bool pick_done = false;
                                 asio::co_spawn(io, window->PickDirectoryAsync(),
                                                [&](std::exception_ptr exception, std::string)
                                                {
                                                    pick_error = exception;
                                                    pick_done = true;
                                                });
                                 timer.expires_after(800ms);
                                 co_await timer.async_wait(asio::use_awaitable);
                                 CHECK_FALSE(pick_done);
                                 co_await window->CloseAsync(true);
                                 co_await window->WaitForExitAsync();
                                 harness.process->Dispose();
                                 for (int attempt = 0; attempt < 100 && !pick_done; ++attempt)
                                 {
                                     timer.expires_after(5ms);
                                     co_await timer.async_wait(asio::use_awaitable);
                                 }
                                 CHECK(pick_done);
                                 CHECK(IsDisconnected(pick_error));
                             }(),
                             30s);
    CHECK(done);
}

TEST_CASE("fake native Cancel reaches the handler through its cancellation state")
{
    Watchdog watchdog(60s, "handler cancel");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    std::atomic<bool> saw_cancellation = false;
    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.proxy_requests = true;
                                 options.request_proxy = [&](JustCefWindow&, const IPCRequest&) -> asio::awaitable<std::optional<IPCResponse>>
                                 {
                                     asio::steady_timer timer(co_await asio::this_coro::executor, 30s);
                                     bool interrupted = false;
                                     try
                                     {
                                         co_await timer.async_wait(asio::use_awaitable);
                                     }
                                     catch (const asio::system_error&)
                                     {
                                         interrupted = true;
                                     }
                                     if (interrupted)
                                     {
                                         auto state = co_await asio::this_coro::cancellation_state;
                                         saw_cancellation = state.cancelled() != asio::cancellation_type::none;
                                         throw asio::system_error(asio::error::operation_aborted);
                                     }
                                     co_return std::nullopt;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);

                                 const auto tag = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", window->Identifier()}});
                                 asio::steady_timer timer(io, 200ms);
                                 co_await timer.async_wait(asio::use_awaitable);
                                 harness.fake.Command({{"cmd", "cancelLastRequest"}, {"opcode", "WindowProxyRequest"}}, "cancelSent");
                                 auto response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("proxyResponse", tag), 10s);
                                 REQUIRE(response.has_value());
                                 CHECK((*response)["status"] == 2);
                             }(),
                             30s);
    CHECK(done);
    CHECK(saw_cancellation.load());
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake handler failures answer Error and declines answer NotHandled")
{
    Watchdog watchdog(60s, "handler failures");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 WindowCreateOptions options;
                                 options.proxy_requests = true;
                                 options.request_proxy = [](JustCefWindow&, const IPCRequest& request) -> asio::awaitable<std::optional<IPCResponse>>
                                 {
                                     if (request.url.find("throw") != std::string::npos)
                                     {
                                         throw std::runtime_error("proxy exploded");
                                     }
                                     co_return std::nullopt;
                                 };
                                 options.modify_requests = true;
                                 options.request_modifier = [](JustCefWindow&, const IPCRequest& request) -> asio::awaitable<std::optional<IPCRequest>>
                                 {
                                     if (request.url.find("skip") != std::string::npos)
                                     {
                                         co_return std::nullopt;
                                     }
                                     IPCRequest modified = request;
                                     modified.headers["X-Modified"].push_back("yes");
                                     modified.elements.push_back(IPCProxyBodyElement::Bytes({1, 2, 3}));
                                     co_return modified;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);
                                 const int id = window->Identifier();

                                 const auto thrown = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", id}, {"url", "https://app/throw"}});
                                 const auto declined = harness.fake.Send({{"cmd", "proxyRequest"}, {"browserId", id}, {"url", "https://app/decline"}});
                                 const auto skipped = harness.fake.Send({{"cmd", "modifyRequest"}, {"browserId", id}, {"url", "https://app/skip"}});
                                 const auto modified = harness.fake.Send({{"cmd", "modifyRequest"}, {"browserId", id}, {"url", "https://app/change"}, {"bodySize", 10}});

                                 auto thrown_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("proxyResponse", thrown), 10s);
                                 REQUIRE(thrown_response.has_value());
                                 CHECK((*thrown_response)["status"] == 1);
                                 CHECK((*thrown_response)["message"] == "proxy exploded");

                                 auto declined_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("proxyResponse", declined), 10s);
                                 REQUIRE(declined_response.has_value());
                                 CHECK((*declined_response)["status"] == 4);

                                 auto skipped_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("modifyResponse", skipped), 10s);
                                 REQUIRE(skipped_response.has_value());
                                 CHECK((*skipped_response)["status"] == 4);

                                 auto modified_response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("modifyResponse", modified), 10s);
                                 REQUIRE(modified_response.has_value());
                                 CHECK((*modified_response)["status"] == 0);
                                 CHECK((*modified_response)["url"] == "https://app/change");
                                 CHECK((*modified_response)["elements"].size() == 2);
                                 bool header_found = false;
                                 for (auto& [key, value] : (*modified_response)["headers"].items())
                                 {
                                     header_found = header_found || key == "X-Modified";
                                 }
                                 CHECK(header_found);

                                 co_await window->SetTitleAsync("still open");
                             }(),
                             30s);
    CHECK(done);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake response followed by Exit is delivered and closes notify once")
{
    Watchdog watchdog(60s, "exit ordering");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    int closes = 0;
    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 auto window = co_await harness.process->CreateWindowAsync(WindowCreateOptions{});
                                 window->OnClose.Connect(
                                     [&]()
                                     {
                                         ++closes;
                                     });
                                 harness.fake.Command(Configure("WindowShow", "ok", {{"afterReply", Json::array({{{"action", "notify"}, {"opcode", "Exit"}}})}}));
                                 co_await window->ShowAsync();
                                 co_await harness.process->PingAsync();

                                 co_await window->CloseAsync();
                                 co_await window->WaitForExitAsync();
                                 try
                                 {
                                     co_await window->ShowAsync();
                                     FAIL_CHECK("ShowAsync on a closed window should fail");
                                 }
                                 catch (const RemoteError& error)
                                 {
                                     CHECK(error.GetStatus() == Status::NotFound);
                                 }
                                 CHECK(harness.process->GetWindow(window->Identifier()) == nullptr);

                                 harness.process->Dispose();
                                 CHECK(harness.process->HasExited());
                                 asio::steady_timer timer(io, 50ms);
                                 co_await timer.async_wait(asio::use_awaitable);
                             }(),
                             30s);
    CHECK(done);
    CHECK(closes == 1);
    CHECK(harness.fake.Wait(FakeNativeControl::Named("exiting"), 5s).has_value());
}

TEST_CASE("fake C8 pending calls fail when the child exits while a grandchild holds the pipes")
{
    Watchdog watchdog(60s, "C8");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    std::exception_ptr held_error;
    std::atomic<bool> held_done = false;
    std::chrono::steady_clock::duration latency{};
    int grandchild = 0;

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 auto window = co_await harness.process->CreateWindowAsync(WindowCreateOptions{});
                                 harness.fake.Command(Configure("WindowShow", "hold"));
                                 asio::co_spawn(io, window->ShowAsync(),
                                                [&](std::exception_ptr exception)
                                                {
                                                    held_error = exception;
                                                    held_done = true;
                                                });
                                 co_await harness.fake.WaitAsync(FakeNativeControl::Named("held"), 5s);

                                 const auto tag = harness.fake.Send({{"cmd", "spawnGrandchildAndExit"}, {"seconds", 10}});
                                 auto spawned = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("grandchildSpawned", tag), 10s);
                                 REQUIRE(spawned.has_value());
                                 grandchild = (*spawned)["pid"].get<int>();
                                 const auto start = std::chrono::steady_clock::now();
                                 asio::steady_timer timer(io);
                                 for (int attempt = 0; attempt < 400 && !held_done; ++attempt)
                                 {
                                     timer.expires_after(5ms);
                                     co_await timer.async_wait(asio::use_awaitable);
                                 }
                                 latency = std::chrono::steady_clock::now() - start;
                             }(),
                             30s);

#ifndef _WIN32
    if (grandchild > 0)
    {
        ::kill(grandchild, SIGKILL);
    }
#endif
    CHECK(done);
    CHECK(held_done.load());
    CHECK(IsDisconnected(held_error));
    CHECK(latency < 2s);
    CHECK(harness.process->HasExited());
}

TEST_CASE("fake the CreateWindowAsync argument overload keeps its options alive")
{
    Watchdog watchdog(60s, "create window options");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    std::shared_ptr<JustCefWindow> window;
    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 auto pending = harness.process->CreateWindowAsync(
                                     "https://fake.invalid/created", 320, 240, 800, 600, false, false, true, false, true, false, true, false, false, RequestProxy{}, false,
                                     RequestModifier{}, false, std::string("Created"), std::nullopt, std::nullopt, true,
                                     [](JustCefWindow&, std::string, std::string json) -> asio::awaitable<std::optional<std::string>>
                                     {
                                         co_return json;
                                     });
                                 ClobberStack();
                                 window = co_await std::move(pending);
                                 co_await window->WaitUntilLoadedAsync();

                                 const auto tag = harness.fake.Send({{"cmd", "bridgeRpc"}, {"browserId", window->Identifier()}, {"json", "[5]"}});
                                 auto response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("bridgeRpcResponse", tag), 10s);
                                 REQUIRE(response.has_value());
                                 CHECK((*response)["status"] == 0);
                                 CHECK((*response)["json"] == "[5]");
                             }(),
                             30s);
    CHECK(done);
    REQUIRE(window);

    bool found = false;
    const Json state = harness.fake.State();
    for (const auto& entry : state["windows"])
    {
        if (entry["id"] == window->Identifier())
        {
            found = true;
            CHECK(entry["url"] == "https://fake.invalid/created");
        }
    }
    CHECK(found);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake a reused in-flight request id is answered InvalidRequest and leaves the first request alone")
{
    Watchdog watchdog(60s, "duplicate request id");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    constexpr std::uint32_t kRequestId = 900001;
    std::atomic<int> calls = 0;
    std::atomic<bool> release = false;

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 harness.fake.Command({{"cmd", "trace"}, {"on", true}});

                                 WindowCreateOptions options;
                                 options.bridge_rpc_handler = [&](JustCefWindow&, std::string, std::string json) -> asio::awaitable<std::optional<std::string>>
                                 {
                                     ++calls;
                                     asio::steady_timer timer(co_await asio::this_coro::executor);
                                     for (int attempt = 0; attempt < 1000 && !release.load(); ++attempt)
                                     {
                                         timer.expires_after(5ms);
                                         co_await timer.async_wait(asio::use_awaitable);
                                     }
                                     co_return json;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);

                                 detail::PacketWriter body;
                                 body.Write<std::int32_t>(window->Identifier()).WriteSizePrefixedString(std::string("held"));
                                 body.Write<std::uint32_t>(4u).WriteString(std::string("null"));
                                 const std::string hex = RawRequest(detail::OpcodeClient::WindowBridgeRpc, kRequestId, std::move(body));
                                 harness.fake.Command({{"cmd", "sendRaw"}, {"hex", hex}});
                                 harness.fake.Command({{"cmd", "sendRaw"}, {"hex", hex}});

                                 auto rejected = co_await harness.fake.WaitAsync(TracedResponse(kRequestId, Status::InvalidRequest), 10s);
                                 CHECK(rejected.has_value());
                                 CHECK(calls.load() == 1);

                                 release = true;
                                 auto accepted = co_await harness.fake.WaitAsync(TracedResponse(kRequestId, Status::Ok), 10s);
                                 CHECK(accepted.has_value());
                                 CHECK(calls.load() == 1);
                             }(),
                             40s);
    CHECK(done);
}

TEST_CASE("fake counts larger than the packet body are rejected without allocating")
{
    Watchdog watchdog(60s, "wire counts");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    constexpr std::uint32_t kRequestId = 900002;
    std::atomic<int> proxied = 0;
    std::exception_ptr picker_error;

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 harness.fake.Command({{"cmd", "trace"}, {"on", true}});

                                 WindowCreateOptions options;
                                 options.proxy_requests = true;
                                 options.request_proxy = [&](JustCefWindow&, const IPCRequest&) -> asio::awaitable<std::optional<IPCResponse>>
                                 {
                                     ++proxied;
                                     co_return std::nullopt;
                                 };
                                 auto window = co_await harness.process->CreateWindowAsync(options);

                                 detail::PacketWriter body;
                                 body.Write<std::int32_t>(window->Identifier());
                                 body.WriteSizePrefixedString(std::string("GET")).WriteSizePrefixedString(std::string("https://app/huge"));
                                 body.Write<std::int32_t>(0).Write<std::uint32_t>(0xFFFFFFFFu);
                                 harness.fake.Command({{"cmd", "sendRaw"}, {"hex", RawRequest(detail::OpcodeClient::WindowProxyRequest, kRequestId, std::move(body))}});

                                 auto rejected = co_await harness.fake.WaitAsync(TracedResponse(kRequestId, Status::InvalidRequest), 10s);
                                 CHECK(rejected.has_value());
                                 CHECK(proxied.load() == 0);

                                 harness.fake.Command(Configure("PickFile", "hold"));
                                 bool picker_done = false;
                                 asio::co_spawn(io, window->PickFileAsync(false, {}),
                                                [&](std::exception_ptr exception, std::vector<std::string>)
                                                {
                                                    picker_error = exception;
                                                    picker_done = true;
                                                });

                                 auto sent = co_await harness.fake.WaitAsync(
                                     [](const Json& event)
                                     {
                                         return event.value("event", std::string()) == "packet" && event.value("type", std::string()) == "Request" &&
                                                event.value("opcode", -1) == static_cast<int>(detail::OpcodeController::PickFile);
                                     },
                                     10s);
                                 REQUIRE(sent.has_value());

                                 detail::PacketWriter payload;
                                 payload.Write<std::uint32_t>(0xFFFFFFFFu);
                                 const auto response = detail::MakeOkResponse(static_cast<std::uint8_t>(detail::OpcodeController::PickFile),
                                                                              (*sent)["requestId"].get<std::uint32_t>(), payload.Release());
                                 harness.fake.Command({{"cmd", "sendRaw"}, {"hex", ToHex(response.Flatten())}});

                                 asio::steady_timer timer(io);
                                 for (int attempt = 0; attempt < 200 && !picker_done; ++attempt)
                                 {
                                     timer.expires_after(10ms);
                                     co_await timer.async_wait(asio::use_awaitable);
                                 }
                                 CHECK(picker_done);
                             }(),
                             40s);
    CHECK(done);
    REQUIRE(picker_error != nullptr);
    CHECK_THROWS_AS(std::rethrow_exception(picker_error), detail::ProtocolError);
}

TEST_CASE("fake a modify request without a registered modifier answers NotHandled")
{
    Watchdog watchdog(60s, "modify not handled");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor());

    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 co_await harness.process->WaitForReadyAsync();
                                 auto window = co_await harness.process->CreateWindowAsync(WindowCreateOptions{});
                                 const auto tag = harness.fake.Send({{"cmd", "modifyRequest"}, {"browserId", window->Identifier()}, {"url", "https://app/unmodified"}});
                                 auto response = co_await harness.fake.WaitAsync(FakeNativeControl::Tagged("modifyResponse", tag), 10s);
                                 REQUIRE(response.has_value());
                                 CHECK((*response)["status"] == 4);
                             }(),
                             30s);
    CHECK(done);
    CHECK(harness.fake.ViolationCount() == 0);
}

TEST_CASE("fake a view created after its parent closed is never registered")
{
    Watchdog watchdog(120s, "view after close");
    asio::thread_pool pool(4);
    Harness harness;
    harness.Start(pool.get_executor());

    Await(pool.get_executor(), harness.process->WaitForReadyAsync());

    std::vector<std::shared_ptr<JustCefWindow>> windows;
    for (int index = 0; index < 20; ++index)
    {
        WindowCreateOptions options;
        options.view_created_handler = [](JustCefView&) -> asio::awaitable<void>
        {
            co_return;
        };
        auto window = Await(pool.get_executor(), harness.process->CreateWindowAsync(options));
        const int id = window->Identifier();
        harness.fake.Send({{"cmd", "notify"}, {"opcode", "WindowClosed"}, {"browserId", id}});
        Await(pool.get_executor(), window->WaitForExitAsync());

        const auto tag = harness.fake.Send({{"cmd", "viewCreated"}, {"parentId", id}, {"viewId", 100000 + index}});
        auto response = harness.fake.Wait(FakeNativeControl::Tagged("viewCreatedResponse", tag), 10s);
        REQUIRE(response.has_value());
        CHECK((*response)["status"] == 3);
        windows.push_back(std::move(window));
    }

    int registered = 0;
    for (const auto& window : windows)
    {
        registered += static_cast<int>(window->Views().size());
    }
    CHECK(registered == 0);
    harness.process->Dispose();
    pool.join();
}

#ifndef _WIN32

TEST_CASE("fake a child that could not be reaped is still killed by dispose")
{
    Watchdog watchdog(60s, "unreaped child");
    asio::io_context io;
    int pid = 0;
    int status = 0;
    pid_t reaped = 0;

    {
        Harness harness;
        g_reap_failures = 0;
        g_fail_reap = true;
        harness.Start(io.get_executor());
        auto hello = harness.fake.Wait(FakeNativeControl::Named("hello"), 10s);
        std::this_thread::sleep_for(500ms);
        g_fail_reap = false;
        REQUIRE(hello.has_value());
        pid = (*hello)["pid"].get<int>();

        if (g_reap_failures.load() == 0)
        {
            MESSAGE("waitid and waitpid are intercepted by the sanitizer runtime; the reap failure cannot be injected here.");
            harness.process->Dispose();
            return;
        }

        harness.fake.Command({{"cmd", "stopReading"}});
        harness.process->Dispose();

        for (int attempt = 0; attempt < 500 && reaped == 0; ++attempt)
        {
            reaped = ::waitpid(pid, &status, WNOHANG);
            if (reaped == 0)
            {
                std::this_thread::sleep_for(10ms);
            }
        }
    }

    CHECK(reaped == pid);
    CHECK(WIFSIGNALED(status));
    if (reaped != pid)
    {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
}

#endif

TEST_CASE("fake WaitForReadyAsync rejects a protocol version mismatch")
{
    Watchdog watchdog(60s, "version");
    asio::io_context io;
    Harness harness;
    harness.Start(io.get_executor(), {}, "--fake-ready-version 1");

    std::string message;
    const bool done = RunFor(io,
                             [&]() -> asio::awaitable<void>
                             {
                                 try
                                 {
                                     co_await harness.process->WaitForReadyAsync();
                                 }
                                 catch (const std::exception& error)
                                 {
                                     message = error.what();
                                 }
                             }(),
                             20s);
    CHECK(done);
    CHECK(message.find("protocol version 1") != std::string::npos);
}
