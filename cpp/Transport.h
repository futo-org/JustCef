#pragma once

#include "Packet.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace justcef::detail
{

#ifdef _WIN32
using NativeHandle = void*;
#else
using NativeHandle = int;
#endif

NativeHandle InvalidNativeHandle();
void CloseNativeHandle(NativeHandle handle);

struct IncomingPacket
{
    std::uint32_t request_id = 0;
    PacketType packet_type = PacketType::Request;
    std::uint8_t opcode = 0;
    std::vector<std::uint8_t> body;
};

enum class CloseMode : int
{
    Open = 0,
    Drain = 1,
    Immediate = 2,
};

class Transport
{
public:
    using PacketCallback = std::function<void(IncomingPacket&&)>;
    using ClosedCallback = std::function<void()>;

    Transport(NativeHandle read_handle, NativeHandle write_handle);
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    void Start(PacketCallback on_packet, ClosedCallback on_closed);
    bool Enqueue(OutgoingPacket packet);
    void Close(CloseMode mode);
    void Join();

    static bool IsTransportThread();

private:
    void ReaderLoop();
    void WriterLoop();
    std::size_t ReadSome(std::uint8_t* buffer, std::size_t size);
    bool WriteAll(const std::uint8_t* data, std::size_t size, bool committed);
    void Wake();

    NativeHandle read_handle_;
    NativeHandle write_handle_;
    std::mutex wake_mutex_;
#ifdef _WIN32
    void* reader_thread_handle_ = nullptr;
    void* writer_thread_handle_ = nullptr;
#else
    int wake_read_ = -1;
    int wake_write_ = -1;
#endif

    PacketCallback on_packet_;
    ClosedCallback on_closed_;

    std::atomic<int> close_mode_ = static_cast<int>(CloseMode::Open);
    std::atomic<bool> reader_done_ = false;
    std::atomic<bool> writer_done_ = false;
    std::atomic<bool> reader_in_io_ = false;
    std::atomic<bool> writer_in_io_ = false;

    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::deque<OutgoingPacket> queue_;
    bool accepting_ = true;
    bool writer_stop_ = false;

    std::mutex join_mutex_;
    bool joined_ = false;
    std::thread reader_thread_;
    std::thread writer_thread_;
};

} // namespace justcef::detail
