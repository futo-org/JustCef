#include "JustCefProcess.h"
#include "AsyncSignal.h"
#include "DataStream.h"
#include "Packet.h"
#include "Rpc.h"
#include "Transport.h"
#include "WindowInternals.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
extern char** environ;
#endif

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace justcef
{
namespace
{

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs))
        {
            return false;
        }
    }

    return true;
}

std::optional<std::string> TryGetFirstHeaderValue(const HeaderMap& headers, std::string_view key)
{
    for (const auto& [header_key, values] : headers)
    {
        if (!EqualsIgnoreCase(header_key, key) || values.empty())
        {
            continue;
        }

        return values.front();
    }

    return std::nullopt;
}

HeaderMap FilterResponseHeaders(const HeaderMap& headers)
{
    HeaderMap filtered;
    for (const auto& [key, values] : headers)
    {
        if (EqualsIgnoreCase(key, "transfer-encoding"))
        {
            bool has_chunked = false;
            for (const auto& value : values)
            {
                if (EqualsIgnoreCase(value, "chunked"))
                {
                    has_chunked = true;
                    break;
                }
            }

            if (has_chunked)
            {
                continue;
            }
        }

        filtered.insert({key, values});
    }

    return filtered;
}

std::size_t CountHeaderValuePairs(const HeaderMap& headers)
{
    std::size_t count = 0;
    for (const auto& [_, values] : headers)
    {
        count += values.size();
    }
    return count;
}

std::optional<std::uint64_t> ParseContentLength(const HeaderMap& headers)
{
    const auto value = TryGetFirstHeaderValue(headers, "content-length");
    if (!value)
    {
        return std::nullopt;
    }

    try
    {
        return static_cast<std::uint64_t>(std::stoull(*value));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

template <typename T> T ReadRequired(detail::PacketReader& reader, const char* field_name)
{
    const auto value = reader.Read<T>();
    if (!value)
    {
        throw detail::ProtocolError(std::string("Missing field: ") + field_name);
    }
    return *value;
}

std::string ReadRequiredString(detail::PacketReader& reader, const char* field_name)
{
    const auto value = reader.ReadSizePrefixedString();
    if (!value)
    {
        throw detail::ProtocolError(std::string("Missing field: ") + field_name);
    }
    return *value;
}

constexpr std::uint64_t kInlineProxyBodyLimit = 1024 * 1024;

void WriteInlinePayload(detail::PacketWriter& writer, std::string_view payload)
{
    writer.Write<std::uint32_t>(static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty())
    {
        writer.WriteBytes(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    }
}

struct HandlerReply
{
    Status status = Status::Ok;
    std::optional<std::string> message;
    std::shared_ptr<DataStream> stream;
};

class TransportLink
{
public:
    void Set(std::shared_ptr<detail::Transport> transport)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport_ = std::move(transport);
    }

    std::shared_ptr<detail::Transport> Get() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return transport_;
    }

    bool Send(detail::OutgoingPacket packet) const
    {
        auto transport = Get();
        return transport && transport->Enqueue(std::move(packet));
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<detail::Transport> transport_;
};

std::uint32_t ClampMilliseconds(std::chrono::milliseconds value)
{
    return static_cast<std::uint32_t>(std::clamp<std::chrono::milliseconds::rep>(value.count(), 0, UINT32_MAX));
}

struct ParsedWindowRequest
{
    int identifier = 0;
    IPCRequest request;
};

std::vector<std::string> SplitArgumentsPosix(const std::string& arguments)
{
    std::vector<std::string> parts;
    std::string current;
    bool in_single_quotes = false;
    bool in_double_quotes = false;
    bool escaped = false;

    for (const char character : arguments)
    {
        if (escaped)
        {
            current.push_back(character);
            escaped = false;
            continue;
        }

        if (character == '\\' && !in_single_quotes)
        {
            escaped = true;
            continue;
        }

        if (character == '"' && !in_single_quotes)
        {
            in_double_quotes = !in_double_quotes;
            continue;
        }

        if (character == '\'' && !in_double_quotes)
        {
            in_single_quotes = !in_single_quotes;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(character)) && !in_single_quotes && !in_double_quotes)
        {
            if (!current.empty())
            {
                parts.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(character);
    }

    if (escaped || in_single_quotes || in_double_quotes)
    {
        throw std::runtime_error("Failed to parse command line arguments.");
    }

    if (!current.empty())
    {
        parts.push_back(std::move(current));
    }

    return parts;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
    {
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
    }

    std::wstring converted(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), converted.data(), size);
    return converted;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument)
{
    if (argument.empty())
    {
        return L"\"\"";
    }

    const bool needs_quotes = argument.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes)
    {
        return argument;
    }

    std::wstring quoted = L"\"";
    unsigned int backslashes = 0;

    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (character == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashes = 0;
            continue;
        }

        if (backslashes > 0)
        {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
        }

        quoted.push_back(character);
    }

    if (backslashes > 0)
    {
        quoted.append(backslashes * 2, L'\\');
    }

    quoted.push_back(L'"');
    return quoted;
}

std::vector<std::string> SplitArguments(const std::string& arguments)
{
    if (arguments.empty())
    {
        return {};
    }

    const std::wstring wide_arguments = Utf8ToWide(arguments);
    int argument_count = 0;
    LPWSTR* parsed = CommandLineToArgvW(wide_arguments.c_str(), &argument_count);
    if (parsed == nullptr)
    {
        throw std::runtime_error("Failed to parse Windows command line arguments.");
    }

    std::vector<std::string> result;
    result.reserve(argument_count);
    for (int index = 0; index < argument_count; ++index)
    {
        const std::wstring current = parsed[index];
        const int size = WideCharToMultiByte(CP_UTF8, 0, current.data(), static_cast<int>(current.size()), nullptr, 0, nullptr, nullptr);
        std::string converted(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, current.data(), static_cast<int>(current.size()), converted.data(), size, nullptr, nullptr);
        result.push_back(std::move(converted));
    }

    LocalFree(parsed);
    return result;
}
#else
std::vector<std::string> SplitArguments(const std::string& arguments)
{
    return SplitArgumentsPosix(arguments);
}

int CreateCloseOnExecPipe(int fds[2])
{
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC);
#else
    if (::pipe(fds) != 0)
    {
        return -1;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

char** CurrentEnvironment()
{
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return environ;
#endif
}
#endif

std::filesystem::path CurrentExecutablePath()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = 0;
    while (true)
    {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            throw std::runtime_error("Failed to resolve the current executable path.");
        }

        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }

        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        throw std::runtime_error("Failed to resolve the current executable path.");
    }

    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()));
#else
    std::array<char, 4096> buffer{};
    const ssize_t size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0)
    {
        throw std::runtime_error("Failed to resolve the current executable path.");
    }

    buffer[static_cast<std::size_t>(size)] = '\0';
    return std::filesystem::path(buffer.data());
#endif
}

void CurrentModulePathMarker()
{
}

std::optional<std::filesystem::path> CurrentModulePath()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&CurrentModulePathMarker), &module))
    {
        return std::nullopt;
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = 0;
    while (true)
    {
        length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return std::nullopt;
        }

        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }

        buffer.resize(buffer.size() * 2);
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&CurrentModulePathMarker), &info) == 0 || info.dli_fname == nullptr)
    {
        return std::nullopt;
    }

    return std::filesystem::path(info.dli_fname);
#endif
}

std::string GetNativeFileName()
{
#ifdef _WIN32
    return "justcefnative.exe";
#else
    return "justcefnative";
#endif
}

std::vector<std::filesystem::path> BuildSearchPaths()
{
    const std::string native_file_name = GetNativeFileName();
    const auto executable_directory = CurrentExecutablePath().parent_path();
    const auto module_path = CurrentModulePath();
    const auto current_working_directory = std::filesystem::current_path();

    std::vector<std::filesystem::path> base_directories;
    if (module_path)
    {
        base_directories.push_back(module_path->parent_path());
    }
    base_directories.push_back(executable_directory);
    base_directories.push_back(current_working_directory);

    std::vector<std::filesystem::path> search_paths;
    std::set<std::filesystem::path> seen;

    const auto append = [&](const std::filesystem::path& path)
    {
        if (seen.insert(path).second)
        {
            search_paths.push_back(path);
        }
    };

    for (const auto& base : base_directories)
    {
#ifdef __APPLE__
        append(base / "justcefnative.app/Contents/MacOS" / native_file_name);
        append(base / "JustCef.app/Contents/MacOS" / native_file_name);
        append(base / "../Frameworks/justcefnative.app/Contents/MacOS" / native_file_name);
        append(base / "../Frameworks/JustCef.app/Contents/MacOS" / native_file_name);
#endif
        append(base / "cef" / native_file_name);
        append(base / native_file_name);
    }

    return search_paths;
}

} // namespace

class JustCefProcessImpl : public WindowCommandTarget, public std::enable_shared_from_this<JustCefProcessImpl>
{
public:
    explicit JustCefProcessImpl(asio::any_io_executor executor) : executor_(std::move(executor)), event_strand_(executor_)
    {
        rpc_ = std::make_shared<detail::Rpc>(executor_,
                                             [link = link_](detail::OutgoingPacket packet)
                                             {
                                                 return link->Send(std::move(packet));
                                             });
    }

    ~JustCefProcessImpl() { Dispose(); }

    void Start(const StartOptions& options)
    {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true))
        {
            throw std::runtime_error("Process has already been started.");
        }
        if (shutdown_.load())
        {
            started_ = false;
            throw std::runtime_error("Process has been disposed.");
        }

        start_options_ = options;

        try
        {
            const auto native_path = JustCefProcess::ResolveNativeExecutablePath(options.native_executable_path);
            const auto working_directory = options.working_directory ? std::filesystem::absolute(*options.working_directory) : native_path.parent_path();
            const auto additional_arguments = SplitArguments(options.arguments);

            Logger::Info("JustCefProcess", "Searching for justcefnative, search paths:");
            for (const auto& path : JustCefProcess::GenerateSearchPaths())
            {
                Logger::Info("JustCefProcess", std::string(" - ") + path.string());
            }
            Logger::Info("JustCefProcess", "Working directory '" + working_directory.string() + "'.");
            Logger::Info("JustCefProcess", "CEF exe path '" + native_path.string() + "'.");
#ifndef JUSTCEF_EXPECTED_VERSION
#define JUSTCEF_EXPECTED_VERSION 0
#endif
            Logger::Info("JustCefProcess", "JustCef expected native runtime v" + std::to_string(JUSTCEF_EXPECTED_VERSION) + ".");

#ifdef _WIN32
            SECURITY_ATTRIBUTES security_attributes{};
            security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
            security_attributes.bInheritHandle = FALSE;

            HANDLE child_read_handle = INVALID_HANDLE_VALUE;
            HANDLE child_write_handle = INVALID_HANDLE_VALUE;

            if (!CreatePipe(&child_read_handle, &write_handle_, &security_attributes, 0))
            {
                throw std::runtime_error("Failed to create parent-to-child pipe.");
            }

            if (!CreatePipe(&read_handle_, &child_write_handle, &security_attributes, 0))
            {
                CloseHandle(child_read_handle);
                throw std::runtime_error("Failed to create child-to-parent pipe.");
            }

            SetHandleInformation(child_read_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(child_write_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

            std::vector<std::wstring> command_parts;
            command_parts.push_back(native_path.wstring());
            command_parts.push_back(L"--change-stack-guard-on-fork=disable");
            command_parts.push_back(L"--parent-to-child");
            command_parts.push_back(std::to_wstring(reinterpret_cast<std::uintptr_t>(child_read_handle)));
            command_parts.push_back(L"--child-to-parent");
            command_parts.push_back(std::to_wstring(reinterpret_cast<std::uintptr_t>(child_write_handle)));
            for (const auto& argument : additional_arguments)
            {
                command_parts.push_back(Utf8ToWide(argument));
            }

            std::wstring command_line;
            for (std::size_t index = 0; index < command_parts.size(); ++index)
            {
                if (index > 0)
                {
                    command_line.push_back(L' ');
                }
                command_line += QuoteWindowsArgument(command_parts[index]);
            }

            SIZE_T attribute_size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
            std::vector<char> attribute_storage(attribute_size);
            auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
            if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size))
            {
                CloseHandle(child_read_handle);
                CloseHandle(child_write_handle);
                throw std::runtime_error("Failed to initialize the process attribute list.");
            }

            HANDLE inherited_handles[2] = {child_read_handle, child_write_handle};
            if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles, sizeof(inherited_handles), nullptr, nullptr))
            {
                DeleteProcThreadAttributeList(attribute_list);
                CloseHandle(child_read_handle);
                CloseHandle(child_write_handle);
                throw std::runtime_error("Failed to restrict inherited handles.");
            }

            STARTUPINFOEXW startup_info{};
            startup_info.StartupInfo.cb = sizeof(startup_info);
            startup_info.lpAttributeList = attribute_list;
            PROCESS_INFORMATION process_information{};

            std::wstring mutable_command_line = command_line;
            const BOOL created = CreateProcessW(nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                                working_directory.wstring().c_str(), &startup_info.StartupInfo, &process_information);
            DeleteProcThreadAttributeList(attribute_list);
            if (!created)
            {
                CloseHandle(child_read_handle);
                CloseHandle(child_write_handle);
                throw std::runtime_error("Failed to start justcefnative.");
            }

            CloseHandle(child_read_handle);
            CloseHandle(child_write_handle);
            process_handle_ = process_information.hProcess;
            CloseHandle(process_information.hThread);
#else
            int parent_to_child[2] = {-1, -1};
            int child_to_parent[2] = {-1, -1};

            if (CreateCloseOnExecPipe(parent_to_child) != 0)
            {
                throw std::runtime_error("Failed to create parent-to-child pipe.");
            }

            if (CreateCloseOnExecPipe(child_to_parent) != 0)
            {
                ::close(parent_to_child[0]);
                ::close(parent_to_child[1]);
                throw std::runtime_error("Failed to create child-to-parent pipe.");
            }

            const int child_read = ::fcntl(parent_to_child[0], F_DUPFD_CLOEXEC, 10);
            const int child_write = ::fcntl(child_to_parent[1], F_DUPFD_CLOEXEC, 10);
            ::close(parent_to_child[0]);
            ::close(child_to_parent[1]);

            std::vector<std::string> argv_storage;
            argv_storage.push_back(native_path.string());
            argv_storage.push_back("--change-stack-guard-on-fork=disable");
            argv_storage.push_back("--parent-to-child");
            argv_storage.push_back("3");
            argv_storage.push_back("--child-to-parent");
            argv_storage.push_back("4");
            argv_storage.insert(argv_storage.end(), additional_arguments.begin(), additional_arguments.end());

            std::vector<char*> argv;
            argv.reserve(argv_storage.size() + 1);
            for (auto& value : argv_storage)
            {
                argv.push_back(value.data());
            }
            argv.push_back(nullptr);

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawnattr_t attributes;
            posix_spawnattr_init(&attributes);
            sigset_t empty_mask;
            sigemptyset(&empty_mask);
            sigset_t default_signals;
            sigemptyset(&default_signals);
            sigaddset(&default_signals, SIGPIPE);

            int result = child_read < 0 || child_write < 0 ? EMFILE : 0;
            if (result == 0)
            {
                result = posix_spawn_file_actions_adddup2(&actions, child_read, 3);
            }
            if (result == 0)
            {
                result = posix_spawn_file_actions_adddup2(&actions, child_write, 4);
            }
#if defined(__APPLE__) || (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29)))
            if (result == 0)
            {
                result = posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
            }
#else
            Logger::Warning("JustCefProcess", "This platform cannot set the working directory of justcefnative; it inherits the current one.");
#endif
            if (result == 0)
            {
                result = posix_spawnattr_setsigmask(&attributes, &empty_mask);
            }
            if (result == 0)
            {
                result = posix_spawnattr_setsigdefault(&attributes, &default_signals);
            }
            if (result == 0)
            {
                result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
            }

            pid_t child_pid = -1;
            if (result == 0)
            {
                result = posix_spawn(&child_pid, native_path.c_str(), &actions, &attributes, argv.data(), CurrentEnvironment());
            }

            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attributes);
            if (child_read >= 0)
            {
                ::close(child_read);
            }
            if (child_write >= 0)
            {
                ::close(child_write);
            }

            if (result != 0)
            {
                ::close(parent_to_child[1]);
                ::close(child_to_parent[0]);
                throw std::runtime_error("Failed to start justcefnative.");
            }

            write_handle_ = parent_to_child[1];
            read_handle_ = child_to_parent[0];
            child_pid_ = child_pid;
#endif

            waiter_thread_ = std::thread(
                [this]()
                {
                    WaitForChild();
                });

            auto transport =
                std::make_shared<detail::Transport>(std::exchange(read_handle_, detail::InvalidNativeHandle()), std::exchange(write_handle_, detail::InvalidNativeHandle()));
            link_->Set(transport);
            transport->Start(
                [this](detail::IncomingPacket&& packet)
                {
                    OnPacket(std::move(packet));
                },
                [this]()
                {
                    Shutdown();
                });
        }
        catch (...)
        {
            started_ = false;
            if (waiter_thread_.joinable())
            {
                Terminate();
            }
            KillChild();
            CloseTransportHandles();
            throw;
        }
    }

    bool HasExited() const { return !started_.load() || shutdown_.load(); }

    std::vector<std::shared_ptr<JustCefWindow>> Windows() const
    {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        std::vector<std::shared_ptr<JustCefWindow>> windows;
        windows.reserve(windows_.size());
        for (const auto& record : windows_)
        {
            if (record.window)
            {
                windows.push_back(record.window);
            }
        }
        return windows;
    }

    std::shared_ptr<JustCefWindow> GetWindow(int identifier) const
    {
        const auto record = GetWindowRecord(identifier);
        return record ? record->window : nullptr;
    }

    std::shared_ptr<JustCefBrowser> GetBrowser(int identifier) const
    {
        const auto record = GetWindowRecord(identifier);
        return record ? record->browser : nullptr;
    }

    std::vector<std::shared_ptr<JustCefView>> WindowViews(int identifier) const override
    {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        std::vector<std::shared_ptr<JustCefView>> views;
        for (const auto& record : windows_)
        {
            if (record.window || !record.browser)
            {
                continue;
            }

            auto view = std::static_pointer_cast<JustCefView>(record.browser);
            auto parent = view->Parent();
            if (parent && parent->Identifier() == identifier)
            {
                views.push_back(std::move(view));
            }
        }
        return views;
    }

    void WaitForExit() const
    {
        EnsureStarted();
        exit_signal_.Wait();
    }

    asio::awaitable<void> WaitForExitAsync() const
    {
        EnsureStarted();
        co_await exit_signal_.AsyncWait(executor_);
    }

    void WaitForReady() const
    {
        EnsureStarted();
        ready_signal_.Wait();
    }

    asio::awaitable<void> WaitForReadyAsync() const
    {
        EnsureStarted();
        co_await ready_signal_.AsyncWait(executor_);
    }

    asio::awaitable<void> EchoAsync(std::vector<std::uint8_t> data)
    {
        detail::PacketWriter writer;
        writer.WriteBytes(data);
        co_await AsyncVoidCall(detail::OpcodeController::Echo, std::move(writer));
    }

    asio::awaitable<void> PingAsync() { co_await AsyncVoidCall(detail::OpcodeController::Ping, detail::PacketWriter{}); }

    asio::awaitable<void> PrintAsync(std::string message)
    {
        detail::PacketWriter writer;
        writer.WriteString(message);
        co_await AsyncVoidCall(detail::OpcodeController::Print, std::move(writer));
    }

    asio::awaitable<std::shared_ptr<JustCefWindow>> CreateWindowAsync(WindowCreateOptions options)
    {
        EnsureStarted();

        if (options.proxy_requests && !options.request_proxy)
        {
            throw std::invalid_argument("When proxy_requests is true, request_proxy must be set.");
        }
        if (options.modify_requests && !options.request_modifier)
        {
            throw std::invalid_argument("When modify_requests is true, request_modifier must be set.");
        }

        const bool bridge_enabled = options.bridge_enabled || static_cast<bool>(options.bridge_rpc_handler);
        if (options.bridge_rpc_handler && !bridge_enabled)
        {
            throw std::invalid_argument("When bridge RPC is configured, bridge_enabled must be true.");
        }

        const bool views_enabled = options.views_enabled || static_cast<bool>(options.view_created_handler);

        detail::PacketWriter writer;
        writer.Write<bool>(options.resizable);
        writer.Write<bool>(options.frameless);
        writer.Write<bool>(options.fullscreen);
        writer.Write<bool>(options.centered);
        writer.Write<bool>(options.shown);
        writer.Write<bool>(options.context_menu_enable);
        writer.Write<bool>(options.developer_tools_enabled);
        writer.Write<bool>(options.modify_requests);
        writer.Write<bool>(options.modify_request_body);
        writer.Write<bool>(options.proxy_requests);
        writer.Write<bool>(options.log_console);
        writer.Write<bool>(bridge_enabled);
        writer.Write<std::int32_t>(options.minimum_width);
        writer.Write<std::int32_t>(options.minimum_height);
        writer.Write<std::int32_t>(options.preferred_width);
        writer.Write<std::int32_t>(options.preferred_height);
        writer.WriteSizePrefixedString(options.url);
        writer.WriteSizePrefixedString(options.title);
        writer.WriteSizePrefixedString(options.icon_path);
        writer.WriteSizePrefixedString(options.app_id);
        writer.Write<bool>(views_enabled);
        writer.Write<std::uint32_t>(ClampMilliseconds(options.modify_timeout));
        writer.Write<std::uint8_t>(static_cast<std::uint8_t>(options.modify_timeout_policy));
        writer.Write<std::uint32_t>(ClampMilliseconds(options.proxy_open_timeout));

        auto window = std::make_shared<std::shared_ptr<JustCefWindow>>();
        (void)co_await AsyncRawCall(detail::OpcodeController::WindowCreate, std::move(writer), true,
                                    [weak_self = weak_from_this(), window, options](detail::PacketReader& reader)
                                    {
                                        if (auto self = weak_self.lock())
                                        {
                                            *window = self->RegisterWindow(reader, options);
                                        }
                                    });
        co_return *window;
    }

    std::shared_ptr<JustCefWindow> RegisterWindow(detail::PacketReader& reader, const WindowCreateOptions& options)
    {
        const int identifier = ReadRequired<std::int32_t>(reader, "windowIdentifier");

        auto shared = std::make_shared<WindowShared>();
        shared->executor = executor_;
        shared->bridge_rpc_handler = options.bridge_rpc_handler;
        shared->view_created_handler = options.view_created_handler;
        if (!options.url.empty())
        {
            shared->loading.ArmNavigation();
        }

        auto window = std::shared_ptr<JustCefWindow>(new JustCefWindow(identifier, shared_from_this(), shared));
        window->SetRequestProxy(options.request_proxy);
        window->SetRequestModifier(options.request_modifier);

        {
            std::lock_guard<std::mutex> lock(windows_mutex_);
            windows_.push_back(WindowRecord{
                .identifier = identifier,
                .window = window,
                .browser = window,
                .shared = std::move(shared),
            });
        }

        return window;
    }

    asio::awaitable<void> NotifyExitAsync()
    {
        co_await NotifyAsync(detail::OpcodeControllerNotification::Exit);
    }

    asio::awaitable<void> WindowMaximizeAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowMaximize, identifier); }

    asio::awaitable<void> WindowMinimizeAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowMinimize, identifier); }

    asio::awaitable<void> WindowRestoreAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowRestore, identifier); }

    asio::awaitable<void> WindowShowAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowShow, identifier); }

    asio::awaitable<void> WindowHideAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowHide, identifier); }

    asio::awaitable<void> WindowActivateAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowActivate, identifier); }

    asio::awaitable<void> WindowBringToTopAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowBringToTop, identifier); }

    asio::awaitable<void> WindowSetAlwaysOnTopAsync(int identifier, bool always_on_top)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(always_on_top);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetAlwaysOnTop, std::move(writer));
    }

    asio::awaitable<void> WindowSetFullscreenAsync(int identifier, bool fullscreen)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(fullscreen);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetFullscreen, std::move(writer));
    }

    asio::awaitable<void> WindowCenterSelfAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowCenterSelf, identifier); }

    asio::awaitable<void> WindowSetProxyRequestsAsync(int identifier, bool enable_proxy_requests)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(enable_proxy_requests);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetProxyRequests, std::move(writer));
    }

    asio::awaitable<void> WindowSetModifyRequestsAsync(int identifier, bool enable_modify_requests, bool enable_modify_body)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        const std::uint8_t flags = static_cast<std::uint8_t>(((enable_modify_body ? 1U : 0U) << 1U) | (enable_modify_requests ? 1U : 0U));
        writer.Write<std::uint8_t>(flags);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetModifyRequests, std::move(writer));
    }

    asio::awaitable<void> RequestFocusAsync(int identifier) { co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowRequestFocus, identifier); }

    asio::awaitable<void> WindowLoadUrlAsync(int identifier, std::string url)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(url);
        co_await AsyncVoidCall(detail::OpcodeController::WindowLoadUrl, std::move(writer));
    }

    asio::awaitable<void> WindowSetPositionAsync(int identifier, int x, int y)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<std::int32_t>(x);
        writer.Write<std::int32_t>(y);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetPosition, std::move(writer));
    }

    asio::awaitable<Position> WindowGetPositionAsync(int identifier)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_return co_await AsyncParsedCall<Position>(detail::OpcodeController::WindowGetPosition, std::move(writer),
                                                     [](detail::PacketReader& reader)
                                                     {
                                                         return Position{
                                                             .x = ReadRequired<std::int32_t>(reader, "x"),
                                                             .y = ReadRequired<std::int32_t>(reader, "y"),
                                                         };
                                                     });
    }

    asio::awaitable<void> WindowSetSizeAsync(int identifier, int width, int height)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<std::int32_t>(width);
        writer.Write<std::int32_t>(height);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetSize, std::move(writer));
    }

    asio::awaitable<Size> WindowGetSizeAsync(int identifier)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_return co_await AsyncParsedCall<Size>(detail::OpcodeController::WindowGetSize, std::move(writer),
                                                 [](detail::PacketReader& reader)
                                                 {
                                                     return Size{
                                                         .width = ReadRequired<std::int32_t>(reader, "width"),
                                                         .height = ReadRequired<std::int32_t>(reader, "height"),
                                                     };
                                                 });
    }

    asio::awaitable<void> WindowSetZoomAsync(int identifier, double zoom)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<double>(zoom);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetZoom, std::move(writer));
    }

    asio::awaitable<double> WindowGetZoomAsync(int identifier)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_return co_await AsyncParsedCall<double>(detail::OpcodeController::WindowGetZoom, std::move(writer),
                                                   [](detail::PacketReader& reader)
                                                   {
                                                       return ReadRequired<double>(reader, "zoom");
                                                   });
    }

    asio::awaitable<void> WindowSetDevelopmentToolsEnabledAsync(int identifier, bool enabled)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(enabled);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetDevelopmentToolsEnabled, std::move(writer));
    }

    asio::awaitable<void> WindowSetDevelopmentToolsVisibleAsync(int identifier, bool visible)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(visible);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetDevelopmentToolsVisible, std::move(writer));
    }

    asio::awaitable<void> WindowCloseAsync(int identifier, bool force_close)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(force_close);
        co_await AsyncVoidCall(detail::OpcodeController::WindowClose, std::move(writer));
    }

    asio::awaitable<std::vector<std::string>> WindowPickFileAsync(int identifier, bool multiple, std::vector<FileFilter> filters)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(multiple);
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(filters.size()));
        for (const auto& filter : filters)
        {
            writer.WriteSizePrefixedString(filter.name);
            writer.WriteSizePrefixedString(filter.pattern);
        }

        co_return co_await AsyncParsedCall<std::vector<std::string>>(detail::OpcodeController::PickFile, std::move(writer), false,
                                                                     [](detail::PacketReader& reader)
                                                                     {
                                                                         const auto count = ReadRequired<std::uint32_t>(reader, "pathCount");
                                                                         std::vector<std::string> paths;
                                                                         paths.reserve(std::min<std::size_t>(count, reader.RemainingSize() / sizeof(std::int32_t)));
                                                                         for (std::uint32_t index = 0; index < count; ++index)
                                                                         {
                                                                             paths.push_back(ReadRequiredString(reader, "path"));
                                                                         }
                                                                         return paths;
                                                                     });
    }

    asio::awaitable<std::string> WindowPickDirectoryAsync(int identifier)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_return co_await AsyncParsedCall<std::string>(detail::OpcodeController::PickDirectory, std::move(writer), false,
                                                        [](detail::PacketReader& reader)
                                                        {
                                                            return ReadRequiredString(reader, "directory");
                                                        });
    }

    asio::awaitable<std::string> WindowSaveFileAsync(int identifier, std::string default_name, std::vector<FileFilter> filters)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(default_name);
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(filters.size()));
        for (const auto& filter : filters)
        {
            writer.WriteSizePrefixedString(filter.name);
            writer.WriteSizePrefixedString(filter.pattern);
        }

        co_return co_await AsyncParsedCall<std::string>(detail::OpcodeController::SaveFile, std::move(writer), false,
                                                        [](detail::PacketReader& reader)
                                                        {
                                                            return ReadRequiredString(reader, "path");
                                                        });
    }

    asio::awaitable<std::vector<std::string>> PickFileAsync(bool multiple, std::vector<FileFilter> filters)
    {
        auto windows = Windows();
        if (windows.empty())
        {
            throw std::runtime_error("PickFileAsync requires an open window. Use JustCefWindow::PickFileAsync.");
        }
        if (windows.size() > 1)
        {
            throw std::runtime_error("PickFileAsync is ambiguous when multiple windows are open. Use JustCefWindow::PickFileAsync.");
        }
        co_return co_await WindowPickFileAsync(windows.front()->Identifier(), multiple, std::move(filters));
    }

    asio::awaitable<std::string> PickDirectoryAsync()
    {
        auto windows = Windows();
        if (windows.empty())
        {
            throw std::runtime_error("PickDirectoryAsync requires an open window. Use JustCefWindow::PickDirectoryAsync.");
        }
        if (windows.size() > 1)
        {
            throw std::runtime_error("PickDirectoryAsync is ambiguous when multiple windows are open. Use JustCefWindow::PickDirectoryAsync.");
        }
        co_return co_await WindowPickDirectoryAsync(windows.front()->Identifier());
    }

    asio::awaitable<std::string> SaveFileAsync(std::string default_name, std::vector<FileFilter> filters)
    {
        auto windows = Windows();
        if (windows.empty())
        {
            throw std::runtime_error("SaveFileAsync requires an open window. Use JustCefWindow::SaveFileAsync.");
        }
        if (windows.size() > 1)
        {
            throw std::runtime_error("SaveFileAsync is ambiguous when multiple windows are open. Use JustCefWindow::SaveFileAsync.");
        }
        co_return co_await WindowSaveFileAsync(windows.front()->Identifier(), std::move(default_name), std::move(filters));
    }

    asio::awaitable<DevToolsMethodResult> WindowExecuteDevToolsMethodAsync(int identifier, std::string method_name, std::optional<std::string> json)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(method_name);
        writer.Write<bool>(json.has_value());
        if (json)
        {
            WriteInlinePayload(writer, *json);
        }

        co_return co_await AsyncParsedCall<DevToolsMethodResult>(
            detail::OpcodeController::WindowExecuteDevToolsMethod, std::move(writer),
            [this](detail::PacketReader& reader)
            {
                DevToolsMethodResult result;
                result.success = ReadRequired<bool>(reader, "success");
                result.data = DeserializeBinaryPayload(reader, result.success ? "DevTools method result" : "DevTools method error payload");
                return result;
            });
    }

    asio::awaitable<std::string> WindowBridgeRpcAsync(int identifier, std::string method, std::optional<std::string> json)
    {
        if (method.empty())
        {
            throw std::invalid_argument("Bridge RPC method must be a non-empty string.");
        }

        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(method);

        const std::string payload = json.value_or("null");
        WriteInlinePayload(writer, payload);

        co_return co_await AsyncParsedCall<std::string>(
            detail::OpcodeController::WindowBridgeRpc, std::move(writer),
            [this](detail::PacketReader& reader)
            {
                return DeserializeBridgeRpcPayload(reader, "bridge RPC response payload");
            });
    }

    asio::awaitable<void> WindowSetTitleAsync(int identifier, std::string title)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(title);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetTitle, std::move(writer));
    }

    asio::awaitable<void> WindowSetIconAsync(int identifier, std::string icon_path)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(icon_path);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetIcon, std::move(writer));
    }

    asio::awaitable<void> WindowAddUrlToProxyAsync(int identifier, std::string url)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddUrlToProxy, identifier, std::move(url));
    }

    asio::awaitable<void> WindowRemoveUrlToProxyAsync(int identifier, std::string url)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveUrlToProxy, identifier, std::move(url));
    }

    asio::awaitable<void> WindowAddDomainToProxyAsync(int identifier, std::string domain)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddDomainToProxy, identifier, std::move(domain));
    }

    asio::awaitable<void> WindowRemoveDomainToProxyAsync(int identifier, std::string domain)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveDomainToProxy, identifier, std::move(domain));
    }

    asio::awaitable<void> WindowAddUrlToModifyAsync(int identifier, std::string url)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddUrlToModify, identifier, std::move(url));
    }

    asio::awaitable<void> WindowRemoveUrlToModifyAsync(int identifier, std::string url)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveUrlToModify, identifier, std::move(url));
    }

    asio::awaitable<void> WindowAddDevToolsEventMethod(int identifier, std::string method)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddDevToolsEventMethod, identifier, std::move(method));
    }

    asio::awaitable<void> WindowRemoveDevToolsEventMethod(int identifier, std::string method)
    {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveDevToolsEventMethod, identifier, std::move(method));
    }

    void Dispose()
    {
        {
            std::unique_lock<std::mutex> lock(dispose_mutex_);
            dispose_condition_.wait(lock,
                                    [this]()
                                    {
                                        return !disposing_;
                                    });
            if (disposed_)
            {
                return;
            }
            disposing_ = true;
        }

        if (started_.load() && !child_exit_signal_.IsSignaled() && Send(detail::MakeNotification(static_cast<std::uint8_t>(detail::OpcodeControllerNotification::Exit), {})))
        {
            child_exit_signal_.WaitFor(start_options_.shutdown_grace_period);
        }

        Terminate();

        {
            std::lock_guard<std::mutex> lock(dispose_mutex_);
            disposing_ = false;
            disposed_ = true;
        }
        dispose_condition_.notify_all();
    }

private:
    struct InFlight
    {
        explicit InFlight(const asio::any_io_executor& executor) : strand(executor) {}

        asio::strand<asio::any_io_executor> strand;
        asio::cancellation_signal signal;
        std::atomic<bool> canceled = false;
    };

    struct WindowRecord
    {
        int identifier = 0;
        std::shared_ptr<JustCefWindow> window;
        std::shared_ptr<JustCefBrowser> browser;
        std::shared_ptr<WindowShared> shared;
    };

    void EnsureStarted() const
    {
        if (!started_.load())
        {
            throw std::runtime_error("Process has not been started.");
        }
    }

    bool Send(detail::OutgoingPacket packet) { return link_->Send(std::move(packet)); }

    asio::awaitable<void> NotifyAsync(detail::OpcodeControllerNotification opcode)
    {
        if (!Send(detail::MakeNotification(static_cast<std::uint8_t>(opcode), {})))
        {
            throw std::runtime_error("Process transport is shut down.");
        }
        co_return;
    }

    asio::awaitable<detail::PacketReader> AsyncRawCall(detail::OpcodeController opcode, detail::PacketWriter writer, bool timeout = true, detail::Rpc::ReplyHook hook = {})
    {
        EnsureStarted();
        auto self = shared_from_this();

        detail::Rpc::Timeout call_timeout;
        if (timeout && start_options_.default_call_timeout.count() > 0)
        {
            call_timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(start_options_.default_call_timeout);
        }
        co_return co_await self->rpc_->CallAsync(static_cast<std::uint8_t>(opcode), writer.Release(), call_timeout, std::move(hook));
    }

    asio::awaitable<void> AsyncVoidCall(detail::OpcodeController opcode, detail::PacketWriter writer) { (void)co_await AsyncRawCall(opcode, std::move(writer)); }

    template <typename T, typename Parser> asio::awaitable<T> AsyncParsedCall(detail::OpcodeController opcode, detail::PacketWriter writer, Parser parser)
    {
        return AsyncParsedCall<T>(opcode, std::move(writer), true, std::move(parser));
    }

    template <typename T, typename Parser> asio::awaitable<T> AsyncParsedCall(detail::OpcodeController opcode, detail::PacketWriter writer, bool timeout, Parser parser)
    {
        detail::PacketReader reader(co_await AsyncRawCall(opcode, std::move(writer), timeout));
        co_return parser(reader);
    }

    asio::awaitable<void> AsyncWindowIdentifierCall(detail::OpcodeController opcode, int identifier)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_await AsyncVoidCall(opcode, std::move(writer));
    }

    asio::awaitable<void> AsyncWindowStringCall(detail::OpcodeController opcode, int identifier, std::string value)
    {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(value);
        co_await AsyncVoidCall(opcode, std::move(writer));
    }

    void OnPacket(detail::IncomingPacket&& packet)
    {
        switch (packet.packet_type)
        {
        case detail::PacketType::Response:
            rpc_->OnResponse(packet.request_id, packet.opcode, std::move(packet.body));
            break;
        case detail::PacketType::Request:
        {
            if (packet.request_id == 0)
            {
                Logger::Error("JustCefProcess", "Received a request with request id 0. Ignored.");
                break;
            }

            auto entry = std::make_shared<InFlight>(executor_);
            bool duplicate = false;
            {
                std::lock_guard<std::mutex> lock(in_flight_mutex_);
                duplicate = !in_flight_.emplace(packet.request_id, entry).second;
            }

            if (duplicate)
            {
                Respond(packet.opcode, packet.request_id, HandlerReply{.status = Status::InvalidRequest, .message = "The request identifier is already in flight."}, {});
                break;
            }

            auto self = shared_from_this();
            asio::post(entry->strand,
                       [self, entry, opcode = static_cast<detail::OpcodeClient>(packet.opcode), request_id = packet.request_id, body = std::move(packet.body)]() mutable
                       {
                           asio::co_spawn(
                               entry->strand,
                               [self, entry, opcode, request_id, body = std::move(body)]() mutable
                               {
                                   return self->HandleIncomingRequest(opcode, request_id, std::move(body), entry);
                               },
                               asio::bind_cancellation_slot(entry->signal.slot(), asio::detached));
                       });
            break;
        }
        case detail::PacketType::Notification:
        {
            try
            {
                detail::PacketReader reader(std::move(packet.body));
                HandleNotification(static_cast<detail::OpcodeClientNotification>(packet.opcode), reader);
            }
            catch (...)
            {
                Logger::Error("JustCefProcess", "Exception occurred while processing IPC notification.", std::current_exception());
            }
            break;
        }
        case detail::PacketType::Cancel:
            HandleCancel(packet.request_id);
            break;
        }
    }

    std::optional<WindowRecord> GetWindowRecord(int identifier) const
    {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        const auto iterator = std::find_if(windows_.begin(), windows_.end(),
                                           [identifier](const WindowRecord& record)
                                           {
                                               return record.identifier == identifier;
                                           });
        if (iterator == windows_.end())
        {
            return std::nullopt;
        }
        return *iterator;
    }

    bool RegisterWindowRecord(WindowRecord record, int owner_identifier)
    {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        const bool owner_open = std::any_of(windows_.begin(), windows_.end(),
                                            [owner_identifier](const WindowRecord& existing)
                                            {
                                                return existing.identifier == owner_identifier;
                                            });
        const bool already_registered = std::any_of(windows_.begin(), windows_.end(),
                                                    [&](const WindowRecord& existing)
                                                    {
                                                        return existing.identifier == record.identifier;
                                                    });
        if (!owner_open || already_registered || closed_window_ids_.contains(record.identifier))
        {
            return false;
        }

        windows_.push_back(std::move(record));
        return true;
    }

    void MarkWindowClosed(int identifier)
    {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        if (!closed_window_ids_.insert(identifier).second)
        {
            return;
        }

        closed_window_order_.push(identifier);
        while (closed_window_order_.size() > kMaximumClosedWindowIds)
        {
            closed_window_ids_.erase(closed_window_order_.front());
            closed_window_order_.pop();
        }
    }

    std::optional<WindowRecord> RemoveWindowRecord(int identifier)
    {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        const auto iterator = std::find_if(windows_.begin(), windows_.end(),
                                           [identifier](const WindowRecord& record)
                                           {
                                               return record.identifier == identifier;
                                           });
        if (iterator == windows_.end())
        {
            return std::nullopt;
        }

        WindowRecord removed = std::move(*iterator);
        windows_.erase(iterator);
        return removed;
    }

    std::string DeserializeBridgeRpcPayload(detail::PacketReader& reader, std::string_view description)
    {
        const auto payload_length = ReadRequired<std::uint32_t>(reader, "payloadLength");
        const auto payload = reader.ReadString(static_cast<std::size_t>(payload_length));
        if (!payload)
        {
            throw detail::ProtocolError(std::string("Failed to parse inline payload for ") + std::string(description) + ".");
        }
        return *payload;
    }

    std::vector<std::uint8_t> DeserializeBinaryPayload(detail::PacketReader& reader, std::string_view description)
    {
        const auto payload_length = ReadRequired<std::uint32_t>(reader, "payloadLength");
        if (!reader.HasAvailable(payload_length))
        {
            throw detail::ProtocolError(std::string("Failed to parse inline payload for ") + std::string(description) + ".");
        }
        return reader.ReadBytes(static_cast<std::size_t>(payload_length));
    }

    std::shared_ptr<DataStream> OpenStream(std::shared_ptr<ByteStream> source, std::optional<std::uint64_t> content_length)
    {
        std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
        std::uint32_t stream_identifier = 0;
        do
        {
            stream_identifier = ++stream_identifier_counter_;
        } while (stream_identifier == 0 || outgoing_streams_.contains(stream_identifier));

        auto stream = std::make_shared<DataStream>(stream_identifier, std::move(source), content_length,
                                                   [link = link_](detail::OutgoingPacket packet)
                                                   {
                                                       return link->Send(std::move(packet));
                                                   });
        outgoing_streams_[stream_identifier] = stream;
        return stream;
    }

    std::shared_ptr<DataStream> FindStream(std::uint32_t identifier)
    {
        std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
        const auto iterator = outgoing_streams_.find(identifier);
        return iterator == outgoing_streams_.end() ? nullptr : iterator->second;
    }

    void RemoveStream(std::uint32_t identifier)
    {
        std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
        outgoing_streams_.erase(identifier);
    }

    void SerializeModifyRequest(detail::PacketWriter& writer, const IPCRequest& request)
    {
        writer.WriteSizePrefixedString(request.method);
        writer.WriteSizePrefixedString(request.url);
        writer.Write<std::int32_t>(static_cast<std::int32_t>(CountHeaderValuePairs(request.headers)));
        for (const auto& [key, values] : request.headers)
        {
            for (const auto& value : values)
            {
                writer.WriteSizePrefixedString(key);
                writer.WriteSizePrefixedString(value);
            }
        }

        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(request.elements.size()));
        for (const auto& element : request.elements)
        {
            writer.Write<std::uint8_t>(static_cast<std::uint8_t>(element.type));
            if (element.type == IPCProxyBodyElementType::Bytes)
            {
                writer.Write<std::uint32_t>(static_cast<std::uint32_t>(element.data.size()));
                writer.WriteBytes(element.data);
            }
            else if (element.type == IPCProxyBodyElementType::File)
            {
                writer.WriteSizePrefixedString(element.file_name);
            }
        }
    }

    ParsedWindowRequest ReadWindowRequest(detail::PacketReader& reader)
    {
        ParsedWindowRequest parsed;
        parsed.identifier = ReadRequired<std::int32_t>(reader, "identifier");
        parsed.request.method = ReadRequiredString(reader, "method");
        parsed.request.url = ReadRequiredString(reader, "url");

        const auto header_count = ReadRequired<std::int32_t>(reader, "headerCount");
        if (header_count < 0)
        {
            throw detail::ProtocolError("Header count cannot be negative.");
        }

        for (int index = 0; index < header_count; ++index)
        {
            const auto key = ReadRequiredString(reader, "headerKey");
            const auto value = ReadRequiredString(reader, "headerValue");
            parsed.request.headers[key].push_back(value);
        }

        const auto element_count = ReadRequired<std::uint32_t>(reader, "elementCount");
        parsed.request.elements.reserve(std::min<std::size_t>(element_count, reader.RemainingSize()));
        for (std::uint32_t index = 0; index < element_count; ++index)
        {
            const auto element_type = static_cast<IPCProxyBodyElementType>(ReadRequired<std::uint8_t>(reader, "elementType"));
            switch (element_type)
            {
            case IPCProxyBodyElementType::Bytes:
            {
                const auto size = ReadRequired<std::uint32_t>(reader, "elementSize");
                parsed.request.elements.push_back(IPCProxyBodyElement::Bytes(reader.ReadBytes(size)));
                break;
            }
            case IPCProxyBodyElementType::File:
            {
                parsed.request.elements.push_back(IPCProxyBodyElement::File(ReadRequiredString(reader, "fileName")));
                break;
            }
            case IPCProxyBodyElementType::Empty:
            default:
                parsed.request.elements.push_back({});
                break;
            }
        }

        return parsed;
    }

    asio::awaitable<void> HandleWindowBridgeRpc(detail::PacketReader& reader, detail::PacketWriter& writer, HandlerReply& reply)
    {
        const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
        const auto record = GetWindowRecord(identifier);
        if (!record || !record->window || !record->shared)
        {
            reply.status = Status::NotFound;
            co_return;
        }

        const auto method = reader.ReadSizePrefixedString();
        if (!method || method->empty())
        {
            reply.status = Status::Error;
            reply.message = "Bridge RPC method must be a non-empty string.";
            co_return;
        }

        const auto payload_json = DeserializeBridgeRpcPayload(reader, "bridge RPC request payload");

        BridgeRpcHandler handler;
        {
            std::lock_guard<std::mutex> lock(record->shared->request_mutex);
            handler = record->shared->bridge_rpc_handler;
        }

        if (!handler)
        {
            reply.status = Status::Error;
            reply.message = "No bridge RPC handler is registered for this window.";
            co_return;
        }

        const auto result_json = co_await handler(*record->window, *method, payload_json);
        const std::string payload = result_json.value_or("null");
        WriteInlinePayload(writer, payload);
    }

    asio::awaitable<void> HandleWindowViewCreated(detail::PacketReader& reader, detail::PacketWriter& writer, HandlerReply& reply)
    {
        const int parent_identifier = ReadRequired<std::int32_t>(reader, "parentIdentifier");
        const int view_identifier = ReadRequired<std::int32_t>(reader, "viewIdentifier");
        const auto src = reader.ReadSizePrefixedString();

        const auto parent = GetWindow(parent_identifier);
        if (!parent)
        {
            Logger::Info("JustCefProcess", "View " + std::to_string(view_identifier) + " was created for unknown parent window " + std::to_string(parent_identifier) + ".");
            reply.status = Status::NotFound;
            co_return;
        }

        auto shared = std::make_shared<WindowShared>();
        shared->executor = executor_;

        auto view = std::shared_ptr<JustCefView>(new JustCefView(view_identifier, shared_from_this(), shared, parent));
        if (!RegisterWindowRecord(WindowRecord{.identifier = view_identifier, .browser = view, .shared = shared}, parent_identifier))
        {
            writer.Write<bool>(false);
            co_return;
        }

        ViewCreatedHandler handler;
        {
            std::lock_guard<std::mutex> lock(parent->shared_->request_mutex);
            handler = parent->shared_->view_created_handler;
        }

        try
        {
            if (handler)
            {
                co_await handler(*view);
            }
            writer.Write<bool>(true);
        }
        catch (...)
        {
            Logger::Error("JustCefProcess", "Exception occurred while configuring view " + std::to_string(view_identifier) + " (" + src.value_or("") + ").",
                          std::current_exception());
            SignalWindowClosed(RemoveWindowRecord(view_identifier));
            writer.Write<bool>(false);
        }
    }

    asio::awaitable<void> HandleIncomingRequest(detail::OpcodeClient opcode, std::uint32_t request_id, std::vector<std::uint8_t> body, std::shared_ptr<InFlight> entry)
    {
        detail::PacketWriter writer(SIZE_MAX);
        HandlerReply reply;
        try
        {
            detail::PacketReader reader(std::move(body));

            switch (opcode)
            {
            case detail::OpcodeClient::Ping:
                break;
            case detail::OpcodeClient::Print:
                if (const auto text = reader.ReadString(reader.RemainingSize()))
                {
                    Logger::Info("JustCefProcess", *text);
                }
                break;
            case detail::OpcodeClient::Echo:
                writer.WriteBytes(reader.ReadBytes(reader.RemainingSize()));
                break;
            case detail::OpcodeClient::WindowProxyRequest:
                co_await HandleWindowProxyRequest(reader, writer, reply);
                break;
            case detail::OpcodeClient::WindowModifyRequest:
                co_await HandleWindowModifyRequest(reader, writer, reply);
                break;
            case detail::OpcodeClient::WindowBridgeRpc:
                co_await HandleWindowBridgeRpc(reader, writer, reply);
                break;
            case detail::OpcodeClient::WindowViewCreated:
                co_await HandleWindowViewCreated(reader, writer, reply);
                break;
            default:
                Logger::Warning("JustCefProcess", "Received an unhandled client opcode.");
                reply.status = Status::Unsupported;
                break;
            }
        }
        catch (const detail::ProtocolError& exception)
        {
            reply.status = Status::InvalidRequest;
            reply.message = exception.what();
        }
        catch (const std::exception& exception)
        {
            if (!entry->canceled)
            {
                Logger::Error("JustCefProcess", "Exception occurred while processing IPC request.", std::current_exception());
            }
            reply.status = entry->canceled ? Status::Canceled : Status::Error;
            reply.message = exception.what();
        }
        catch (...)
        {
            Logger::Error("JustCefProcess", "Exception occurred while processing IPC request.", std::current_exception());
            reply.status = entry->canceled ? Status::Canceled : Status::Error;
        }

        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            const auto iterator = in_flight_.find(request_id);
            if (iterator != in_flight_.end() && iterator->second == entry)
            {
                in_flight_.erase(iterator);
            }
        }

        Respond(static_cast<std::uint8_t>(opcode), request_id, std::move(reply), writer.Release());
    }

    void Respond(std::uint8_t opcode, std::uint32_t request_id, HandlerReply reply, std::vector<std::uint8_t> payload)
    {
        bool sent = false;
        if (reply.status == Status::Ok)
        {
            try
            {
                sent = Send(detail::MakeOkResponse(opcode, request_id, std::move(payload)));
            }
            catch (const detail::ProtocolError&)
            {
                reply.status = Status::TooLarge;
                sent = Send(detail::MakeStatusResponse(opcode, request_id, Status::TooLarge, std::string("The response exceeds the maximum IPC packet size.")));
            }
        }
        else
        {
            sent = Send(detail::MakeStatusResponse(opcode, request_id, reply.status, reply.message));
        }

        if (!reply.stream)
        {
            return;
        }

        if (!sent || reply.status != Status::Ok)
        {
            RemoveStream(reply.stream->GetIdentifier());
            reply.stream->CloseSource();
            return;
        }

        std::weak_ptr<JustCefProcessImpl> weak_self = weak_from_this();
        reply.stream->Start(asio::make_strand(executor_),
                            [weak_self](std::uint32_t stream_identifier)
                            {
                                if (auto self = weak_self.lock())
                                {
                                    self->RemoveStream(stream_identifier);
                                }
                            });
    }

    void HandleCancel(std::uint32_t request_id)
    {
        std::shared_ptr<InFlight> entry;
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            const auto iterator = in_flight_.find(request_id);
            if (iterator == in_flight_.end())
            {
                return;
            }
            entry = iterator->second;
        }
        CancelHandler(entry);
    }

    static void CancelHandler(const std::shared_ptr<InFlight>& entry)
    {
        entry->canceled = true;
        asio::post(entry->strand,
                   [entry]()
                   {
                       entry->signal.emit(asio::cancellation_type::terminal);
                   });
    }

    asio::awaitable<void> HandleWindowProxyRequest(detail::PacketReader& reader, detail::PacketWriter& writer, HandlerReply& reply)
    {
        const ParsedWindowRequest parsed = ReadWindowRequest(reader);
        const auto record = GetWindowRecord(parsed.identifier);
        if (!record || !record->browser || !record->shared)
        {
            reply.status = Status::NotFound;
            co_return;
        }

        BoundRequestProxy request_proxy;
        {
            std::lock_guard<std::mutex> lock(record->shared->request_mutex);
            request_proxy = record->shared->request_proxy;
        }

        if (!request_proxy)
        {
            reply.status = Status::NotHandled;
            co_return;
        }

        std::optional<IPCResponse> response = co_await request_proxy(parsed.request);
        if (!response)
        {
            reply.status = Status::NotHandled;
            co_return;
        }

        const HeaderMap filtered_headers = FilterResponseHeaders(response->headers);

        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(response->status_code));
        writer.WriteSizePrefixedString(response->status_text);
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(CountHeaderValuePairs(filtered_headers)));
        for (const auto& [key, values] : filtered_headers)
        {
            for (const auto& value : values)
            {
                writer.WriteSizePrefixedString(key);
                writer.WriteSizePrefixedString(value);
            }
        }

        if (!response->body_stream)
        {
            writer.Write<std::uint8_t>(0);
            co_return;
        }

        const auto content_length = ParseContentLength(filtered_headers);
        if (content_length && *content_length <= kInlineProxyBodyLimit)
        {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(*content_length));
            std::size_t total = 0;
            try
            {
                while (total < buffer.size())
                {
                    const std::size_t read = co_await response->body_stream->ReadAsync(buffer.data() + total, buffer.size() - total);
                    if (read == 0)
                    {
                        break;
                    }
                    total += read;
                }
            }
            catch (...)
            {
                response->body_stream->Close();
                throw;
            }
            response->body_stream->Close();

            writer.Write<std::uint8_t>(1);
            writer.Write<std::uint32_t>(static_cast<std::uint32_t>(total));
            writer.WriteBytes(buffer.data(), total);
            co_return;
        }

        writer.Write<std::uint8_t>(2);
        writer.Write<std::int64_t>(content_length ? static_cast<std::int64_t>(*content_length) : static_cast<std::int64_t>(-1));
        reply.stream = OpenStream(response->body_stream, content_length);
        writer.Write<std::uint32_t>(reply.stream->GetIdentifier());
    }

    asio::awaitable<void> HandleWindowModifyRequest(detail::PacketReader& reader, detail::PacketWriter& writer, HandlerReply& reply)
    {
        const ParsedWindowRequest parsed = ReadWindowRequest(reader);
        const auto record = GetWindowRecord(parsed.identifier);
        if (!record || !record->browser || !record->shared)
        {
            reply.status = Status::NotFound;
            co_return;
        }

        BoundRequestModifier request_modifier;
        {
            std::lock_guard<std::mutex> lock(record->shared->request_mutex);
            request_modifier = record->shared->request_modifier;
        }

        if (!request_modifier)
        {
            reply.status = Status::NotHandled;
            co_return;
        }

        const std::optional<IPCRequest> modified_request = co_await request_modifier(parsed.request);
        if (!modified_request)
        {
            reply.status = Status::NotHandled;
            co_return;
        }

        SerializeModifyRequest(writer, *modified_request);
        co_return;
    }

    void HandleNotification(detail::OpcodeClientNotification opcode, detail::PacketReader& reader)
    {
        switch (opcode)
        {
        case detail::OpcodeClientNotification::Exit:
            Logger::Info("JustCefProcess", "CEF process is exiting.");
            break;
        case detail::OpcodeClientNotification::Ready:
        {
            const auto version = ReadRequired<std::uint32_t>(reader, "protocolVersion");
            if (version != detail::kProtocolVersion)
            {
                const std::string message = "justcefnative speaks IPC protocol version " + std::to_string(version) + ", expected " + std::to_string(detail::kProtocolVersion) + ".";
                Logger::Error("JustCefProcess", message);
                ready_signal_.SignalFailure(std::make_exception_ptr(std::runtime_error(message)));
                break;
            }
            Logger::Info("JustCefProcess", "Client is ready.");
            ready_signal_.SignalSuccess();
            break;
        }
        case detail::OpcodeClientNotification::WindowOpened:
            Logger::Info("JustCefProcess", "Window opened: " + std::to_string(ReadRequired<std::int32_t>(reader, "identifier")));
            break;
        case detail::OpcodeClientNotification::WindowClosed:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            MarkWindowClosed(identifier);
            SignalWindowClosed(RemoveWindowRecord(identifier));
            break;
        }
        case detail::OpcodeClientNotification::WindowFocused:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnFocused);
            }
            break;
        }
        case detail::OpcodeClientNotification::WindowUnfocused:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnUnfocused);
            }
            break;
        }
        case detail::OpcodeClientNotification::WindowFullscreenChanged:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            const bool fullscreen = ReadRequired<bool>(reader, "fullscreen");
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnFullscreenChanged, fullscreen);
            }
            break;
        }
        case detail::OpcodeClientNotification::WindowFrameLoadStart:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            const auto frame_identifier = reader.ReadSizePrefixedString();
            const bool is_main_frame = ReadRequired<bool>(reader, "isMainFrame");
            const auto url = reader.ReadSizePrefixedString();
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnFrameLoadStart,
                          FrameLoadStartInfo{
                              .frame_identifier = frame_identifier,
                              .is_main_frame = is_main_frame,
                              .url = url,
                          });
            }
            break;
        }
        case detail::OpcodeClientNotification::WindowFrameLoadEnd:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            const auto frame_identifier = reader.ReadSizePrefixedString();
            const bool is_main_frame = ReadRequired<bool>(reader, "isMainFrame");
            const auto url = reader.ReadSizePrefixedString();
            const int http_status_code = ReadRequired<std::int32_t>(reader, "httpStatusCode");
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnFrameLoadEnd,
                          FrameLoadEndInfo{
                              .frame_identifier = frame_identifier,
                              .is_main_frame = is_main_frame,
                              .url = url,
                              .http_status_code = http_status_code,
                          });
            }
            break;
        }
        case detail::OpcodeClientNotification::WindowFrameLoadError:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            const auto frame_identifier = reader.ReadSizePrefixedString();
            const bool is_main_frame = ReadRequired<bool>(reader, "isMainFrame");
            const int error_code = ReadRequired<std::int32_t>(reader, "errorCode");
            const auto error_text = reader.ReadSizePrefixedString();
            const auto failed_url = reader.ReadSizePrefixedString();
            const auto record = GetWindowRecord(identifier);
            if (record && record->shared && is_main_frame)
            {
                record->shared->loading.OnMainFrameLoadError(error_code, error_text.value_or(std::string()));
            }
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnFrameLoadError,
                          FrameLoadErrorInfo{
                              .frame_identifier = frame_identifier,
                              .is_main_frame = is_main_frame,
                              .error_code = error_code,
                              .error_text = error_text,
                              .failed_url = failed_url,
                          });
            }
            break;
        }
        case detail::OpcodeClientNotification::WindowLoadingStateChanged:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            const bool is_loading = ReadRequired<bool>(reader, "isLoading");
            const bool can_go_back = ReadRequired<bool>(reader, "canGoBack");
            const bool can_go_forward = ReadRequired<bool>(reader, "canGoForward");
            const auto record = GetWindowRecord(identifier);
            if (record && record->shared)
            {
                record->shared->loading.Apply(is_loading, can_go_back, can_go_forward);
            }
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnLoadingStateChanged,
                          LoadingStateChangedInfo{
                              .is_loading = is_loading,
                              .can_go_back = can_go_back,
                              .can_go_forward = can_go_forward,
                          });
            }
            break;
        }
        case detail::OpcodeClientNotification::WindowDevToolsEvent:
        {
            const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
            const auto method = reader.ReadSizePrefixedString();
            auto payload = DeserializeBinaryPayload(reader, "DevTools event payload");
            if (auto window = GetBrowser(identifier))
            {
                PostEvent(window, &JustCefBrowser::OnDevToolsEvent, method, std::move(payload));
            }
            break;
        }
        case detail::OpcodeClientNotification::StreamCredit:
        {
            const auto stream_identifier = ReadRequired<std::uint32_t>(reader, "streamIdentifier");
            const auto bytes = ReadRequired<std::uint32_t>(reader, "bytes");
            if (auto stream = FindStream(stream_identifier))
            {
                stream->AddCredit(bytes);
            }
            break;
        }
        case detail::OpcodeClientNotification::StreamCancel:
        {
            if (auto stream = FindStream(ReadRequired<std::uint32_t>(reader, "streamIdentifier")))
            {
                stream->Cancel();
            }
            break;
        }
        default:
            Logger::Info("JustCefProcess", "Received unhandled notification opcode.");
            break;
        }
    }

    template <typename... Args> void PostEvent(std::shared_ptr<JustCefBrowser> browser, Event<Args...> JustCefBrowser::* event, std::type_identity_t<Args>... args)
    {
        asio::post(event_strand_,
                   [browser = std::move(browser), event, args...]()
                   {
                       try
                       {
                           ((*browser).*event).Emit(args...);
                       }
                       catch (...)
                       {
                           Logger::Error("JustCefProcess", "Exception occurred while processing IPC notification.", std::current_exception());
                       }
                   });
    }

    void SignalWindowClosed(const std::optional<WindowRecord>& record)
    {
        if (!record || !record->shared)
        {
            return;
        }

        bool expected = false;
        if (!record->shared->close_signaled.compare_exchange_strong(expected, true))
        {
            return;
        }

        record->shared->loading.Close();
        record->shared->close_signal.SignalSuccess();
        if (record->browser)
        {
            PostEvent(record->browser, &JustCefBrowser::OnClose);
        }
    }

    void WaitForChild()
    {
        std::optional<int> exit_code;
#ifdef _WIN32
        const DWORD wait_result = WaitForSingleObject(process_handle_, INFINITE);
        {
            std::lock_guard<std::mutex> lock(child_mutex_);
            DWORD code = 0;
            if (GetExitCodeProcess(process_handle_, &code))
            {
                exit_code = static_cast<int>(code);
            }
            child_exited_ = wait_result == WAIT_OBJECT_0;
        }
#else
        siginfo_t info{};
        while (::waitid(P_PID, static_cast<id_t>(child_pid_), &info, WEXITED | WNOWAIT) != 0 && errno == EINTR)
        {
        }

        {
            std::lock_guard<std::mutex> lock(child_mutex_);
            int status = 0;
            pid_t reaped = -1;
            do
            {
                reaped = ::waitpid(child_pid_, &status, 0);
            } while (reaped < 0 && errno == EINTR);

            if (reaped == child_pid_ && WIFEXITED(status))
            {
                exit_code = WEXITSTATUS(status);
            }
            else if (reaped == child_pid_ && WIFSIGNALED(status))
            {
                exit_code = 128 + WTERMSIG(status);
            }
            child_exited_ = reaped == child_pid_;
        }
#endif

        Logger::Info("JustCefProcess", "justcefnative exited" + (exit_code ? " with code " + std::to_string(*exit_code) : std::string()) + ".");
        if (auto transport = link_->Get())
        {
            transport->Close(detail::CloseMode::Drain);
        }
        child_exit_signal_.SignalSuccess();
    }

    void KillChild()
    {
        std::lock_guard<std::mutex> lock(child_mutex_);
#ifdef _WIN32
        if (!child_exited_ && process_handle_ != nullptr)
        {
            TerminateProcess(process_handle_, 1);
        }
#else
        if (!child_exited_ && child_pid_ > 0)
        {
            ::kill(child_pid_, SIGKILL);
        }
#endif
    }

    void Terminate()
    {
        KillChild();
        if (auto transport = link_->Get())
        {
            transport->Close(detail::CloseMode::Immediate);
            transport->Join();
        }

        Shutdown();

        if (waiter_thread_.joinable())
        {
            if (waiter_thread_.get_id() == std::this_thread::get_id())
            {
                waiter_thread_.detach();
            }
            else
            {
                waiter_thread_.join();
            }
        }

        CloseTransportHandles();
    }

    void CloseTransportHandles()
    {
#ifdef _WIN32
        if (read_handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(read_handle_);
            read_handle_ = INVALID_HANDLE_VALUE;
        }
        if (write_handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(write_handle_);
            write_handle_ = INVALID_HANDLE_VALUE;
        }
        if (process_handle_ != nullptr)
        {
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
        }
#else
        if (read_handle_ != -1)
        {
            ::close(read_handle_);
            read_handle_ = -1;
        }
        if (write_handle_ != -1)
        {
            ::close(write_handle_);
            write_handle_ = -1;
        }
#endif
    }

    void Shutdown()
    {
        bool expected = false;
        if (!shutdown_.compare_exchange_strong(expected, true))
        {
            return;
        }

        if (!ready_signal_.IsSignaled())
        {
            ready_signal_.SignalFailure(std::make_exception_ptr(std::runtime_error("Process disposed before ready.")));
        }

        const auto shutdown_exception = std::make_exception_ptr(std::runtime_error("Process disposed while awaiting IPC response."));
        rpc_->Close(shutdown_exception);

        std::unordered_map<std::uint32_t, std::shared_ptr<InFlight>> in_flight;
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            in_flight.swap(in_flight_);
        }
        for (auto& [_, entry] : in_flight)
        {
            CancelHandler(entry);
        }

        {
            std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
            for (auto& [_, stream] : outgoing_streams_)
            {
                stream->Cancel();
            }
            outgoing_streams_.clear();
        }

        std::vector<WindowRecord> windows_to_close;
        {
            std::lock_guard<std::mutex> lock(windows_mutex_);
            windows_to_close.swap(windows_);
        }

        for (const auto& record : windows_to_close)
        {
            SignalWindowClosed(record);
        }

        exit_signal_.SignalSuccess();
    }

    asio::any_io_executor executor_;
    detail::AsyncSignal ready_signal_;
    detail::AsyncSignal exit_signal_;
    detail::AsyncSignal child_exit_signal_;
    std::atomic<bool> started_ = false;
    std::atomic<bool> shutdown_ = false;
    StartOptions start_options_;
    std::atomic<std::uint32_t> stream_identifier_counter_ = 0;
    mutable std::mutex windows_mutex_;
    std::vector<WindowRecord> windows_;
    static constexpr std::size_t kMaximumClosedWindowIds = 256;
    std::unordered_set<int> closed_window_ids_;
    std::queue<int> closed_window_order_;
    std::shared_ptr<TransportLink> link_ = std::make_shared<TransportLink>();
    std::shared_ptr<detail::Rpc> rpc_;
    std::mutex in_flight_mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<InFlight>> in_flight_;
    std::mutex outgoing_streams_mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<DataStream>> outgoing_streams_;
    asio::strand<asio::any_io_executor> event_strand_;
    std::mutex dispose_mutex_;
    std::condition_variable dispose_condition_;
    bool disposing_ = false;
    bool disposed_ = false;
    std::mutex child_mutex_;
    bool child_exited_ = false;
    std::thread waiter_thread_;

#ifdef _WIN32
    HANDLE read_handle_ = INVALID_HANDLE_VALUE;
    HANDLE write_handle_ = INVALID_HANDLE_VALUE;
    HANDLE process_handle_ = nullptr;
#else
    int read_handle_ = -1;
    int write_handle_ = -1;
    pid_t child_pid_ = -1;
#endif
};

JustCefProcess::JustCefProcess() : impl_(std::make_shared<JustCefProcessImpl>(asio::system_executor()))
{
}

JustCefProcess::JustCefProcess(asio::any_io_executor executor) : impl_(std::make_shared<JustCefProcessImpl>(std::move(executor)))
{
}

JustCefProcess::~JustCefProcess()
{
    impl_->Dispose();
}

void JustCefProcess::Start(const std::string& args)
{
    Start(StartOptions{.arguments = args});
}

void JustCefProcess::Start(const StartOptions& options)
{
    impl_->Start(options);
}

bool JustCefProcess::HasExited() const
{
    return impl_->HasExited();
}

std::vector<std::shared_ptr<JustCefWindow>> JustCefProcess::Windows() const
{
    return impl_->Windows();
}

std::shared_ptr<JustCefWindow> JustCefProcess::GetWindow(int identifier) const
{
    return impl_->GetWindow(identifier);
}

void JustCefProcess::WaitForExit() const
{
    impl_->WaitForExit();
}

asio::awaitable<void> JustCefProcess::WaitForExitAsync() const
{
    return impl_->WaitForExitAsync();
}

void JustCefProcess::WaitForReady() const
{
    impl_->WaitForReady();
}

asio::awaitable<void> JustCefProcess::WaitForReadyAsync() const
{
    return impl_->WaitForReadyAsync();
}

asio::awaitable<void> JustCefProcess::EchoAsync(std::vector<std::uint8_t> data)
{
    return impl_->EchoAsync(std::move(data));
}

asio::awaitable<void> JustCefProcess::PingAsync()
{
    return impl_->PingAsync();
}

asio::awaitable<void> JustCefProcess::PrintAsync(std::string message)
{
    return impl_->PrintAsync(std::move(message));
}

asio::awaitable<std::shared_ptr<JustCefWindow>> JustCefProcess::CreateWindowAsync(const WindowCreateOptions& options)
{
    return impl_->CreateWindowAsync(options);
}

asio::awaitable<std::shared_ptr<JustCefWindow>> JustCefProcess::CreateWindowAsync(std::string url, int minimum_width, int minimum_height, int preferred_width, int preferred_height,
                                                                                  bool fullscreen, bool context_menu_enable, bool shown, bool developer_tools_enabled,
                                                                                  bool resizable, bool frameless, bool centered, bool proxy_requests, bool log_console,
                                                                                  RequestProxy request_proxy, bool modify_requests, RequestModifier request_modifier,
                                                                                  bool modify_request_body, std::optional<std::string> title, std::optional<std::string> icon_path,
                                                                                  std::optional<std::string> app_id, bool bridge_enabled, BridgeRpcHandler bridge_rpc_handler,
                                                                                  bool views_enabled, ViewCreatedHandler view_created_handler)
{
    return CreateWindowAsync(WindowCreateOptions{
        .url = std::move(url),
        .minimum_width = minimum_width,
        .minimum_height = minimum_height,
        .preferred_width = preferred_width,
        .preferred_height = preferred_height,
        .fullscreen = fullscreen,
        .context_menu_enable = context_menu_enable,
        .shown = shown,
        .developer_tools_enabled = developer_tools_enabled,
        .resizable = resizable,
        .frameless = frameless,
        .centered = centered,
        .proxy_requests = proxy_requests,
        .log_console = log_console,
        .request_proxy = std::move(request_proxy),
        .modify_requests = modify_requests,
        .request_modifier = std::move(request_modifier),
        .modify_request_body = modify_request_body,
        .title = std::move(title),
        .icon_path = std::move(icon_path),
        .app_id = std::move(app_id),
        .bridge_enabled = bridge_enabled,
        .bridge_rpc_handler = std::move(bridge_rpc_handler),
        .views_enabled = views_enabled,
        .view_created_handler = std::move(view_created_handler),
    });
}

asio::awaitable<void> JustCefProcess::NotifyExitAsync()
{
    return impl_->NotifyExitAsync();
}

asio::awaitable<std::vector<std::string>> JustCefProcess::PickFileAsync(bool multiple, std::vector<FileFilter> filters)
{
    return impl_->PickFileAsync(multiple, std::move(filters));
}

asio::awaitable<std::string> JustCefProcess::PickDirectoryAsync()
{
    return impl_->PickDirectoryAsync();
}

asio::awaitable<std::string> JustCefProcess::SaveFileAsync(std::string default_name, std::vector<FileFilter> filters)
{
    return impl_->SaveFileAsync(std::move(default_name), std::move(filters));
}

void JustCefProcess::Dispose()
{
    impl_->Dispose();
}

std::vector<std::filesystem::path> JustCefProcess::GenerateSearchPaths()
{
    return BuildSearchPaths();
}

std::filesystem::path JustCefProcess::ResolveNativeExecutablePath(const std::optional<std::filesystem::path>& native_executable_path)
{
    if (native_executable_path)
    {
        const auto resolved = std::filesystem::absolute(*native_executable_path);
        if (!std::filesystem::exists(resolved))
        {
            throw std::runtime_error("Failed to find justcefnative at '" + resolved.string() + "'.");
        }
        return resolved;
    }

    for (const auto& candidate : GenerateSearchPaths())
    {
        if (std::filesystem::exists(candidate))
        {
            return std::filesystem::absolute(candidate);
        }
    }

    throw std::runtime_error("Failed to find justcefnative.");
}

} // namespace justcef
