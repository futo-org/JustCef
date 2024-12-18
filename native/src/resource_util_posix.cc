#include "resource_util.h"

#include <stdio.h>

namespace shared {

  bool FileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
      fclose(f);
      return true;
    }
    return false;
  }

  bool ReadFileToString(const char* path, std::string& data) {
    // Implementation adapted from base/file_util.cc
    FILE* file = fopen(path, "rb");
    if (!file)
      return false;

    char buf[1 << 16];
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), file)) > 0)
      data.append(buf, len);
    fclose(file);

    return true;
  }

  bool GetResourceString(const std::string& resource_path,
                        std::string& out_data) {
    std::string path;
    if (!GetResourceDir(path))
      return false;

    path.append("/");
    path.append(resource_path);

    return ReadFileToString(path.c_str(), out_data);
  }

  CefRefPtr<CefStreamReader> GetResourceReader(const std::string& resource_path) {
    std::string path;
    if (!GetResourceDir(path))
      return nullptr;

    path.append("/");
    path.append(resource_path);

    if (!FileExists(path.c_str()))
      return nullptr;

    return CefStreamReader::CreateForFile(path);
  }

}
