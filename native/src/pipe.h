#ifndef PIPE_H
#define PIPE_H

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

#include "include/base/cef_logging.h"

class Pipe {
public:
    Pipe(bool closeOnDestruct = true) :
#ifdef _WIN32
       _readHandle(INVALID_HANDLE_VALUE),
       _writeHandle(INVALID_HANDLE_VALUE),
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
    size_t Read(void* buffer, size_t size, bool readFully = false);
    size_t Write(const void* buffer, size_t size, bool writeFully = false);
    void Close();

private:
#ifdef _WIN32
    HANDLE _readHandle;
    HANDLE _writeHandle;
#else
    int _readFd;
    int _writeFd;
#endif
    bool _closeOnDestruct;
};

#endif //PIPE_H
