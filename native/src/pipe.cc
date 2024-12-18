#include "pipe.h"

#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

bool Pipe::Create() {
    LOG(INFO) << "Pipe create";

#ifdef _WIN32
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&_readHandle, &_writeHandle, &saAttr, 0)) {
        return false;
    }
    return true;
#else
    int fds[2];
    if (pipe(fds) == -1) {
        return false;
    }
    _readFd = fds[0];
    _writeFd = fds[1];
    return true;
#endif
}

size_t Pipe::Read(void* buffer, size_t size, bool readFully) {
#ifdef _WIN32
    DWORD totalBytesRead = 0;
    DWORD bytesRead;
    while (totalBytesRead < size) {
        if (!ReadFile(_readHandle, (char*)buffer + totalBytesRead, static_cast<DWORD>(size - totalBytesRead), &bytesRead, NULL) || bytesRead == 0) {
            break; // Error or pipe closed
        }
        totalBytesRead += bytesRead;

        if (!readFully) break;
    }
    return static_cast<size_t>(totalBytesRead);
#else
    size_t totalBytesRead = 0;
    size_t bytesRead;
    while (totalBytesRead < size) {
        bytesRead = read(_readFd, (char*)buffer + totalBytesRead, size - totalBytesRead);
        if (bytesRead <= 0) { // Error or pipe closed
            break;
        }
        totalBytesRead += bytesRead;

        if (!readFully) break;
    }
    return static_cast<size_t>(totalBytesRead);
#endif
}

size_t Pipe::Write(const void* buffer, size_t size, bool writeFully) {
#ifdef _WIN32
    DWORD totalBytesWritten = 0;
    DWORD bytesWritten;
    while (totalBytesWritten < size) {
        if (!WriteFile(_writeHandle, (const char*)buffer + totalBytesWritten, static_cast<DWORD>(size - totalBytesWritten), &bytesWritten, NULL) || bytesWritten == 0) {
            break; // Error or pipe closed
        }
        totalBytesWritten += bytesWritten;
        if (!writeFully) break;
    }
    return static_cast<size_t>(totalBytesWritten);
#else
    size_t totalBytesWritten = 0;
    size_t bytesWritten;
    while (totalBytesWritten < size) {
        bytesWritten = write(_writeFd, (const char*)buffer + totalBytesWritten, size - totalBytesWritten);
        if (bytesWritten <= 0) { // Error or pipe closed
            break;
        }
        totalBytesWritten += bytesWritten;
        if (!writeFully) break;
    }
    return static_cast<size_t>(totalBytesWritten);
#endif
}

void Pipe::Close() {
    LOG(INFO) << "Pipe close.";

#ifdef _WIN32
    if (_readHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(_readHandle);
        _readHandle = INVALID_HANDLE_VALUE;
    }

    LOG(INFO) << "Read handle closed.";
   
     if (_writeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(_writeHandle);
        _writeHandle = INVALID_HANDLE_VALUE;
    }

     LOG(INFO) << "Write handle closed.";
#else
    if (_readFd != -1) {
        close(_readFd);
        _readFd = -1;
    }

    LOG(INFO) << "Read handle closed.";

    if (_writeFd != -1) {
        close(_writeFd);
        _writeFd = -1;
    }

    LOG(INFO) << "Write handle closed.";
#endif

    LOG(INFO) << "Pipe closed.";
}

#ifdef _WIN32
void Pipe::SetHandles(HANDLE readHandle, HANDLE writeHandle) {
    LOG(INFO) << "Pipe set handles readHandle " << readHandle << ", writeHandle " << writeHandle;

    _readHandle = readHandle;
    _writeHandle = writeHandle;
}
#else
void Pipe::SetHandles(int readFd, int writeFd) {
    LOG(INFO) << "Pipe set handles readFd " << readFd << ", writeFd " << writeFd;

    _readFd = readFd;
    _writeFd = writeFd;
}
#endif
