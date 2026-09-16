#include "Rpc.h"

#include "JustCefErrors.h"

#include <stdexcept>
#include <utility>

namespace justcef::detail
{

Rpc::Rpc(asio::any_io_executor executor, Sender sender) : executor_(std::move(executor)), sender_(std::move(sender))
{
}

asio::awaitable<PacketReader> Rpc::CallAsync(std::uint8_t opcode, std::vector<std::uint8_t> body, Timeout timeout, ReplyHook hook)
{
    return asio::async_initiate<decltype(asio::use_awaitable), void(std::exception_ptr, PacketReader)>(
        [self = shared_from_this(), opcode, body = std::move(body), timeout, hook = std::move(hook)](auto handler) mutable
        {
            self->Initiate(Completion<void(std::exception_ptr, PacketReader)>(std::move(handler), self->executor_), opcode, std::move(body), timeout, std::move(hook));
        },
        asio::use_awaitable);
}

void Rpc::Initiate(Completion<void(std::exception_ptr, PacketReader)> completion, std::uint8_t opcode, std::vector<std::uint8_t> body, Timeout timeout, ReplyHook hook)
{
    auto pending = std::make_shared<Pending>();
    pending->opcode = opcode;
    pending->hook = std::move(hook);
    pending->cleanup_executor = executor_;
    pending->completion = std::move(completion);

    if (body.size() > kMaxIpcSize)
    {
        Finish(pending, std::make_exception_ptr(RemoteError(Status::TooLarge, std::string("The request exceeds the maximum IPC packet size."))), {});
        return;
    }

    const std::weak_ptr<Rpc> weak_self = weak_from_this();
    const std::weak_ptr<Pending> weak_pending = pending;

    std::uint32_t request_id = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_)
        {
            auto exception = close_exception_;
            lock.unlock();
            Finish(pending, exception, {});
            return;
        }

        do
        {
            request_id = ++next_request_id_;
        } while (request_id == 0 || pending_.contains(request_id));

        pending->request_id = request_id;
        pending_.emplace(request_id, pending);

        if (timeout)
        {
            pending->timer = std::make_unique<asio::steady_timer>(executor_, *timeout);
            pending->timer->async_wait(
                [weak_self, weak_pending](const asio::error_code& error)
                {
                    if (error)
                    {
                        return;
                    }

                    auto self = weak_self.lock();
                    auto locked = weak_pending.lock();
                    if (self && locked)
                    {
                        self->Abort(locked, std::make_exception_ptr(asio::system_error(asio::error::timed_out)));
                    }
                });
        }

        auto slot = pending->completion.Slot();
        if (slot.is_connected())
        {
            slot.assign(
                [weak_self, weak_pending](asio::cancellation_type)
                {
                    auto self = weak_self.lock();
                    auto locked = weak_pending.lock();
                    if (self && locked)
                    {
                        self->Abort(locked, std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)));
                    }
                });
        }
    }

    if (!sender_(MakePacket(PacketType::Request, opcode, request_id, std::move(body))))
    {
        if (auto taken = Take(request_id))
        {
            Finish(taken, std::make_exception_ptr(std::runtime_error("The IPC connection is closed.")), {});
        }
    }
}

void Rpc::Abort(const std::shared_ptr<Pending>& pending, std::exception_ptr exception)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = pending_.find(pending->request_id);
        if (iterator == pending_.end() || iterator->second != pending)
        {
            return;
        }
        pending_.erase(iterator);
    }

    sender_(MakeCancel(pending->opcode, pending->request_id));
    Finish(pending, std::move(exception), {});
}

std::shared_ptr<Rpc::Pending> Rpc::Take(std::uint32_t request_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = pending_.find(request_id);
    if (iterator == pending_.end())
    {
        return nullptr;
    }

    auto pending = std::move(iterator->second);
    pending_.erase(iterator);
    return pending;
}

void Rpc::Finish(const std::shared_ptr<Pending>& pending, std::exception_ptr exception, PacketReader reader)
{
    if (pending->timer || pending->hook)
    {
        auto executor = pending->timer ? pending->timer->get_executor() : pending->cleanup_executor;
        asio::post(executor,
                   [timer = std::move(pending->timer), hook = std::move(pending->hook)]() mutable
                   {
                       timer.reset();
                       hook = {};
                   });
    }

    pending->completion.Post(std::move(exception), std::move(reader));
}

void Rpc::OnResponse(std::uint32_t request_id, std::uint8_t opcode, std::vector<std::uint8_t> body)
{
    auto pending = Take(request_id);
    if (!pending)
    {
        return;
    }

    if (pending->opcode != opcode)
    {
        Finish(pending, std::make_exception_ptr(ProtocolError("Received a response with a mismatched opcode.")), {});
        return;
    }

    if (body.empty())
    {
        Finish(pending, std::make_exception_ptr(ProtocolError("Received a response without a status.")), {});
        return;
    }

    PacketReader reader(std::move(body));
    const auto status = static_cast<Status>(*reader.Read<std::uint8_t>());
    if (status != Status::Ok)
    {
        Finish(pending, std::make_exception_ptr(RemoteError(status, reader.ReadSizePrefixedString())), {});
        return;
    }

    if (pending->hook)
    {
        try
        {
            PacketReader hook_reader = reader;
            pending->hook(hook_reader);
        }
        catch (...)
        {
            Finish(pending, std::current_exception(), {});
            return;
        }
    }

    Finish(pending, nullptr, std::move(reader));
}

void Rpc::Close(std::exception_ptr exception)
{
    std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_)
        {
            closed_ = true;
            close_exception_ = exception;
        }
        pending.swap(pending_);
    }

    for (auto& [_, entry] : pending)
    {
        Finish(entry, exception, {});
    }
}

std::size_t Rpc::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

} // namespace justcef::detail
