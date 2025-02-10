#include "main.h"

#if defined(CEF_X11)
#include <X11/Xlib.h>
#endif

#include <chrono>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <iostream>

#include "include/base/cef_logging.h"

#include "app_factory.h"
#include "client_manager.h"
#include "main_util.h"
#include "ipc.h"

namespace shared {
#if defined(CEF_X11)
  int XErrorHandlerImpl(Display* display, XErrorEvent* event) {
    LOG(WARNING) << "X error received: "
                << "type " << event->type << ", "
                << "serial " << event->serial << ", "
                << "error_code " << static_cast<int>(event->error_code) << ", "
                << "request_code " << static_cast<int>(event->request_code)
                << ", "
                << "minor_code " << static_cast<int>(event->minor_code);
    return 0;
  }

  int XIOErrorHandlerImpl(Display* display) {
    return 0;
  }
#endif  // defined(CEF_X11)

  std::filesystem::path GetExecutablePath() {
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count != -1) {
      return std::filesystem::path(std::string(result, count)).parent_path();
    }
    return std::filesystem::path();
  }

  // Entry point function for all processes.
  int main(int argc, char* argv[]) {
    if (argc == 1) {
      auto executableDir = GetExecutablePath();
      std::ifstream file(executableDir / "launch");
      if (file) {
        std::string command;
        std::getline(file, command);
        if (!command.empty()) {
          std::filesystem::path commandPath = command;
          if (commandPath.is_relative()) {
            commandPath = executableDir / commandPath;
          }
          std::filesystem::path workingDir = commandPath.parent_path();

          std::string commandStr = commandPath.u8string();
          int result = chdir(workingDir.u8string().c_str());
          if (result == 0) {
            result = system(commandStr.c_str());
            if (result != 0) {
              std::cerr << "Failed to execute command from launch_command file: " << commandStr << std::endl;
              return result;
            }
            return 0;
          } else {
            std::cerr << "Failed to change working directory to: " << workingDir.u8string() << std::endl;
            return 1;
          }
        }
      }
    }

    // Provide CEF with command-line arguments.
    CefMainArgs main_args(argc, argv);

    // Create a temporary CommandLine object.
    CefRefPtr<CefCommandLine> command_line = CreateCommandLine(main_args);
    ProcessType processType = GetProcessType(command_line);
    if (processType == PROCESS_TYPE_BROWSER) {
      int readFd = -1;
      int writeFd = -1;

      for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--parent-to-child" && i + 1 < argc) {
          readFd = std::stoi(argv[++i]);
        } else if (arg == "--child-to-parent" && i + 1 < argc) {
          writeFd = std::stoi(argv[++i]);
        }
      }

      if (readFd != -1 && writeFd != -1) {
        IPC::Singleton.SetHandles(readFd, writeFd);
        LOG(INFO) << "Set handles.";
      } else {
        LOG(INFO) << "Missing handles.";
      }

      if (!command_line->HasSwitch("url") && !IPC::Singleton.HasValidHandles()) {
        std::cerr << "Either URL or IPC handles should be set.";
        return 1;
      }
    }
    
    printf("main with processType = %i.\r\n", processType);
    for (int i = 0; i < argc; i++) {
      printf("Argument %i: '%s'.\r\n", i, argv[i]);
    }

    // Create a CefApp of the correct process type.
    CefRefPtr<CefApp> app;
    switch (processType) {
      case PROCESS_TYPE_BROWSER:
        app = CreateBrowserProcessApp();
        break;
      case PROCESS_TYPE_RENDERER:
        app = CreateRendererProcessApp();
        break;
      case PROCESS_TYPE_OTHER:
        app = CreateOtherProcessApp();
        break;
    }

    // CEF applications have multiple sub-processes (render, plugin, GPU, etc)
    // that share the same executable. This function checks the command-line and,
    // if this is a sub-process, executes the appropriate logic.
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
      // The sub-process has completed so return here.
      return exit_code;
    }

#if defined(CEF_X11)
    // Install xlib error handlers so that the application won't be terminated
    // on non-fatal errors.
    XSetErrorHandler(XErrorHandlerImpl);
    XSetIOErrorHandler(XIOErrorHandlerImpl);
#endif

    // Create the singleton manager instance.
    ClientManager manager;

    // Specify CEF global settings here.
    CefSettings settings;

// When generating projects with CMake the CEF_USE_SANDBOX value will be defined
// automatically. Pass -DUSE_SANDBOX=OFF to the CMake command-line to disable
// use of the sandbox.
#if !defined(CEF_USE_SANDBOX)
    settings.no_sandbox = true;
#endif

    //settings.log_severity = LOGSEVERITY_WARNING;
    //settings.single_process = true;

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::stringstream ss;
    ss << ms;
    std::string uniqueIdentifier = ss.str();

    std::filesystem::path cachePath = std::filesystem::temp_directory_path() / ("dotcef_" + uniqueIdentifier);
    LOG(INFO) << "Cache path: " << cachePath.u8string();
    CefString(&settings.cache_path) = cachePath.u8string();

    // Initialize the CEF browser process. The first browser instance will be
    // created in CefBrowserProcessHandler::OnContextInitialized() after CEF has
    // been initialized. May return false if initialization fails or if early exit
    // is desired (for example, due to process singleton relaunch behavior).
    if (!CefInitialize(main_args, settings, app, nullptr)) {
      return 1;
    }

    // Run the CEF message loop. This will block until CefQuitMessageLoop() is
    // called.
    CefRunMessageLoop();

    IPC::Singleton.Stop();

    // Shut down CEF.
    CefShutdown();

    std::error_code ec;
    auto removedCount = std::filesystem::remove_all(cachePath, ec);
    if (ec) {
      LOG(ERROR) << "Failed to delete cache path: " << cachePath.u8string() << ". Error: " << ec.message();
    } else {
      LOG(INFO) << "Deleted " << removedCount << " items from cache path: " << cachePath.u8string();
    }

    return 0;
  }

}