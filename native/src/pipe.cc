#include "pipe.h"

#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <stdint.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef _WIN32
namespace
{

bool SetNonBlockingCloseOnExec(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return false;
    const int fdFlags = fcntl(fd, F_GETFD);
    return fdFlags != -1 && fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) != -1;
}

void ConsumePendingSigpipe()
{
#if defined(__linux__)
    sigset_t pipeSet;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    sigset_t pending;
    sigemptyset(&pending);
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE))
    {
        timespec zero = {0, 0};
        while (sigtimedwait(&pipeSet, nullptr, &zero) == -1 && errno == EINTR)
        {
        }
    }
#endif
}

} // namespace
#endif

bool Pipe::Create()
{
    LOG(INFO) << "Pipe create";

#ifdef _WIN32
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&_readHandle, &_writeHandle, &saAttr, 0))
    {
        return false;
    }
    return true;
#else
    int fds[2];
    if (pipe(fds) == -1)
    {
        return false;
    }
    _readFd = fds[0];
    _writeFd = fds[1];
    return true;
#endif
}

bool Pipe::HasValidHandles()
{
#ifdef _WIN32
    return _readHandle != INVALID_HANDLE_VALUE && _writeHandle != INVALID_HANDLE_VALUE;
#else
    return _readFd != -1 && _writeFd != -1;
#endif
}

bool Pipe::Open()
{
    if (!HasValidHandles())
        return false;

    _interrupted = false;
#ifdef _WIN32
    SetHandleInformation(_readHandle, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(_writeHandle, HANDLE_FLAG_INHERIT, 0);
    return true;
#else
    if (!SetNonBlockingCloseOnExec(_readFd) || !SetNonBlockingCloseOnExec(_writeFd))
        return false;
#ifdef F_SETNOSIGPIPE
    fcntl(_writeFd, F_SETNOSIGPIPE, 1);
#endif

    int wake[2];
    if (pipe(wake) == -1)
        return false;
    if (!SetNonBlockingCloseOnExec(wake[0]) || !SetNonBlockingCloseOnExec(wake[1]))
    {
        close(wake[0]);
        close(wake[1]);
        return false;
    }

    _wakeReadFd = wake[0];
    _wakeWriteFd = wake[1];
    return true;
#endif
}

#ifndef _WIN32
bool Pipe::WaitReady(int fd, short events)
{
    pollfd fds[2] = {{fd, events, 0}, {_wakeReadFd, POLLIN, 0}};
    if (poll(fds, 2, -1) == -1 && errno != EINTR)
        return false;
    if (fds[1].revents != 0)
        return false;
    return (fds[0].revents & (POLLERR | POLLNVAL)) == 0 || (fds[0].revents & events) != 0;
}
#endif

size_t Pipe::Read(void* buffer, size_t size, bool readFully)
{
#ifdef _WIN32
    DWORD totalBytesRead = 0;
    DWORD bytesRead;
    while (totalBytesRead < size && !_interrupted)
    {
        if (!ReadFile(_readHandle, (char*)buffer + totalBytesRead, static_cast<DWORD>(size - totalBytesRead), &bytesRead, NULL) || bytesRead == 0)
        {
            break; // Error or pipe closed
        }
        totalBytesRead += bytesRead;

        if (!readFully)
            break;
    }
    return static_cast<size_t>(totalBytesRead);
#else
    size_t totalBytesRead = 0;
    ssize_t bytesRead;
    while (totalBytesRead < size && !_interrupted)
    {
        bytesRead = read(_readFd, (char*)buffer + totalBytesRead, size - totalBytesRead);
        if (bytesRead < 0 && errno == EINTR)
            continue;
        if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!WaitReady(_readFd, POLLIN))
                break;
            continue;
        }
        if (bytesRead <= 0)
        { // Error or pipe closed
            break;
        }
        totalBytesRead += bytesRead;

        if (!readFully)
            break;
    }
    return static_cast<size_t>(totalBytesRead);
#endif
}

size_t Pipe::Write(const void* buffer, size_t size, bool writeFully)
{
#ifdef _WIN32
    DWORD totalBytesWritten = 0;
    DWORD bytesWritten;
    while (totalBytesWritten < size && !_interrupted)
    {
        if (!WriteFile(_writeHandle, (const char*)buffer + totalBytesWritten, static_cast<DWORD>(size - totalBytesWritten), &bytesWritten, NULL) || bytesWritten == 0)
        {
            break; // Error or pipe closed
        }
        totalBytesWritten += bytesWritten;
        if (!writeFully)
            break;
    }
    return static_cast<size_t>(totalBytesWritten);
#else
    size_t totalBytesWritten = 0;
    ssize_t bytesWritten;
    while (totalBytesWritten < size && !_interrupted)
    {
        bytesWritten = write(_writeFd, (const char*)buffer + totalBytesWritten, size - totalBytesWritten);
        if (bytesWritten < 0 && errno == EINTR)
            continue;
        if (bytesWritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!WaitReady(_writeFd, POLLOUT))
                break;
            continue;
        }
        if (bytesWritten < 0 && errno == EPIPE)
            ConsumePendingSigpipe();
        if (bytesWritten <= 0)
        { // Error or pipe closed
            break;
        }
        totalBytesWritten += bytesWritten;
        if (!writeFully)
            break;
    }
    return static_cast<size_t>(totalBytesWritten);
#endif
}

void Pipe::BindReaderThread()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(_threadMutex);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &_readerThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
#endif
}

void Pipe::BindWriterThread()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(_threadMutex);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &_writerThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
#else
    sigset_t pipeSet;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, nullptr);
#endif
}

void Pipe::Interrupt()
{
    _interrupted = true;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(_threadMutex);
    if (_readerThread)
        CancelSynchronousIo(_readerThread);
    if (_writerThread)
        CancelSynchronousIo(_writerThread);
#else
    if (_wakeWriteFd != -1)
    {
        const uint8_t byte = 1;
        while (write(_wakeWriteFd, &byte, 1) == -1 && errno == EINTR)
        {
        }
    }
#endif
}

void Pipe::Close()
{
    LOG(INFO) << "Pipe close.";

#ifdef _WIN32
    if (_readHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_readHandle);
        _readHandle = INVALID_HANDLE_VALUE;
    }

    LOG(INFO) << "Read handle closed.";

    if (_writeHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_writeHandle);
        _writeHandle = INVALID_HANDLE_VALUE;
    }

    LOG(INFO) << "Write handle closed.";

    {
        std::lock_guard<std::mutex> lock(_threadMutex);
        if (_readerThread)
        {
            CloseHandle(_readerThread);
            _readerThread = nullptr;
        }
        if (_writerThread)
        {
            CloseHandle(_writerThread);
            _writerThread = nullptr;
        }
    }
#else
    if (_readFd != -1)
    {
        close(_readFd);
        _readFd = -1;
    }

    LOG(INFO) << "Read handle closed.";

    if (_writeFd != -1)
    {
        close(_writeFd);
        _writeFd = -1;
    }

    LOG(INFO) << "Write handle closed.";

    if (_wakeReadFd != -1)
    {
        close(_wakeReadFd);
        _wakeReadFd = -1;
    }
    if (_wakeWriteFd != -1)
    {
        close(_wakeWriteFd);
        _wakeWriteFd = -1;
    }
#endif

    LOG(INFO) << "Pipe closed.";
}

#ifdef _WIN32
void Pipe::SetHandles(HANDLE readHandle, HANDLE writeHandle)
{
    LOG(INFO) << "Pipe set handles readHandle " << readHandle << ", writeHandle " << writeHandle;

    _readHandle = readHandle;
    _writeHandle = writeHandle;
}
#else
void Pipe::SetHandles(int readFd, int writeFd)
{
    LOG(INFO) << "Pipe set handles readFd " << readFd << ", writeFd " << writeFd;

    _readFd = readFd;
    _writeFd = writeFd;
}
#endif
