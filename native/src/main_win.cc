#include "main.h"

#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <fstream>

#include "include/cef_sandbox_win.h"
#include "ipc.h"

#include "app_factory.h"
#include "client_manager.h"
#include "main_util.h"

// When generating projects with CMake the CEF_USE_SANDBOX value will be defined
// automatically if using the required compiler version. Pass -DUSE_SANDBOX=OFF
// to the CMake command-line to disable use of the sandbox.

namespace shared {
    std::filesystem::path GetExecutablePath() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    // Entry point function for all processes.
    int APIENTRY wWinMain(HINSTANCE hInstance) {
        void* sandbox_info = nullptr;

        #if defined(CEF_USE_SANDBOX)
        // Manage the life span of the sandbox information object. This is necessary
        // for sandbox support on Windows. See cef_sandbox_win.h for complete details.
        CefScopedSandboxInfo scoped_sandbox;
        sandbox_info = scoped_sandbox.sandbox_info();
        #endif

        // Provide CEF with command-line arguments.
        CefMainArgs main_args(hInstance);

        int argc;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

        // If no arguments are provided, read from launch_command file and execute.
        if (argc == 1) {
            auto executableDir = GetExecutablePath();
            std::ifstream file(executableDir / "launch");
            if (file) {
                std::string command;
                std::getline(file, command);
                if (!command.empty()) {
                    std::filesystem::path commandPath = std::filesystem::u8path(command);
                    if (commandPath.is_relative()) {
                        commandPath = executableDir / commandPath;
                    }
                    std::filesystem::path workingDir = commandPath.parent_path();
                    std::wstring commandStr = commandPath.wstring();
                    std::wstring workingDirStr = workingDir.wstring();

                    SHELLEXECUTEINFOW sei = { sizeof(sei) };
                    sei.lpFile = commandStr.c_str();
                    sei.lpDirectory = workingDirStr.c_str();
                    sei.nShow = SW_SHOWNORMAL;

                    if (!ShellExecuteExW(&sei)) {
                        std::cerr << "Failed to execute command from launch_command file: " << command << std::endl;
                        return 1;
                    }
                    return 0;
                }
            }
        }

        // Create a temporary CommandLine object.
        CefRefPtr<CefCommandLine> command_line = CreateCommandLine(main_args);
        ProcessType processType = GetProcessType(command_line);
        LOG(INFO) << "main with processType = " << processType << ".";
        if (processType == PROCESS_TYPE_BROWSER) {
            HANDLE readHandle = INVALID_HANDLE_VALUE;
            HANDLE writeHandle = INVALID_HANDLE_VALUE;

            for (int i = 1; i < argc; i++) {
                std::wstring arg = argv[i];
                if (arg == L"--parent-to-child" && i + 1 < argc) {
                    readHandle = (HANDLE)std::stoull(argv[++i]);
                } else if (arg == L"--child-to-parent" && i + 1 < argc) {
                    writeHandle = (HANDLE)std::stoull(argv[++i]);
                }

                LOG(INFO) << "Argument " << i << ": " << arg;
            }

            LocalFree(argv);
            if (readHandle != INVALID_HANDLE_VALUE && writeHandle != INVALID_HANDLE_VALUE) {
                IPC::Singleton.SetHandles(readHandle, writeHandle);
                LOG(INFO) << "Set handles.";
            } else {
                LOG(INFO) << "Missing handles.";
            }

            if (!command_line->HasSwitch("url") && !IPC::Singleton.HasValidHandles()) {
                std::cerr << "Either URL or IPC handles should be set.";
                return 1;
            }
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
        int exit_code = CefExecuteProcess(main_args, app, sandbox_info);
        if (exit_code >= 0) {
            // The sub-process has completed so return here.
            return exit_code;
        }

        // Create the singleton manager instance.
        ClientManager manager;

        // Specify CEF global settings here.
        CefSettings settings;
        //settings.log_severity = LOGSEVERITY_WARNING;

        // Support the command-line switch to specify a cache path.
        // If --cache-path is provided, use that path and do not remove it on exit.
        // Otherwise, create a temporary directory that will be removed after shutdown.
        std::filesystem::path cachePath;
        bool autoRemoveCachePath = true;
        if (command_line->HasSwitch("cache-path")) {
            std::string userCachePath = command_line->GetSwitchValue("cache-path");
            cachePath = std::filesystem::u8path(userCachePath);
            autoRemoveCachePath = false;
        } else {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::stringstream ss;
            ss << ms;
            std::string uniqueIdentifier = ss.str();
            cachePath = std::filesystem::temp_directory_path() / ("dotcef_" + uniqueIdentifier);
        }

        // Set the cache paths for CEF.
        CefString(&settings.cache_path) = cachePath.u8string();
        CefString(&settings.root_cache_path) = cachePath.u8string();

    #if !defined(CEF_USE_SANDBOX)
        settings.no_sandbox = true;
    #endif

        // Initialize the CEF browser process. The first browser instance will be
        // created in CefBrowserProcessHandler::OnContextInitialized() after CEF has
        // been initialized. May return false if initialization fails or if early exit
        // is desired (for example, due to process singleton relaunch behavior).
        if (!CefInitialize(main_args, settings, app, sandbox_info)) {
            return 1;
        }

        // Run the CEF message loop. This will block until CefQuitMessageLoop() is
        // called.
        CefRunMessageLoop();

        // Shut down CEF.
        CefShutdown();

        // Remove the cache directory only if it was auto-generated.
        if (autoRemoveCachePath) {
            std::error_code ec;
            auto removedCount = std::filesystem::remove_all(cachePath, ec);
            if (ec) {
                LOG(ERROR) << "Failed to delete cache path: " << cachePath.u8string() << ". Error: " << ec.message();
            } else {
                LOG(INFO) << "Deleted " << removedCount << " items from cache path: " << cachePath.u8string();
            }
        }

        return 0;
    }

}