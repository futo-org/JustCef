#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "endpoint.h"
#include "event_loop.h"
#include "stream_receiver.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <future>
#include <mutex>
#include <numeric>
#include <pthread.h>
#include <random>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace
{

struct PipePair
{
    int aRead, aWrite, bRead, bWrite;

    PipePair()
    {
        int ab[2];
        int ba[2];
        REQUIRE(pipe(ab) == 0);
        REQUIRE(pipe(ba) == 0);
        aRead = ba[0];
        bWrite = ba[1];
        bRead = ab[0];
        aWrite = ab[1];
    }
};

struct Collector
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ipc::Packet> packets;
    std::atomic<bool> closed{false};

    ipc::Transport::PacketHandler Handler()
    {
        return [this](ipc::Packet&& packet)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                packets.push_back(std::move(packet));
            }
            changed.notify_all();
        };
    }

    ipc::Transport::ClosedHandler ClosedHandler()
    {
        return [this]
        {
            closed = true;
            changed.notify_all();
        };
    }

    bool WaitFor(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return packets.size() >= count; });
    }

    bool WaitClosed(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return closed.load(); });
    }
};

std::vector<uint8_t> Bytes(size_t size, uint32_t seed)
{
    std::vector<uint8_t> data(size);
    std::mt19937 random(seed);
    for (auto& byte : data)
        byte = static_cast<uint8_t>(random());
    return data;
}

struct Peer
{
    ipc::Transport transport;
    ipc::EventLoop loop;
    std::shared_ptr<ipc::Endpoint> endpoint;

    void Start(int readFd, int writeFd)
    {
        loop.Start();
        endpoint = ipc::Endpoint::Create(transport, loop);
        std::weak_ptr<ipc::Endpoint> weak = endpoint;
        ipc::EventLoop* loopPtr = &loop;
        transport.SetHandles(readFd, writeFd);
        REQUIRE(transport.Start(
            [weak, loopPtr](ipc::Packet&& packet)
            {
                auto shared = std::make_shared<ipc::Packet>(std::move(packet));
                loopPtr->Post(
                    [weak, shared]
                    {
                        if (auto endpoint = weak.lock())
                            endpoint->HandlePacket(std::move(*shared));
                    });
            },
            [weak, loopPtr]
            {
                loopPtr->Post(
                    [weak]
                    {
                        if (auto endpoint = weak.lock())
                            endpoint->Close();
                    });
            }));
    }

    void Stop()
    {
        transport.Shutdown(200ms);
        loop.Post([endpoint = endpoint] { endpoint->Close(); });
        loop.Stop();
    }
};

std::vector<uint8_t> OkPayload(const ipc::Response& response)
{
    return response.payload;
}

} // namespace

TEST_CASE("transport delivers packets in order with intact bodies")
{
    PipePair pipes;
    ipc::Transport a;
    ipc::Transport b;
    Collector received;
    Collector unused;
    a.SetHandles(pipes.aRead, pipes.aWrite);
    REQUIRE(a.Start(unused.Handler(), unused.ClosedHandler()));
    b.SetHandles(pipes.bRead, pipes.bWrite);
    REQUIRE(b.Start(received.Handler(), received.ClosedHandler()));

    std::vector<std::vector<uint8_t>> sent;
    for (uint32_t i = 0; i < 2000; ++i)
    {
        sent.push_back(Bytes(i % 97, i));
        REQUIRE(a.Enqueue(ipc::PacketType::Notification, static_cast<uint8_t>(i % 200), i + 1, sent.back()));
    }
    auto large = Bytes(50 * 1024 * 1024, 42);
    REQUIRE(a.Enqueue(ipc::PacketType::Request, 7, 99999, large));

    REQUIRE(received.WaitFor(2001, 20s));
    for (uint32_t i = 0; i < 2000; ++i)
    {
        CHECK(received.packets[i].requestId == i + 1);
        CHECK(received.packets[i].opcode == static_cast<uint8_t>(i % 200));
        CHECK(received.packets[i].body == sent[i]);
    }
    CHECK(received.packets[2000].type == ipc::PacketType::Request);
    CHECK(received.packets[2000].body == large);

    a.Shutdown(1s);
    REQUIRE(received.WaitClosed(5s));
    b.Shutdown(0ms);
}

TEST_CASE("transport reports EOF and rejects oversized headers")
{
    PipePair pipes;
    ipc::Transport b;
    Collector received;
    b.SetHandles(pipes.bRead, pipes.bWrite);
    REQUIRE(b.Start(received.Handler(), received.ClosedHandler()));

    uint8_t header[ipc::kHeaderSize] = {};
    const uint32_t hugeLength = static_cast<uint32_t>(ipc::kMaxBodySize + 100);
    std::memcpy(header, &hugeLength, sizeof(uint32_t));
    REQUIRE(write(pipes.aWrite, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header)));
    REQUIRE(received.WaitClosed(5s));
    CHECK(received.packets.empty());
    CHECK_FALSE(b.Enqueue(ipc::PacketType::Notification, 1, 0, nullptr, 0));
    b.Shutdown(0ms);
    close(pipes.aWrite);
    close(pipes.aRead);
}

TEST_CASE("transport shutdown completes while the peer never reads")
{
    PipePair pipes;
    ipc::Transport a;
    Collector unused;
    a.SetHandles(pipes.aRead, pipes.aWrite);
    REQUIRE(a.Start(unused.Handler(), unused.ClosedHandler()));

    auto large = Bytes(8 * 1024 * 1024, 5);
    for (int i = 0; i < 4; ++i)
        REQUIRE(a.Enqueue(ipc::PacketType::Notification, 1, 0, large));

    const auto started = std::chrono::steady_clock::now();
    a.Shutdown(300ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < 2s);
    close(pipes.bRead);
    close(pipes.bWrite);
}

TEST_CASE("transport survives signals interrupting reads and writes")
{
    struct sigaction action = {};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGUSR1, &action, nullptr);

    PipePair pipes;
    ipc::Transport a;
    ipc::Transport b;
    Collector received;
    Collector unused;
    a.SetHandles(pipes.aRead, pipes.aWrite);
    REQUIRE(a.Start(unused.Handler(), unused.ClosedHandler()));
    b.SetHandles(pipes.bRead, pipes.bWrite);
    REQUIRE(b.Start(received.Handler(), received.ClosedHandler()));

    std::atomic<bool> stop{false};
    std::thread noise(
        [&]
        {
            while (!stop)
            {
                kill(getpid(), SIGUSR1);
                std::this_thread::sleep_for(100us);
            }
        });

    auto payload = Bytes(4 * 1024 * 1024, 9);
    for (int i = 0; i < 10; ++i)
        REQUIRE(a.Enqueue(ipc::PacketType::Notification, 3, 0, payload));
    REQUIRE(received.WaitFor(10, 30s));
    stop = true;
    noise.join();
    for (auto& packet : received.packets)
        CHECK(packet.body == payload);

    a.Shutdown(1s);
    b.Shutdown(0ms);
}

TEST_CASE("event loop runs tasks in order and fires timers")
{
    ipc::EventLoop loop;
    loop.Start();
    std::mutex mutex;
    std::vector<int> order;
    std::promise<void> done;
    for (int i = 0; i < 100; ++i)
        loop.Post([&, i] { std::lock_guard<std::mutex> lock(mutex); order.push_back(i); });

    auto canceled = loop.PostDelayed(20ms, [&] { std::lock_guard<std::mutex> lock(mutex); order.push_back(-1); });
    loop.CancelTimer(canceled);
    loop.PostDelayed(40ms, [&] { done.set_value(); });
    REQUIRE(done.get_future().wait_for(2s) == std::future_status::ready);

    std::vector<int> expected(100);
    std::iota(expected.begin(), expected.end(), 0);
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(order == expected);
    loop.Stop();
    CHECK_FALSE(loop.Post([] {}));
}

TEST_CASE("endpoint calls, replies and notifications")
{
    PipePair pipes;
    Peer a;
    Peer b;
    b.endpoint = nullptr;
    a.Start(pipes.aRead, pipes.aWrite);
    b.Start(pipes.bRead, pipes.bWrite);

    std::promise<std::pair<uint8_t, std::vector<uint8_t>>> notified;
    b.endpoint->SetNotificationHandler([&](uint8_t opcode, std::vector<uint8_t> body) { notified.set_value({opcode, std::move(body)}); });
    b.endpoint->SetRequestHandler(
        [](ipc::IncomingRequest request, ipc::Reply reply)
        {
            if (request.opcode == 1)
            {
                std::vector<uint8_t> echoed = request.body;
                reply.Ok(echoed);
            }
            else if (request.opcode == 2)
            {
                reply.Fail(ipc::StatusCode::NotFound, "missing");
            }
        });

    std::promise<ipc::Response> first;
    a.endpoint->Call(1, {1, 2, 3}, 5s, [&](ipc::Response response) { first.set_value(std::move(response)); });
    auto response = first.get_future().get();
    CHECK(response.status == ipc::StatusCode::Ok);
    CHECK(OkPayload(response) == std::vector<uint8_t>{1, 2, 3});

    std::promise<ipc::Response> second;
    a.endpoint->Call(2, {}, 5s, [&](ipc::Response response) { second.set_value(std::move(response)); });
    response = second.get_future().get();
    CHECK(response.status == ipc::StatusCode::NotFound);
    CHECK(response.message == "missing");

    std::promise<ipc::Response> dropped;
    a.endpoint->Call(3, {}, 5s, [&](ipc::Response response) { dropped.set_value(std::move(response)); });
    response = dropped.get_future().get();
    CHECK(response.status == ipc::StatusCode::Error);

    a.endpoint->Notify(9, std::vector<uint8_t>{4, 5});
    auto note = notified.get_future().get();
    CHECK(note.first == 9);
    CHECK(note.second == std::vector<uint8_t>{4, 5});

    a.Stop();
    b.Stop();
}

TEST_CASE("endpoint timeouts send cancel and late responses are dropped")
{
    PipePair pipes;
    Peer a;
    Peer b;
    a.Start(pipes.aRead, pipes.aWrite);
    b.Start(pipes.bRead, pipes.bWrite);

    std::promise<void> canceledOnPeer;
    std::shared_ptr<ipc::Reply> held;
    b.endpoint->SetRequestHandler(
        [&](ipc::IncomingRequest request, ipc::Reply reply)
        {
            if (request.opcode != 5)
                return;
            held = std::make_shared<ipc::Reply>(std::move(reply));
            held->OnCanceled([&] { canceledOnPeer.set_value(); });
        });

    std::promise<ipc::Response> timedOut;
    const auto started = std::chrono::steady_clock::now();
    a.endpoint->Call(5, {}, 150ms, [&](ipc::Response response) { timedOut.set_value(std::move(response)); });
    auto response = timedOut.get_future().get();
    CHECK(response.status == ipc::StatusCode::Timeout);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    REQUIRE(canceledOnPeer.get_future().wait_for(2s) == std::future_status::ready);
    CHECK(held->IsCanceled());

    std::promise<void> loopIdle;
    held->Ok(std::vector<uint8_t>{1});
    std::this_thread::sleep_for(100ms);
    a.loop.Post([&] { loopIdle.set_value(); });
    loopIdle.get_future().wait();
    CHECK(a.endpoint->PendingCallCount() == 0);

    std::promise<ipc::Response> canceled;
    auto handle = a.endpoint->Call(6, {}, 0ms, [&](ipc::Response response) { canceled.set_value(std::move(response)); });
    handle.Cancel();
    CHECK(canceled.get_future().get().status == ipc::StatusCode::Canceled);

    held.reset();
    a.Stop();
    b.Stop();
}

TEST_CASE("endpoint close fails pending calls and later calls fail immediately")
{
    PipePair pipes;
    Peer a;
    Peer b;
    a.Start(pipes.aRead, pipes.aWrite);
    b.Start(pipes.bRead, pipes.bWrite);
    std::vector<std::shared_ptr<ipc::Reply>> held;
    std::mutex heldMutex;
    b.endpoint->SetRequestHandler(
        [&](ipc::IncomingRequest, ipc::Reply reply)
        {
            std::lock_guard<std::mutex> lock(heldMutex);
            held.push_back(std::make_shared<ipc::Reply>(std::move(reply)));
        });

    std::vector<std::future<ipc::Response>> futures;
    std::vector<std::shared_ptr<std::promise<ipc::Response>>> promises;
    for (int i = 0; i < 20; ++i)
    {
        auto promise = std::make_shared<std::promise<ipc::Response>>();
        futures.push_back(promise->get_future());
        promises.push_back(promise);
        a.endpoint->Call(1, {}, 0ms, [promise](ipc::Response response) { promise->set_value(std::move(response)); });
    }

    std::this_thread::sleep_for(100ms);
    b.transport.Shutdown(0ms);
    for (auto& future : futures)
    {
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        CHECK(future.get().status == ipc::StatusCode::Disconnected);
    }

    std::promise<ipc::Response> after;
    a.endpoint->Call(1, {}, 0ms, [&](ipc::Response response) { after.set_value(std::move(response)); });
    REQUIRE(after.get_future().wait_for(2s) == std::future_status::ready);

    {
        std::lock_guard<std::mutex> lock(heldMutex);
        held.clear();
    }
    a.Stop();
    b.Stop();
}

TEST_CASE("reply fails automatically when dropped")
{
    PipePair pipes;
    Peer a;
    Peer b;
    a.Start(pipes.aRead, pipes.aWrite);
    b.Start(pipes.bRead, pipes.bWrite);
    b.endpoint->SetRequestHandler([](ipc::IncomingRequest, ipc::Reply) {});

    std::promise<ipc::Response> result;
    a.endpoint->Call(1, {}, 5s, [&](ipc::Response response) { result.set_value(std::move(response)); });
    auto response = result.get_future().get();
    CHECK(response.status == ipc::StatusCode::Error);
    CHECK(response.message == "The request was not answered.");
    a.Stop();
    b.Stop();
}

TEST_CASE("stream receiver grants credit, wakes consumers and cancels unknown ids once")
{
    std::mutex mutex;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sent;
    auto receiver = ipc::StreamReceiver::Create(
        [&](uint8_t opcode, const std::vector<uint8_t>& body)
        {
            std::lock_guard<std::mutex> lock(mutex);
            sent.emplace_back(opcode, body);
            return true;
        },
        18, 19);

    auto stream = receiver->Open(5, 300000);
    std::atomic<int> wakes{0};
    CHECK(stream->SetWakeup([&] { ++wakes; }));
    auto data = Bytes(200000, 3);
    receiver->OnData(5, data.data(), 100000);
    CHECK(wakes == 1);
    CHECK_FALSE(stream->SetWakeup([&] { ++wakes; }));
    receiver->OnData(5, data.data() + 100000, 100000);
    CHECK(wakes == 1);

    std::vector<uint8_t> out(200000);
    size_t total = 0;
    while (total < out.size())
    {
        size_t read = 0;
        auto result = stream->Read(out.data() + total, 30000, read);
        REQUIRE(result == ipc::IncomingStream::ReadResult::Data);
        total += read;
    }
    CHECK(out == data);

    uint32_t credited = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& entry : sent)
        {
            REQUIRE(entry.first == 18);
            uint32_t id = 0;
            uint32_t bytes = 0;
            std::memcpy(&id, entry.second.data(), 4);
            std::memcpy(&bytes, entry.second.data() + 4, 4);
            CHECK(id == 5);
            credited += bytes;
        }
    }
    CHECK(credited == 200000);

    size_t read = 0;
    CHECK(stream->Read(out.data(), 10, read) == ipc::IncomingStream::ReadResult::Pending);
    receiver->OnEnd(5, 200000);
    CHECK(stream->Read(out.data(), 10, read) == ipc::IncomingStream::ReadResult::End);

    {
        std::lock_guard<std::mutex> lock(mutex);
        sent.clear();
    }
    uint8_t byte = 0;
    receiver->OnData(77, &byte, 1);
    receiver->OnData(77, &byte, 1);
    {
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].first == 19);
    }

    auto canceled = receiver->Open(8, -1);
    canceled->Cancel();
    CHECK(receiver->OpenCount() == 0);
    CHECK(canceled->Read(out.data(), 10, read) == ipc::IncomingStream::ReadResult::Error);
}

TEST_CASE("endpoint answers a cancel with canceled and drops the late reply")
{
    PipePair pipes;
    Peer b;
    b.Start(pipes.bRead, pipes.bWrite);

    Collector collector;
    ipc::Transport a;
    a.SetHandles(pipes.aRead, pipes.aWrite);
    REQUIRE(a.Start(collector.Handler(), collector.ClosedHandler()));

    std::mutex mutex;
    std::shared_ptr<ipc::Reply> held;
    std::promise<void> arrived;
    b.endpoint->SetRequestHandler(
        [&](ipc::IncomingRequest request, ipc::Reply reply)
        {
            std::lock_guard<std::mutex> lock(mutex);
            held = std::make_shared<ipc::Reply>(std::move(reply));
            arrived.set_value();
        });

    REQUIRE(a.Enqueue(ipc::PacketType::Request, 1, 55, nullptr, 0));
    arrived.get_future().wait();
    REQUIRE(a.Enqueue(ipc::PacketType::Cancel, 1, 55, nullptr, 0));

    REQUIRE(collector.WaitFor(1, 2s));
    {
        std::lock_guard<std::mutex> lock(collector.mutex);
        REQUIRE(collector.packets.size() == 1);
        CHECK(collector.packets[0].type == ipc::PacketType::Response);
        CHECK(collector.packets[0].requestId == 55);
        CHECK(collector.packets[0].body.at(0) == static_cast<uint8_t>(ipc::StatusCode::Canceled));
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(held->IsCanceled());
        held->Ok(std::vector<uint8_t>{7});
    }

    CHECK_FALSE(collector.WaitFor(2, 300ms));

    a.Shutdown(200ms);
    b.Stop();
}

TEST_CASE("endpoint rejects request id zero and duplicate in flight ids")
{
    PipePair pipes;
    Peer b;
    b.Start(pipes.bRead, pipes.bWrite);

    Collector collector;
    ipc::Transport a;
    a.SetHandles(pipes.aRead, pipes.aWrite);
    REQUIRE(a.Start(collector.Handler(), collector.ClosedHandler()));

    std::atomic<int> handled{0};
    std::mutex mutex;
    std::vector<std::shared_ptr<ipc::Reply>> held;
    b.endpoint->SetRequestHandler(
        [&](ipc::IncomingRequest request, ipc::Reply reply)
        {
            ++handled;
            std::lock_guard<std::mutex> lock(mutex);
            held.push_back(std::make_shared<ipc::Reply>(std::move(reply)));
        });

    REQUIRE(a.Enqueue(ipc::PacketType::Request, 1, 0, nullptr, 0));
    REQUIRE(a.Enqueue(ipc::PacketType::Request, 1, 77, nullptr, 0));
    REQUIRE(a.Enqueue(ipc::PacketType::Request, 1, 77, nullptr, 0));

    REQUIRE(collector.WaitFor(1, 2s));
    {
        std::lock_guard<std::mutex> lock(collector.mutex);
        REQUIRE(collector.packets.size() == 1);
        CHECK(collector.packets[0].type == ipc::PacketType::Response);
        CHECK(collector.packets[0].requestId == 77);
        CHECK(collector.packets[0].body.at(0) == static_cast<uint8_t>(ipc::StatusCode::InvalidRequest));
    }
    CHECK(handled == 1);

    a.Shutdown(200ms);
    b.Stop();
}

TEST_CASE("stream receiver fails a reused identifier and enforces the credit window")
{
    std::mutex mutex;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sent;
    auto receiver = ipc::StreamReceiver::Create(
        [&](uint8_t opcode, const std::vector<uint8_t>& body)
        {
            std::lock_guard<std::mutex> lock(mutex);
            sent.emplace_back(opcode, body);
            return true;
        },
        18, 19);

    auto first = receiver->Open(11, -1);
    auto second = receiver->Open(11, -1);
    std::vector<uint8_t> out(16);
    size_t read = 0;
    CHECK(first->Read(out.data(), out.size(), read) == ipc::IncomingStream::ReadResult::Error);

    auto chunk = Bytes(ipc::kStreamMaxChunk + 1, 7);
    receiver->OnData(11, chunk.data(), chunk.size());
    CHECK(second->Read(out.data(), out.size(), read) == ipc::IncomingStream::ReadResult::Error);

    auto third = receiver->Open(12, -1);
    auto block = Bytes(ipc::kStreamMaxChunk, 9);
    for (size_t total = 0; total <= ipc::kStreamMaxBuffered; total += block.size())
        receiver->OnData(12, block.data(), block.size());
    CHECK(third->Read(out.data(), out.size(), read) != ipc::IncomingStream::ReadResult::Pending);
}
