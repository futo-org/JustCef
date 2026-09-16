#include "DataStream.h"

#include "JustCefLogger.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

DataStream::DataStream(uint32_t identifier, std::shared_ptr<justcef::ByteStream> source, std::optional<uint64_t> length, Sender sender)
    : _identifier(identifier), _source(std::move(source)), _length(length), _sender(std::move(sender))
{
}

void DataStream::Start(asio::any_io_executor executor, FinishedCallback onFinished)
{
    asio::co_spawn(executor, Pump(shared_from_this(), std::move(onFinished)), asio::detached);
}

void DataStream::AddCredit(uint32_t bytes)
{
    justcef::detail::Completion<void()> waiter;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _credit += bytes;
        waiter = std::move(_waiter);
    }
    waiter.Post();
}

void DataStream::Cancel()
{
    justcef::detail::Completion<void()> waiter;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _isCanceled = true;
        waiter = std::move(_waiter);
    }
    waiter.Post();
}

void DataStream::CloseSource()
{
    if (_isSourceClosed.exchange(true) || !_source)
    {
        return;
    }

    try
    {
        _source->Close();
    }
    catch (...)
    {
        justcef::Logger::Warning("JustCefProcess", "Closing a proxy response stream failed.", std::current_exception());
    }
}

uint64_t DataStream::AvailableCredit()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _credit;
}

bool DataStream::IsCanceled()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _isCanceled;
}

void DataStream::Send(justcef::detail::OpcodeControllerNotification opcode, std::vector<uint8_t> body)
{
    _sender(justcef::detail::MakeNotification(static_cast<uint8_t>(opcode), std::move(body)));
}

asio::awaitable<void> DataStream::WaitForCredit()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_isCanceled || _credit > 0)
        {
            co_return;
        }
    }

    auto executor = co_await asio::this_coro::executor;
    co_await asio::async_initiate<decltype(asio::use_awaitable), void()>(
        [this, executor](auto handler)
        {
            justcef::detail::Completion<void()> completion(std::move(handler), executor);
            std::unique_lock<std::mutex> lock(_mutex);
            if (_isCanceled || _credit > 0)
            {
                lock.unlock();
                completion.Post();
                return;
            }
            _waiter = std::move(completion);
        },
        asio::use_awaitable);
}

asio::awaitable<void> DataStream::Pump(std::shared_ptr<DataStream> self, FinishedCallback onFinished)
{
    uint64_t total = 0;
    try
    {
        while (true)
        {
            if (self->_length && total >= *self->_length)
            {
                justcef::detail::PacketWriter writer;
                writer.Write<uint32_t>(self->_identifier);
                writer.Write<uint64_t>(total);
                self->Send(justcef::detail::OpcodeControllerNotification::StreamEnd, writer.Release());
                break;
            }

            co_await self->WaitForCredit();
            if (self->IsCanceled())
            {
                break;
            }

            size_t chunk = static_cast<size_t>(std::min<uint64_t>(self->AvailableCredit(), justcef::detail::kStreamMaxChunk));
            if (self->_length)
            {
                chunk = static_cast<size_t>(std::min<uint64_t>(chunk, *self->_length - total));
            }

            std::vector<uint8_t> body(sizeof(uint32_t) + chunk);
            std::memcpy(body.data(), &self->_identifier, sizeof(uint32_t));
            const size_t read = self->_source ? co_await self->_source->ReadAsync(body.data() + sizeof(uint32_t), chunk) : 0;
            if (self->IsCanceled())
            {
                break;
            }

            if (read == 0)
            {
                if (self->_length && total < *self->_length)
                {
                    throw std::runtime_error("The proxy response body ended before its declared length.");
                }

                justcef::detail::PacketWriter writer;
                writer.Write<uint32_t>(self->_identifier);
                writer.Write<uint64_t>(total);
                self->Send(justcef::detail::OpcodeControllerNotification::StreamEnd, writer.Release());
                break;
            }

            const size_t sent = std::min(read, chunk);
            body.resize(sizeof(uint32_t) + sent);
            {
                std::lock_guard<std::mutex> lock(self->_mutex);
                self->_credit -= std::min<uint64_t>(self->_credit, sent);
            }
            total += sent;
            self->Send(justcef::detail::OpcodeControllerNotification::StreamData, std::move(body));
        }
    }
    catch (const std::exception& exception)
    {
        if (!self->IsCanceled())
        {
            justcef::detail::PacketWriter writer;
            writer.Write<uint32_t>(self->_identifier);
            writer.WriteSizePrefixedString(std::string(exception.what()));
            self->Send(justcef::detail::OpcodeControllerNotification::StreamError, writer.Release());
        }
    }
    catch (...)
    {
        if (!self->IsCanceled())
        {
            justcef::detail::PacketWriter writer;
            writer.Write<uint32_t>(self->_identifier);
            writer.WriteSizePrefixedString(std::string("The proxy response stream failed."));
            self->Send(justcef::detail::OpcodeControllerNotification::StreamError, writer.Release());
        }
    }

    self->CloseSource();
    if (onFinished)
    {
        onFinished(self->_identifier);
    }
}
