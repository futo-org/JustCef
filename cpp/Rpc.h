#pragma once

#include "AsyncSignal.h"
#include "Packet.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace justcef::detail
{

class Rpc : public std::enable_shared_from_this<Rpc>
{
public:
    using Sender = std::function<bool(OutgoingPacket)>;
    using ReplyHook = std::function<void(PacketReader&)>;
    using Timeout = std::optional<std::chrono::steady_clock::duration>;

    Rpc(asio::any_io_executor executor, Sender sender);

    asio::awaitable<PacketReader> CallAsync(std::uint8_t opcode, std::vector<std::uint8_t> body, Timeout timeout, ReplyHook hook = {});
    void OnResponse(std::uint32_t request_id, std::uint8_t opcode, std::vector<std::uint8_t> body);
    void Close(std::exception_ptr exception);
    std::size_t PendingCount() const;

private:
    struct Pending
    {
        std::uint32_t request_id = 0;
        std::uint8_t opcode = 0;
        ReplyHook hook;
        std::unique_ptr<asio::steady_timer> timer;
        asio::any_io_executor cleanup_executor;
        Completion<void(std::exception_ptr, PacketReader)> completion;
    };

    void Initiate(Completion<void(std::exception_ptr, PacketReader)> completion, std::uint8_t opcode, std::vector<std::uint8_t> body, Timeout timeout, ReplyHook hook);
    void Abort(const std::shared_ptr<Pending>& pending, std::exception_ptr exception);
    std::shared_ptr<Pending> Take(std::uint32_t request_id);
    static void Finish(const std::shared_ptr<Pending>& pending, std::exception_ptr exception, PacketReader reader);

    asio::any_io_executor executor_;
    Sender sender_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending_;
    std::uint32_t next_request_id_ = 0;
    bool closed_ = false;
    std::exception_ptr close_exception_;
};

} // namespace justcef::detail
