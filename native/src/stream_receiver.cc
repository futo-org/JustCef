#include "stream_receiver.h"

#include "include/base/cef_logging.h"

#include <algorithm>

namespace ipc
{

namespace
{

constexpr size_t kMaxRememberedCancels = 1024;

std::vector<uint8_t> EncodeIdAndCount(uint32_t id, uint32_t count, bool withCount)
{
    std::vector<uint8_t> body(withCount ? 8 : 4);
    std::memcpy(body.data(), &id, sizeof(uint32_t));
    if (withCount)
        std::memcpy(body.data() + 4, &count, sizeof(uint32_t));
    return body;
}

} // namespace

IncomingStream::ReadResult IncomingStream::Read(uint8_t* destination, size_t capacity, size_t& read)
{
    read = 0;
    uint32_t credit = 0;
    ReadResult result = ReadResult::Pending;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_canceled)
            return ReadResult::Error;

        while (read < capacity && !_chunks.empty())
        {
            std::vector<uint8_t>& chunk = _chunks.front();
            const size_t available = chunk.size() - _chunkOffset;
            const size_t count = std::min(available, capacity - read);
            std::memcpy(destination + read, chunk.data() + _chunkOffset, count);
            read += count;
            _chunkOffset += count;
            if (_chunkOffset == chunk.size())
            {
                _chunks.pop_front();
                _chunkOffset = 0;
            }
        }

        _buffered -= read;
        _consumedSinceGrant += static_cast<uint32_t>(read);
        if (!_ended && !_failed && (_consumedSinceGrant >= kStreamCreditBatch || (_buffered == 0 && _consumedSinceGrant > 0)))
        {
            credit = _consumedSinceGrant;
            _consumedSinceGrant = 0;
        }

        if (read > 0)
            result = ReadResult::Data;
        else if (_failed)
            result = ReadResult::Error;
        else if (_ended)
            result = ReadResult::End;
    }

    if (credit > 0)
    {
        if (auto receiver = _receiver.lock())
            receiver->SendCredit(_id, credit);
    }
    return result;
}

bool IncomingStream::SetWakeup(std::function<void()> wakeup)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_buffered > 0 || _ended || _failed || _canceled)
        return false;
    _wakeup = std::move(wakeup);
    return true;
}

void IncomingStream::Cancel()
{
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        finished = _canceled || _ended || _failed;
        _canceled = true;
        _wakeup = nullptr;
        _chunks.clear();
        _buffered = 0;
    }

    if (auto receiver = _receiver.lock())
    {
        receiver->Remove(_id);
        if (!finished)
            receiver->SendCancel(_id);
    }
}

std::string IncomingStream::ErrorMessage() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _error;
}

void IncomingStream::Push(const uint8_t* data, size_t size)
{
    std::function<void()> wakeup;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_canceled || _ended || _failed || size == 0)
            return;
        if (size > kStreamMaxChunk || _buffered + size > kStreamMaxBuffered)
        {
            _failed = true;
            _error = "The controller exceeded the stream credit window.";
            overflow = true;
        }
        else
        {
            _chunks.emplace_back(data, data + size);
            _buffered += size;
        }
        wakeup = TakeWakeupLocked();
    }

    if (overflow)
    {
        LOG(ERROR) << "Stream " << _id << " exceeded the credit window. Canceled.";
        if (auto receiver = _receiver.lock())
        {
            receiver->Remove(_id);
            receiver->SendCancel(_id);
        }
    }
    if (wakeup)
        wakeup();
}

void IncomingStream::Finish(bool error, const std::string& message)
{
    std::function<void()> wakeup;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_canceled || _ended || _failed)
            return;
        if (error)
        {
            _failed = true;
            _error = message;
        }
        else
        {
            _ended = true;
        }
        wakeup = TakeWakeupLocked();
    }
    if (wakeup)
        wakeup();
}

std::function<void()> IncomingStream::TakeWakeupLocked()
{
    std::function<void()> wakeup = std::move(_wakeup);
    _wakeup = nullptr;
    return wakeup;
}

std::shared_ptr<StreamReceiver> StreamReceiver::Create(NotifyFunction notify, uint8_t creditOpcode, uint8_t cancelOpcode)
{
    return std::shared_ptr<StreamReceiver>(new StreamReceiver(std::move(notify), creditOpcode, cancelOpcode));
}

std::shared_ptr<IncomingStream> StreamReceiver::Open(uint32_t id, int64_t length)
{
    auto stream = std::shared_ptr<IncomingStream>(new IncomingStream(weak_from_this(), id, length));
    std::shared_ptr<IncomingStream> displaced;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto existing = _streams.find(id);
        if (existing != _streams.end())
        {
            displaced = std::move(existing->second);
            _streams.erase(existing);
        }
        _streams[id] = stream;
        _canceledIds.erase(id);
        _canceledOrder.erase(std::remove(_canceledOrder.begin(), _canceledOrder.end(), id), _canceledOrder.end());
    }

    if (displaced)
    {
        LOG(ERROR) << "Stream " << id << " was opened while it was still open.";
        displaced->Finish(true, "The stream identifier was reused.");
    }
    return stream;
}

std::shared_ptr<IncomingStream> StreamReceiver::Find(uint32_t id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto stream = _streams.find(id);
    return stream == _streams.end() ? nullptr : stream->second;
}

void StreamReceiver::Remove(uint32_t id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _streams.erase(id);
}

void StreamReceiver::OnData(uint32_t id, const uint8_t* data, size_t size)
{
    if (auto stream = Find(id))
    {
        stream->Push(data, size);
        return;
    }
    SendCancel(id);
}

void StreamReceiver::OnEnd(uint32_t id, uint64_t total)
{
    if (auto stream = Find(id))
    {
        Remove(id);
        stream->Finish(false, std::string());
    }
}

void StreamReceiver::OnError(uint32_t id, const std::string& message)
{
    if (auto stream = Find(id))
    {
        Remove(id);
        stream->Finish(true, message);
    }
}

void StreamReceiver::FailAll(const std::string& message)
{
    std::unordered_map<uint32_t, std::shared_ptr<IncomingStream>> streams;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        streams.swap(_streams);
    }
    for (auto& entry : streams)
        entry.second->Finish(true, message);
}

size_t StreamReceiver::OpenCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _streams.size();
}

void StreamReceiver::SendCredit(uint32_t id, uint32_t bytes)
{
    _notify(_creditOpcode, EncodeIdAndCount(id, bytes, true));
}

void StreamReceiver::SendCancel(uint32_t id)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_canceledIds.count(id) != 0)
            return;
        _canceledIds[id] = true;
        _canceledOrder.push_back(id);
        while (_canceledOrder.size() > kMaxRememberedCancels)
        {
            _canceledIds.erase(_canceledOrder.front());
            _canceledOrder.pop_front();
        }
    }
    _notify(_cancelOpcode, EncodeIdAndCount(id, 0, false));
}

} // namespace ipc
