#include "main_util.h"

#include <chrono>
#include <filesystem>
#include <system_error>

#include "include/base/cef_logging.h"

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace shared {
  
  // These flags must match the Chromium values.
  const char kProcessType[] = "type";
  const char kRendererProcess[] = "renderer";
  #if defined(OS_LINUX)
  const char kZygoteProcess[] = "zygote";
  #endif

  const char kCachePathSwitch[] = "cache-path";
  const char kRootCachePathSwitch[] = "root-cache-path";

  namespace {

    std::filesystem::path PathFromSwitch(const CefRefPtr<CefCommandLine>& command_line, const char* name) {
      const std::string value = command_line->GetSwitchValue(name);
  #if defined(OS_WIN)
      return std::filesystem::u8path(value);
  #else
      return std::filesystem::path(value);
  #endif
    }

    std::filesystem::path MakeTemporaryCachePath() {
      const auto now = std::chrono::system_clock::now();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      return std::filesystem::temp_directory_path() / ("justcef_" + std::to_string(ms));
    }

  }

  CefRefPtr<CefCommandLine> CreateCommandLine(const CefMainArgs& main_args) {
    CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  #if defined(OS_WIN)
    command_line->InitFromString(::GetCommandLineW());
  #else
    command_line->InitFromArgv(main_args.argc, main_args.argv);
  #endif
    return command_line;
  }

  ProcessType GetProcessType(const CefRefPtr<CefCommandLine>& command_line) {
    // The command-line flag won't be specified for the browser process.
    if (!command_line->HasSwitch(kProcessType))
      return PROCESS_TYPE_BROWSER;

    const std::string& process_type = command_line->GetSwitchValue(kProcessType);
    if (process_type == kRendererProcess)
      return PROCESS_TYPE_RENDERER;

  #if defined(OS_LINUX)
    // On Linux the zygote process is used to spawn other process types. Since we
    // don't know what type of process it will be we give it the renderer app.
    if (process_type == kZygoteProcess)
      return PROCESS_TYPE_RENDERER;
  #endif

    return PROCESS_TYPE_OTHER;
  }

  CachePaths ResolveCachePaths(const CefRefPtr<CefCommandLine>& command_line) {
    CachePaths cache_paths;

    const bool has_root = command_line->HasSwitch(kRootCachePathSwitch);
    const bool has_cache = command_line->HasSwitch(kCachePathSwitch);

    if (has_root) {
      cache_paths.rootCachePath = PathFromSwitch(command_line, kRootCachePathSwitch).string();
      if (has_cache)
        cache_paths.cachePath = PathFromSwitch(command_line, kCachePathSwitch).string();
    } else if (has_cache) {
      cache_paths.rootCachePath = PathFromSwitch(command_line, kCachePathSwitch).string();
      cache_paths.cachePath = cache_paths.rootCachePath;
    } else {
      cache_paths.rootCachePath = MakeTemporaryCachePath().string();
      cache_paths.cachePath = cache_paths.rootCachePath;
      cache_paths.temporaryPath = cache_paths.rootCachePath;
    }

    LOG(INFO) << "Root cache path: " << cache_paths.rootCachePath;
    LOG(INFO) << "Cache path: " << (cache_paths.cachePath.empty() ? "(incognito)" : cache_paths.cachePath);

    return cache_paths;
  }

  void RemoveTemporaryCachePath(const CachePaths& cache_paths) {
    if (cache_paths.temporaryPath.empty())
      return;

    std::error_code ec;
    const auto removedCount = std::filesystem::remove_all(cache_paths.temporaryPath, ec);
    if (ec) {
      LOG(ERROR) << "Failed to delete cache path: " << cache_paths.temporaryPath << ". Error: " << ec.message();
    } else {
      LOG(INFO) << "Deleted " << removedCount << " items from cache path: " << cache_paths.temporaryPath;
    }
  }

}
