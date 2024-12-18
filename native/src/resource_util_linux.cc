#include "resource_util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace shared {

  bool GetResourceDir(std::string& dir) {
    char buff[1024];

    // Retrieve the executable path.
    ssize_t len = readlink("/proc/self/exe", buff, sizeof(buff) - 1);
    if (len == -1)
      return false;

    buff[len] = 0;

    // Add "_files" to the path.
    strcpy(buff + len, "_files");
    dir = std::string(buff);
    return true;
  }

}