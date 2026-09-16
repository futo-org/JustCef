#ifndef PIPE_H
#define PIPE_H

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

#include "include/base/cef_logging.h"

#include <atomic>
#include <mutex>

class Pipe
{
public:
    Pipe(bool closeOnDestruct = true)
        :
#ifdef _WIN32
          _readHandle(INVALID_HANDLE_VALUE), _writeHandle(INVALID_HANDLE_VALUE),
#else
          _readFd(-1), _writeFd(-1),
#endif
          _closeOnDestruct(closeOnDestruct)
    {
    }

    ~Pipe()
    {
        LOG(INFO) << "Pipe destructor called";
        if (_closeOnDestruct)
            Close();
    }

#ifdef _WIN32
    void SetHandles(HANDLE readHandle, HANDLE writeHandle);
#else
    void SetHandles(int readFd, int writeFd);
#endif

    bool Create();
    bool HasValidHandles();
    bool Open();
    size_t Read(void* buffer, size_t size, bool readFully = false);
    size_t Write(const void* buffer, size_t size, bool writeFully = false);
    void BindReaderThread();
    void BindWriterThread();
    void Interrupt();
    void Close();

private:
#ifdef _WIN32
    HANDLE _readHandle;
    HANDLE _writeHandle;
    std::mutex _threadMutex;
    HANDLE _readerThread = nullptr;
    HANDLE _writerThread = nullptr;
#else
    bool WaitReady(int fd, short events);

    int _readFd;
    int _writeFd;
    int _wakeReadFd = -1;
    int _wakeWriteFd = -1;
#endif
    bool _closeOnDestruct;
    std::atomic<bool> _interrupted{false};
};

#endif // PIPE_H
