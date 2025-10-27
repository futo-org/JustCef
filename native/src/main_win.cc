#include "main.h"

#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <fstream>

#include "include/cef_command_line.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_version_info.h"

#include "ipc.h"
#include "app_factory.h"
#include "client_manager.h"
#include "main_util.h"

namespace shared {
    std::filesystem::path GetExecutablePath() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

int RunMain(HINSTANCE hInstance, LPTSTR /*lpCmdLine*/, int /*nCmdShow*/, void* sandbox_info) {
    CefMainArgs main_args(hInstance);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc == 1) {
        auto executableDir = shared::GetExecutablePath();
        std::ifstream file(executableDir / "launch");
        if (file) {
            std::string command;
            std::getline(file, command);
            if (!command.empty()) {
                std::filesystem::path commandPath = std::filesystem::u8path(command);
                if (commandPath.is_relative()) {
                    commandPath = executableDir / commandPath;
                }
                const std::filesystem::path workingDir = commandPath.parent_path();
                const std::wstring commandStr = commandPath.wstring();
                const std::wstring workingStr = workingDir.wstring();

                SHELLEXECUTEINFOW sei = { sizeof(sei) };
                sei.lpFile = commandStr.c_str();
                sei.lpDirectory= workingStr.c_str();
                sei.nShow = SW_SHOWNORMAL;

                if (!ShellExecuteExW(&sei)) {
                    std::cerr << "Failed to execute command from launch file: " << command << std::endl;
                    if (argv) LocalFree(argv);
                    return 1;
                }
                if (argv) LocalFree(argv);
                return 0;
            }
        }
    }

    CefRefPtr<CefCommandLine> command_line = CreateCommandLine(main_args);
    ProcessType processType = GetProcessType(command_line);
    LOG(INFO) << "main with processType = " << processType << ".";

    if (processType == PROCESS_TYPE_BROWSER) {
        HANDLE readHandle  = INVALID_HANDLE_VALUE;
        HANDLE writeHandle = INVALID_HANDLE_VALUE;

        for (int i = 1; i < argc; i++) {
            std::wstring arg = argv[i];
            if (arg == L"--parent-to-child" && i + 1 < argc) {
                readHandle = reinterpret_cast<HANDLE>(_wcstoui64(argv[++i], nullptr, 10));
            } else if (arg == L"--child-to-parent" && i + 1 < argc) {
                writeHandle = reinterpret_cast<HANDLE>(_wcstoui64(argv[++i], nullptr, 10));
            }
            LOG(INFO) << "Argument " << i << ": " << std::string(arg.begin(), arg.end());
        }

        if (readHandle != INVALID_HANDLE_VALUE && writeHandle != INVALID_HANDLE_VALUE) {
            IPC::Singleton.SetHandles(readHandle, writeHandle);
            LOG(INFO) << "Set handles.";
        } else {
            LOG(INFO) << "Missing handles.";
        }

        if (!command_line->HasSwitch("url") && !IPC::Singleton.HasValidHandles()) {
            std::cerr << "Either URL or IPC handles should be set.";
            if (argv) LocalFree(argv);
            return 1;
        }
    }

    if (argv) LocalFree(argv);
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

    const int exit_code = CefExecuteProcess(main_args, app, sandbox_info);
    if (exit_code >= 0) {
        return exit_code;
    }

    ClientManager manager;
    CefSettings settings;

    if (!sandbox_info) {
        settings.no_sandbox = true;
    }

    std::filesystem::path cachePath;
    bool autoRemoveCachePath = true;
    if (command_line->HasSwitch("cache-path")) {
        std::string userCachePath = command_line->GetSwitchValue("cache-path");
        cachePath = std::filesystem::u8path(userCachePath);
        autoRemoveCachePath = false;
    } else {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        cachePath = std::filesystem::temp_directory_path() / ("dotcef_" + std::to_string(ms));
    }

    CefString(&settings.cache_path) = cachePath.u8string();
    CefString(&settings.root_cache_path) = cachePath.u8string();

    if (!CefInitialize(main_args, settings, app, sandbox_info)) {
        return 1;
    }

    CefRunMessageLoop();
    CefShutdown();

    if (autoRemoveCachePath) {
        std::error_code ec;
        const auto removedCount = std::filesystem::remove_all(cachePath, ec);
        if (ec) {
            LOG(ERROR) << "Failed to delete cache path: " << cachePath.u8string() << ". Error: " << ec.message();
        } else {
            LOG(INFO) << "Deleted " << removedCount << " items from cache path: " << cachePath.u8string();
        }
    }

    return 0;
}

}

#if defined(CEF_USE_BOOTSTRAP)

CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance, LPTSTR lpCmdLine, int nCmdShow, void* sandbox_info, cef_version_info_t* /*version_info*/) {
    return shared::RunMain(hInstance, lpCmdLine, nCmdShow, sandbox_info);
}

#else

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

#if defined(ARCH_CPU_32_BITS)
    const int fiber_exit = CefRunWinMainWithPreferredStackSize(wWinMain, hInstance, lpCmdLine, nCmdShow);
    if (fiber_exit >= 0) {
        return fiber_exit;
    }
#endif

    void* sandbox_info = nullptr;

#if defined(CEF_USE_SANDBOX)
    CefScopedSandboxInfo scoped_sandbox;
    sandbox_info = scoped_sandbox.sandbox_info();
#endif

    return shared::RunMain(hInstance, lpCmdLine, nCmdShow, sandbox_info);
}

#endif