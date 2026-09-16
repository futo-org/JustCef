#include "Transport.h"

#include "JustCefLogger.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace justcef::detail
{
namespace
{

thread_local bool t_is_transport_thread = false;
constexpr std::size_t kReadBufferSize = 64 * 1024;

void NameCurrentThread(const char* name)
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    (void)name;
#endif
}

#ifndef _WIN32
void SetNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
    {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void SetCloseOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0)
    {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void MakeWakePipe(int& read_end, int& write_end)
{
    int fds[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        throw std::system_error(errno, std::generic_category(), "Failed to create the IPC wake pipe");
    }
#else
    if (::pipe(fds) != 0)
    {
        throw std::system_error(errno, std::generic_category(), "Failed to create the IPC wake pipe");
    }
    SetCloseOnExec(fds[0]);
    SetCloseOnExec(fds[1]);
    SetNonBlocking(fds[0]);
    SetNonBlocking(fds[1]);
#endif
    read_end = fds[0];
    write_end = fds[1];
}

void ConsumePendingSigpipe()
{
    sigset_t pending;
    sigemptyset(&pending);
    if (::sigpending(&pending) != 0 || !sigismember(&pending, SIGPIPE))
    {
        return;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
#if defined(__linux__)
    const timespec zero{0, 0};
    while (::sigtimedwait(&set, nullptr, &zero) < 0 && errno == EINTR)
    {
    }
#endif
}
#endif

} // namespace

NativeHandle InvalidNativeHandle()
{
#ifdef _WIN32
    return INVALID_HANDLE_VALUE;
#else
    return -1;
#endif
}

void CloseNativeHandle(NativeHandle handle)
{
#ifdef _WIN32
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
    }
#else
    if (handle >= 0)
    {
        ::close(handle);
    }
#endif
}

Transport::Transport(NativeHandle read_handle, NativeHandle write_handle) : read_handle_(read_handle), write_handle_(write_handle)
{
#ifndef _WIN32
    SetNonBlocking(read_handle_);
    SetNonBlocking(write_handle_);
    SetCloseOnExec(read_handle_);
    SetCloseOnExec(write_handle_);
#ifdef F_SETNOSIGPIPE
    ::fcntl(write_handle_, F_SETNOSIGPIPE, 1);
#endif
    try
    {
        MakeWakePipe(wake_read_, wake_write_);
    }
    catch (...)
    {
        CloseNativeHandle(read_handle_);
        CloseNativeHandle(write_handle_);
        throw;
    }
#endif
}

Transport::~Transport()
{
    Close(CloseMode::Immediate);
    Join();
}

void Transport::Start(PacketCallback on_packet, ClosedCallback on_closed)
{
    on_packet_ = std::move(on_packet);
    on_closed_ = std::move(on_closed);

    try
    {
        reader_thread_ = std::thread(
            [this]()
            {
                ReaderLoop();
            });
        writer_thread_ = std::thread(
            [this]()
            {
                WriterLoop();
            });
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(wake_mutex_);
        for (auto [thread, target] : {std::pair{&reader_thread_, &reader_thread_handle_}, std::pair{&writer_thread_, &writer_thread_handle_}})
        {
            HANDLE duplicated = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(thread->native_handle()), GetCurrentProcess(), &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                *target = duplicated;
            }
        }
#endif
    }
    catch (...)
    {
        Close(CloseMode::Immediate);
        Join();
        throw;
    }
}

bool Transport::Enqueue(OutgoingPacket packet)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!accepting_)
        {
            return false;
        }
        queue_.push_back(std::move(packet));
    }
    queue_condition_.notify_one();
    return true;
}

void Transport::Close(CloseMode mode)
{
    int current = close_mode_.load();
    const int desired = static_cast<int>(mode);
    while (current < desired && !close_mode_.compare_exchange_weak(current, desired))
    {
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        accepting_ = false;
        writer_stop_ = true;
    }
    queue_condition_.notify_all();
    Wake();
}

void Transport::Join()
{
    std::lock_guard<std::mutex> lock(join_mutex_);
    if (joined_)
    {
        return;
    }

    bool self_detached = false;
    for (std::thread* thread : {&reader_thread_, &writer_thread_})
    {
        if (!thread->joinable())
        {
            continue;
        }

        if (thread->get_id() == std::this_thread::get_id())
        {
            thread->detach();
            self_detached = true;
        }
        else
        {
            thread->join();
        }
    }

    joined_ = true;
    if (self_detached)
    {
        Close(CloseMode::Immediate);
    }

    CloseNativeHandle(read_handle_);
    CloseNativeHandle(write_handle_);
    read_handle_ = InvalidNativeHandle();
    write_handle_ = InvalidNativeHandle();
    {
        std::lock_guard<std::mutex> wake_lock(wake_mutex_);
#ifdef _WIN32
        CloseNativeHandle(reader_thread_handle_);
        CloseNativeHandle(writer_thread_handle_);
        reader_thread_handle_ = nullptr;
        writer_thread_handle_ = nullptr;
#else
        CloseNativeHandle(wake_read_);
        CloseNativeHandle(wake_write_);
        wake_read_ = -1;
        wake_write_ = -1;
#endif
    }

    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    queue_.clear();
}

bool Transport::IsTransportThread()
{
    return t_is_transport_thread;
}

void Transport::ReaderLoop()
{
    t_is_transport_thread = true;
    NameCurrentThread("justcef-reader");

    try
    {
        std::vector<std::uint8_t> buffer(kReadBufferSize);
        std::size_t start = 0;
        std::size_t end = 0;

        const auto fill = [&](std::size_t needed) -> bool
        {
            while (end - start < needed)
            {
                if (start > 0)
                {
                    std::memmove(buffer.data(), buffer.data() + start, end - start);
                    end -= start;
                    start = 0;
                }

                const std::size_t read = ReadSome(buffer.data() + end, buffer.size() - end);
                if (read == 0)
                {
                    return false;
                }
                end += read;
            }
            return true;
        };

        while (true)
        {
            if (!fill(kPacketHeaderSize))
            {
                if (end != start && close_mode_.load() == static_cast<int>(CloseMode::Open))
                {
                    Logger::Warning("JustCefTransport", "IPC pipe closed in the middle of a packet header.");
                }
                break;
            }

            const PacketHeader header = DecodeHeader(buffer.data() + start);
            start += kPacketHeaderSize;

            IncomingPacket packet;
            packet.request_id = header.request_id;
            packet.packet_type = header.packet_type;
            packet.opcode = header.opcode;

            const std::size_t body_size = header.BodySize();
            packet.body.resize(body_size);
            std::size_t copied = std::min(body_size, end - start);
            if (copied > 0)
            {
                std::memcpy(packet.body.data(), buffer.data() + start, copied);
                start += copied;
            }
            if (start == end)
            {
                start = 0;
                end = 0;
            }

            bool truncated = false;
            while (copied < body_size)
            {
                const std::size_t read = ReadSome(packet.body.data() + copied, body_size - copied);
                if (read == 0)
                {
                    truncated = true;
                    break;
                }
                copied += read;
            }

            if (truncated)
            {
                if (close_mode_.load() == static_cast<int>(CloseMode::Open))
                {
                    Logger::Warning("JustCefTransport", "IPC pipe closed in the middle of a packet body.");
                }
                break;
            }

            try
            {
                on_packet_(std::move(packet));
            }
            catch (...)
            {
                Logger::Error("JustCefTransport", "Failed to route an IPC packet.", std::current_exception());
            }
        }
    }
    catch (...)
    {
        Logger::Error("JustCefTransport", "IPC reader failed.", std::current_exception());
    }

    reader_done_ = true;
    Close(CloseMode::Drain);

    try
    {
        if (on_closed_)
        {
            on_closed_();
        }
    }
    catch (...)
    {
        Logger::Error("JustCefTransport", "IPC close handler failed.", std::current_exception());
    }
}

void Transport::WriterLoop()
{
    t_is_transport_thread = true;
    NameCurrentThread("justcef-writer");

#ifndef _WIN32
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif

    while (true)
    {
        OutgoingPacket packet;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_condition_.wait(lock,
                                  [this]()
                                  {
                                      return writer_stop_ || !queue_.empty();
                                  });
            if (writer_stop_)
            {
                break;
            }
            packet = std::move(queue_.front());
            queue_.pop_front();
        }

        if (!WriteAll(packet.head.data(), packet.head.size(), false) || !WriteAll(packet.body.data(), packet.body.size(), true))
        {
            if (close_mode_.load() == static_cast<int>(CloseMode::Open))
            {
                Logger::Warning("JustCefTransport", "IPC pipe write failed; the writer is stopping.");
            }
            break;
        }
    }

    writer_done_ = true;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    accepting_ = false;
    queue_.clear();
}

#ifdef _WIN32

std::size_t Transport::ReadSome(std::uint8_t* buffer, std::size_t size)
{
    while (true)
    {
        reader_in_io_ = true;
        const int mode = close_mode_.load();
        if (mode == static_cast<int>(CloseMode::Immediate))
        {
            reader_in_io_ = false;
            return 0;
        }
        if (mode == static_cast<int>(CloseMode::Drain))
        {
            DWORD available = 0;
            if (!PeekNamedPipe(read_handle_, nullptr, 0, nullptr, &available, nullptr) || available == 0)
            {
                reader_in_io_ = false;
                return 0;
            }
        }

        DWORD read = 0;
        const BOOL ok = ReadFile(read_handle_, buffer, static_cast<DWORD>(std::min<std::size_t>(size, 1U << 30)), &read, nullptr);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        reader_in_io_ = false;

        if (ok)
        {
            if (read > 0)
            {
                return read;
            }
            continue;
        }
        if (error == ERROR_OPERATION_ABORTED)
        {
            continue;
        }
        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_PIPE_NOT_CONNECTED)
        {
            return 0;
        }
        throw std::system_error(static_cast<int>(error), std::system_category(), "IPC pipe read failed");
    }
}

bool Transport::WriteAll(const std::uint8_t* data, std::size_t size, bool committed)
{
    std::size_t written = 0;
    while (written < size)
    {
        writer_in_io_ = true;
        const int mode = close_mode_.load();
        if (mode == static_cast<int>(CloseMode::Immediate) || (mode != static_cast<int>(CloseMode::Open) && !committed && written == 0))
        {
            writer_in_io_ = false;
            return false;
        }

        DWORD chunk = 0;
        const BOOL ok = WriteFile(write_handle_, data + written, static_cast<DWORD>(std::min<std::size_t>(size - written, 1U << 30)), &chunk, nullptr);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        writer_in_io_ = false;

        if (ok)
        {
            written += chunk;
            continue;
        }
        if (error == ERROR_OPERATION_ABORTED)
        {
            continue;
        }
        return false;
    }
    return true;
}

void Transport::Wake()
{
    if (t_is_transport_thread)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(wake_mutex_);
    const auto cancel = [](void* handle, std::atomic<bool>& in_io, std::atomic<bool>& done)
    {
        if (handle == nullptr)
        {
            return;
        }

        for (int attempt = 0; attempt < 2000 && in_io.load() && !done.load(); ++attempt)
        {
            CancelSynchronousIo(static_cast<HANDLE>(handle));
            Sleep(1);
        }
    };

    cancel(reader_thread_handle_, reader_in_io_, reader_done_);
    cancel(writer_thread_handle_, writer_in_io_, writer_done_);
}

#else

std::size_t Transport::ReadSome(std::uint8_t* buffer, std::size_t size)
{
    while (true)
    {
        if (close_mode_.load() == static_cast<int>(CloseMode::Immediate))
        {
            return 0;
        }

        const ssize_t result = ::read(read_handle_, buffer, size);
        if (result > 0)
        {
            return static_cast<std::size_t>(result);
        }
        if (result == 0)
        {
            return 0;
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            throw std::system_error(errno, std::generic_category(), "IPC pipe read failed");
        }
        if (close_mode_.load() != static_cast<int>(CloseMode::Open))
        {
            return 0;
        }

        pollfd fds[2] = {
            {read_handle_, POLLIN, 0},
            {wake_read_, POLLIN, 0},
        };
        if (::poll(fds, 2, -1) < 0 && errno != EINTR)
        {
            throw std::system_error(errno, std::generic_category(), "IPC pipe poll failed");
        }
    }
}

bool Transport::WriteAll(const std::uint8_t* data, std::size_t size, bool committed)
{
    std::size_t written = 0;
    while (written < size)
    {
        const int mode = close_mode_.load();
        if (mode == static_cast<int>(CloseMode::Immediate) || (mode != static_cast<int>(CloseMode::Open) && !committed && written == 0))
        {
            return false;
        }

        const ssize_t result = ::write(write_handle_, data + written, size - written);
        if (result > 0)
        {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
        {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            pollfd fds[2] = {
                {write_handle_, POLLOUT, 0},
                {wake_read_, POLLIN, 0},
            };
            const bool open = close_mode_.load() == static_cast<int>(CloseMode::Open);
            if (::poll(fds, open ? 2 : 1, open ? -1 : 1) < 0 && errno != EINTR)
            {
                return false;
            }
            continue;
        }
        if (result < 0 && errno == EPIPE)
        {
            ConsumePendingSigpipe();
        }
        return false;
    }
    return true;
}

void Transport::Wake()
{
    std::lock_guard<std::mutex> lock(wake_mutex_);
    if (wake_write_ < 0)
    {
        return;
    }

    const char byte = 1;
    while (::write(wake_write_, &byte, 1) < 0 && errno == EINTR)
    {
    }
}

#endif

} // namespace justcef::detail
