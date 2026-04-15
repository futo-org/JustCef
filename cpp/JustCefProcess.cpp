#include "JustCefProcess.h"

#include "AsyncSignal.h"
#include "Packet.h"
#include "WindowInternals.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace justcef {
namespace {

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

std::optional<std::string> TryGetFirstHeaderValue(const HeaderMap& headers, std::string_view key) {
    for (const auto& [header_key, values] : headers) {
        if (!EqualsIgnoreCase(header_key, key) || values.empty()) {
            continue;
        }

        return values.front();
    }

    return std::nullopt;
}

HeaderMap FilterResponseHeaders(const HeaderMap& headers) {
    HeaderMap filtered;
    for (const auto& [key, values] : headers) {
        if (EqualsIgnoreCase(key, "transfer-encoding")) {
            bool has_chunked = false;
            for (const auto& value : values) {
                if (EqualsIgnoreCase(value, "chunked")) {
                    has_chunked = true;
                    break;
                }
            }

            if (has_chunked) {
                continue;
            }
        }

        filtered.insert({key, values});
    }

    return filtered;
}

std::size_t CountHeaderValuePairs(const HeaderMap& headers) {
    std::size_t count = 0;
    for (const auto& [_, values] : headers) {
        count += values.size();
    }
    return count;
}

std::optional<std::uint64_t> ParseContentLength(const HeaderMap& headers) {
    const auto value = TryGetFirstHeaderValue(headers, "content-length");
    if (!value) {
        return std::nullopt;
    }

    try {
        return static_cast<std::uint64_t>(std::stoull(*value));
    } catch (...) {
        return std::nullopt;
    }
}

template <typename T>
T ReadRequired(detail::PacketReader& reader, const char* field_name) {
    const auto value = reader.Read<T>();
    if (!value) {
        throw std::runtime_error(std::string("Missing field: ") + field_name);
    }
    return *value;
}

std::string ReadRequiredString(detail::PacketReader& reader, const char* field_name) {
    const auto value = reader.ReadSizePrefixedString();
    if (!value) {
        throw std::runtime_error(std::string("Missing field: ") + field_name);
    }
    return *value;
}

struct ParsedWindowRequest {
    int identifier = 0;
    IPCRequest request;
};

ParsedWindowRequest ReadWindowRequest(detail::PacketReader& reader) {
    ParsedWindowRequest parsed;
    parsed.identifier = ReadRequired<std::int32_t>(reader, "identifier");
    parsed.request.method = ReadRequiredString(reader, "method");
    parsed.request.url = ReadRequiredString(reader, "url");

    const auto header_count = ReadRequired<std::int32_t>(reader, "headerCount");
    if (header_count < 0) {
        throw std::runtime_error("Header count cannot be negative.");
    }

    for (int index = 0; index < header_count; ++index) {
        const auto key = ReadRequiredString(reader, "headerKey");
        const auto value = ReadRequiredString(reader, "headerValue");
        parsed.request.headers[key].push_back(value);
    }

    const auto element_count = ReadRequired<std::uint32_t>(reader, "elementCount");
    parsed.request.elements.reserve(element_count);
    for (std::uint32_t index = 0; index < element_count; ++index) {
        const auto element_type = static_cast<IPCProxyBodyElementType>(ReadRequired<std::uint8_t>(reader, "elementType"));
        switch (element_type) {
            case IPCProxyBodyElementType::Bytes: {
                const auto size = ReadRequired<std::uint32_t>(reader, "elementSize");
                parsed.request.elements.push_back(IPCProxyBodyElement::Bytes(reader.ReadBytes(size)));
                break;
            }
            case IPCProxyBodyElementType::File: {
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

void SerializeModifyRequest(detail::PacketWriter& writer, const IPCRequest& request) {
    writer.WriteSizePrefixedString(request.method);
    writer.WriteSizePrefixedString(request.url);
    writer.Write<std::int32_t>(static_cast<std::int32_t>(CountHeaderValuePairs(request.headers)));
    for (const auto& [key, values] : request.headers) {
        for (const auto& value : values) {
            writer.WriteSizePrefixedString(key);
            writer.WriteSizePrefixedString(value);
        }
    }

    writer.Write<std::uint32_t>(static_cast<std::uint32_t>(request.elements.size()));
    for (const auto& element : request.elements) {
        writer.Write<std::uint8_t>(static_cast<std::uint8_t>(element.type));
        if (element.type == IPCProxyBodyElementType::Bytes) {
            writer.Write<std::uint32_t>(static_cast<std::uint32_t>(element.data.size()));
            writer.WriteBytes(element.data);
        } else if (element.type == IPCProxyBodyElementType::File) {
            writer.WriteSizePrefixedString(element.file_name);
        }
    }
}

std::vector<std::string> SplitArgumentsPosix(const std::string& arguments) {
    std::vector<std::string> parts;
    std::string current;
    bool in_single_quotes = false;
    bool in_double_quotes = false;
    bool escaped = false;

    for (const char character : arguments) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
            continue;
        }

        if (character == '\\' && !in_single_quotes) {
            escaped = true;
            continue;
        }

        if (character == '"' && !in_single_quotes) {
            in_double_quotes = !in_double_quotes;
            continue;
        }

        if (character == '\'' && !in_double_quotes) {
            in_single_quotes = !in_single_quotes;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(character)) && !in_single_quotes && !in_double_quotes) {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(character);
    }

    if (escaped || in_single_quotes || in_double_quotes) {
        throw std::runtime_error("Failed to parse command line arguments.");
    }

    if (!current.empty()) {
        parts.push_back(std::move(current));
    }

    return parts;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
    }

    std::wstring converted(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), converted.data(), size);
    return converted;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    const bool needs_quotes = argument.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes) {
        return argument;
    }

    std::wstring quoted = L"\"";
    unsigned int backslashes = 0;

    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }

        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashes = 0;
            continue;
        }

        if (backslashes > 0) {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
        }

        quoted.push_back(character);
    }

    if (backslashes > 0) {
        quoted.append(backslashes * 2, L'\\');
    }

    quoted.push_back(L'"');
    return quoted;
}

std::vector<std::string> SplitArguments(const std::string& arguments) {
    if (arguments.empty()) {
        return {};
    }

    const std::wstring wide_arguments = Utf8ToWide(arguments);
    int argument_count = 0;
    LPWSTR* parsed = CommandLineToArgvW(wide_arguments.c_str(), &argument_count);
    if (parsed == nullptr) {
        throw std::runtime_error("Failed to parse Windows command line arguments.");
    }

    std::vector<std::string> result;
    result.reserve(argument_count);
    for (int index = 0; index < argument_count; ++index) {
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
std::vector<std::string> SplitArguments(const std::string& arguments) {
    return SplitArgumentsPosix(arguments);
}
#endif

std::filesystem::path CurrentExecutablePath() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("Failed to resolve the current executable path.");
        }

        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }

        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("Failed to resolve the current executable path.");
    }

    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()));
#else
    std::array<char, 4096> buffer{};
    const ssize_t size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0) {
        throw std::runtime_error("Failed to resolve the current executable path.");
    }

    buffer[static_cast<std::size_t>(size)] = '\0';
    return std::filesystem::path(buffer.data());
#endif
}

void CurrentModulePathMarker() {}

std::optional<std::filesystem::path> CurrentModulePath() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CurrentModulePathMarker),
            &module)) {
        return std::nullopt;
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }

        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }

        buffer.resize(buffer.size() * 2);
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&CurrentModulePathMarker), &info) == 0 || info.dli_fname == nullptr) {
        return std::nullopt;
    }

    return std::filesystem::path(info.dli_fname);
#endif
}

std::string GetNativeFileName() {
#ifdef _WIN32
    return "justcefnative.exe";
#else
    return "justcefnative";
#endif
}

std::vector<std::filesystem::path> BuildSearchPaths() {
    const std::string native_file_name = GetNativeFileName();
    const auto executable_directory = CurrentExecutablePath().parent_path();
    const auto module_path = CurrentModulePath();
    const auto current_working_directory = std::filesystem::current_path();

    std::vector<std::filesystem::path> base_directories;
    if (module_path) {
        base_directories.push_back(module_path->parent_path());
    }
    base_directories.push_back(executable_directory);
    base_directories.push_back(current_working_directory);

    std::vector<std::filesystem::path> search_paths;
    std::set<std::filesystem::path> seen;

    const auto append = [&](const std::filesystem::path& path) {
        if (seen.insert(path).second) {
            search_paths.push_back(path);
        }
    };

    for (const auto& base : base_directories) {
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

}  // namespace

class JustCefProcessImpl
    : public WindowCommandTarget,
      public std::enable_shared_from_this<JustCefProcessImpl> {
public:
    explicit JustCefProcessImpl(asio::any_io_executor executor)
        : executor_(std::move(executor)) {}

    ~JustCefProcessImpl() {
        Shutdown(false);
    }

    void Start(const StartOptions& options) {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            throw std::runtime_error("Process has already been started.");
        }

        start_options_ = options;

        try {
            const auto native_path = JustCefProcess::ResolveNativeExecutablePath(options.native_executable_path);
            const auto working_directory =
                options.working_directory ? std::filesystem::absolute(*options.working_directory) : native_path.parent_path();
            const auto additional_arguments = SplitArguments(options.arguments);

            Logger::Info("JustCefProcess", "Searching for justcefnative, search paths:");
            for (const auto& path : JustCefProcess::GenerateSearchPaths()) {
                Logger::Info("JustCefProcess", std::string(" - ") + path.string());
            }
            Logger::Info("JustCefProcess", "Working directory '" + working_directory.string() + "'.");
            Logger::Info("JustCefProcess", "CEF exe path '" + native_path.string() + "'.");

#ifdef _WIN32
            SECURITY_ATTRIBUTES security_attributes{};
            security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
            security_attributes.bInheritHandle = TRUE;

            HANDLE child_read_handle = INVALID_HANDLE_VALUE;
            HANDLE child_write_handle = INVALID_HANDLE_VALUE;

            if (!CreatePipe(&child_read_handle, &write_handle_, &security_attributes, 0)) {
                throw std::runtime_error("Failed to create parent-to-child pipe.");
            }

            if (!CreatePipe(&read_handle_, &child_write_handle, &security_attributes, 0)) {
                CloseHandle(child_read_handle);
                throw std::runtime_error("Failed to create child-to-parent pipe.");
            }

            SetHandleInformation(write_handle_, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(read_handle_, HANDLE_FLAG_INHERIT, 0);

            std::vector<std::wstring> command_parts;
            command_parts.push_back(native_path.wstring());
            command_parts.push_back(L"--change-stack-guard-on-fork=disable");
            command_parts.push_back(L"--parent-to-child");
            command_parts.push_back(std::to_wstring(reinterpret_cast<std::uintptr_t>(child_read_handle)));
            command_parts.push_back(L"--child-to-parent");
            command_parts.push_back(std::to_wstring(reinterpret_cast<std::uintptr_t>(child_write_handle)));
            for (const auto& argument : additional_arguments) {
                command_parts.push_back(Utf8ToWide(argument));
            }

            std::wstring command_line;
            for (std::size_t index = 0; index < command_parts.size(); ++index) {
                if (index > 0) {
                    command_line.push_back(L' ');
                }
                command_line += QuoteWindowsArgument(command_parts[index]);
            }

            STARTUPINFOW startup_info{};
            startup_info.cb = sizeof(startup_info);
            PROCESS_INFORMATION process_information{};

            std::wstring mutable_command_line = command_line;
            if (!CreateProcessW(
                    nullptr,
                    mutable_command_line.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    0,
                    nullptr,
                    working_directory.wstring().c_str(),
                    &startup_info,
                    &process_information)) {
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

            if (::pipe(parent_to_child) != 0) {
                throw std::runtime_error("Failed to create parent-to-child pipe.");
            }

            if (::pipe(child_to_parent) != 0) {
                ::close(parent_to_child[0]);
                ::close(parent_to_child[1]);
                throw std::runtime_error("Failed to create child-to-parent pipe.");
            }

            const pid_t child_pid = ::fork();
            if (child_pid < 0) {
                ::close(parent_to_child[0]);
                ::close(parent_to_child[1]);
                ::close(child_to_parent[0]);
                ::close(child_to_parent[1]);
                throw std::runtime_error("Failed to fork justcefnative.");
            }

            if (child_pid == 0) {
                ::close(parent_to_child[1]);
                ::close(child_to_parent[0]);

                if (::chdir(working_directory.c_str()) != 0) {
                    std::_Exit(127);
                }

                std::vector<std::string> argv_storage;
                argv_storage.push_back(native_path.string());
                argv_storage.push_back("--change-stack-guard-on-fork=disable");
                argv_storage.push_back("--parent-to-child");
                argv_storage.push_back(std::to_string(parent_to_child[0]));
                argv_storage.push_back("--child-to-parent");
                argv_storage.push_back(std::to_string(child_to_parent[1]));
                argv_storage.insert(argv_storage.end(), additional_arguments.begin(), additional_arguments.end());

                std::vector<char*> argv;
                argv.reserve(argv_storage.size() + 1);
                for (auto& value : argv_storage) {
                    argv.push_back(value.data());
                }
                argv.push_back(nullptr);

                ::execv(native_path.c_str(), argv.data());
                std::_Exit(127);
            }

            ::close(parent_to_child[0]);
            ::close(child_to_parent[1]);
            write_handle_ = parent_to_child[1];
            read_handle_ = child_to_parent[0];
            child_pid_ = child_pid;
#endif

            receive_thread_ = std::thread([this]() { ReceiveLoop(); });
        } catch (...) {
            started_ = false;
            CloseTransportHandles();
            throw;
        }
    }

    bool HasExited() const {
        return !started_.load() || shutdown_.load();
    }

    std::vector<std::shared_ptr<JustCefWindow>> Windows() const {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        std::vector<std::shared_ptr<JustCefWindow>> windows;
        windows.reserve(windows_.size());
        for (const auto& record : windows_) {
            if (record.window) {
                windows.push_back(record.window);
            }
        }
        return windows;
    }

    std::shared_ptr<JustCefWindow> GetWindow(int identifier) const {
        const auto record = GetWindowRecord(identifier);
        return record ? record->window : nullptr;
    }

    void WaitForExit() const {
        EnsureStarted();
        exit_signal_.Wait();
    }

    asio::awaitable<void> WaitForExitAsync() const {
        EnsureStarted();
        co_await exit_signal_.AsyncWait(executor_);
    }

    void WaitForReady() const {
        EnsureStarted();
        ready_signal_.Wait();
    }

    asio::awaitable<void> WaitForReadyAsync() const {
        EnsureStarted();
        co_await ready_signal_.AsyncWait(executor_);
    }

    asio::awaitable<void> EchoAsync(std::vector<std::uint8_t> data) {
        detail::PacketWriter writer;
        writer.WriteBytes(data);
        co_await AsyncVoidCall(detail::OpcodeController::Echo, std::move(writer));
    }

    asio::awaitable<void> PingAsync() {
        co_await AsyncVoidCall(detail::OpcodeController::Ping, detail::PacketWriter{});
    }

    asio::awaitable<void> PrintAsync(std::string message) {
        detail::PacketWriter writer;
        writer.WriteString(message);
        co_await AsyncVoidCall(detail::OpcodeController::Print, std::move(writer));
    }

    asio::awaitable<std::shared_ptr<JustCefWindow>> CreateWindowAsync(const WindowCreateOptions& options) {
        EnsureStarted();

        if (options.proxy_requests && !options.request_proxy) {
            throw std::invalid_argument("When proxy_requests is true, request_proxy must be set.");
        }
        if (options.modify_requests && !options.request_modifier) {
            throw std::invalid_argument("When modify_requests is true, request_modifier must be set.");
        }

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
        writer.Write<std::int32_t>(options.minimum_width);
        writer.Write<std::int32_t>(options.minimum_height);
        writer.Write<std::int32_t>(options.preferred_width);
        writer.Write<std::int32_t>(options.preferred_height);
        writer.WriteSizePrefixedString(options.url);
        writer.WriteSizePrefixedString(options.title);
        writer.WriteSizePrefixedString(options.icon_path);
        writer.WriteSizePrefixedString(options.app_id);

        detail::PacketReader reader(co_await AsyncRawCall(detail::OpcodeController::WindowCreate, std::move(writer), asio::use_awaitable));
        const int identifier = ReadRequired<std::int32_t>(reader, "windowIdentifier");

        auto shared = std::make_shared<WindowShared>();
        shared->executor = executor_;
        shared->request_proxy = options.request_proxy;
        shared->request_modifier = options.request_modifier;

        auto window = std::shared_ptr<JustCefWindow>(
            new JustCefWindow(identifier, shared_from_this(), shared));

        {
            std::lock_guard<std::mutex> lock(windows_mutex_);
            windows_.push_back(WindowRecord{
                .identifier = identifier,
                .window = window,
                .shared = std::move(shared),
            });
        }

        co_return window;
    }

    asio::awaitable<void> NotifyExitAsync() {
        Notify(detail::OpcodeControllerNotification::Exit);
        co_return;
    }

    asio::awaitable<void> StreamOpenAsync(std::uint32_t identifier) {
        detail::PacketWriter writer;
        writer.Write<std::uint32_t>(identifier);
        co_await AsyncVoidCall(detail::OpcodeController::StreamOpen, std::move(writer));
    }

    asio::awaitable<bool> StreamDataAsync(std::uint32_t identifier, std::vector<std::uint8_t> data) {
        detail::PacketWriter writer;
        writer.Write<std::uint32_t>(identifier);
        writer.WriteBytes(data);
        detail::PacketReader reader(co_await AsyncRawCall(detail::OpcodeController::StreamData, std::move(writer), asio::use_awaitable));
        co_return ReadRequired<bool>(reader, "streamWritable");
    }

    asio::awaitable<void> StreamCloseAsync(std::uint32_t identifier) {
        detail::PacketWriter writer;
        writer.Write<std::uint32_t>(identifier);
        co_await AsyncVoidCall(detail::OpcodeController::StreamClose, std::move(writer));
    }

    asio::awaitable<void> WindowMaximizeAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowMaximize, identifier);
    }

    asio::awaitable<void> WindowMinimizeAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowMinimize, identifier);
    }

    asio::awaitable<void> WindowRestoreAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowRestore, identifier);
    }

    asio::awaitable<void> WindowShowAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowShow, identifier);
    }

    asio::awaitable<void> WindowHideAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowHide, identifier);
    }

    asio::awaitable<void> WindowActivateAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowActivate, identifier);
    }

    asio::awaitable<void> WindowBringToTopAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowBringToTop, identifier);
    }

    asio::awaitable<void> WindowSetAlwaysOnTopAsync(int identifier, bool always_on_top) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(always_on_top);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetAlwaysOnTop, std::move(writer));
    }

    asio::awaitable<void> WindowSetFullscreenAsync(int identifier, bool fullscreen) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(fullscreen);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetFullscreen, std::move(writer));
    }

    asio::awaitable<void> WindowCenterSelfAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowCenterSelf, identifier);
    }

    asio::awaitable<void> WindowSetProxyRequestsAsync(int identifier, bool enable_proxy_requests) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(enable_proxy_requests);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetProxyRequests, std::move(writer));
    }

    asio::awaitable<void> WindowSetModifyRequestsAsync(int identifier, bool enable_modify_requests, bool enable_modify_body) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        const std::uint8_t flags =
            static_cast<std::uint8_t>(((enable_modify_body ? 1U : 0U) << 1U) | (enable_modify_requests ? 1U : 0U));
        writer.Write<std::uint8_t>(flags);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetModifyRequests, std::move(writer));
    }

    asio::awaitable<void> RequestFocusAsync(int identifier) {
        co_await AsyncWindowIdentifierCall(detail::OpcodeController::WindowRequestFocus, identifier);
    }

    asio::awaitable<void> WindowLoadUrlAsync(int identifier, std::string url) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(url);
        co_await AsyncVoidCall(detail::OpcodeController::WindowLoadUrl, std::move(writer));
    }

    asio::awaitable<void> WindowSetPositionAsync(int identifier, int x, int y) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<std::int32_t>(x);
        writer.Write<std::int32_t>(y);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetPosition, std::move(writer));
    }

    asio::awaitable<Position> WindowGetPositionAsync(int identifier) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_return co_await AsyncParsedCall<Position>(detail::OpcodeController::WindowGetPosition, std::move(writer), [](detail::PacketReader& reader) {
            return Position{
                .x = ReadRequired<std::int32_t>(reader, "x"),
                .y = ReadRequired<std::int32_t>(reader, "y"),
            };
        });
    }

    asio::awaitable<void> WindowSetSizeAsync(int identifier, int width, int height) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<std::int32_t>(width);
        writer.Write<std::int32_t>(height);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetSize, std::move(writer));
    }

    asio::awaitable<Size> WindowGetSizeAsync(int identifier) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_return co_await AsyncParsedCall<Size>(detail::OpcodeController::WindowGetSize, std::move(writer), [](detail::PacketReader& reader) {
            return Size{
                .width = ReadRequired<std::int32_t>(reader, "width"),
                .height = ReadRequired<std::int32_t>(reader, "height"),
            };
        });
    }

    asio::awaitable<void> WindowSetZoomAsync(int identifier, double zoom) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<double>(zoom);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetZoom, std::move(writer));
    }

    asio::awaitable<double> WindowGetZoomAsync(int identifier) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_return co_await AsyncParsedCall<double>(detail::OpcodeController::WindowGetZoom, std::move(writer), [](detail::PacketReader& reader) {
            return ReadRequired<double>(reader, "zoom");
        });
    }

    asio::awaitable<void> WindowSetDevelopmentToolsEnabledAsync(int identifier, bool enabled) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(enabled);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetDevelopmentToolsEnabled, std::move(writer));
    }

    asio::awaitable<void> WindowSetDevelopmentToolsVisibleAsync(int identifier, bool visible) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(visible);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetDevelopmentToolsVisible, std::move(writer));
    }

    asio::awaitable<void> WindowCloseAsync(int identifier, bool force_close) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.Write<bool>(force_close);
        co_await AsyncVoidCall(detail::OpcodeController::WindowClose, std::move(writer));
    }

    asio::awaitable<std::vector<std::string>> PickFileAsync(bool multiple, std::vector<FileFilter> filters) {
        detail::PacketWriter writer;
        writer.Write<bool>(multiple);
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(filters.size()));
        for (const auto& filter : filters) {
            writer.WriteSizePrefixedString(filter.name);
            writer.WriteSizePrefixedString(filter.pattern);
        }

        co_return co_await AsyncParsedCall<std::vector<std::string>>(detail::OpcodeController::PickFile, std::move(writer), [](detail::PacketReader& reader) {
            const auto count = ReadRequired<std::uint32_t>(reader, "pathCount");
            std::vector<std::string> paths;
            paths.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                paths.push_back(ReadRequiredString(reader, "path"));
            }
            return paths;
        });
    }

    asio::awaitable<std::string> PickDirectoryAsync() {
        co_return co_await AsyncParsedCall<std::string>(detail::OpcodeController::PickDirectory, detail::PacketWriter{}, [](detail::PacketReader& reader) {
            return ReadRequiredString(reader, "directory");
        });
    }

    asio::awaitable<std::string> SaveFileAsync(std::string default_name, std::vector<FileFilter> filters) {
        detail::PacketWriter writer;
        writer.WriteSizePrefixedString(default_name);
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(filters.size()));
        for (const auto& filter : filters) {
            writer.WriteSizePrefixedString(filter.name);
            writer.WriteSizePrefixedString(filter.pattern);
        }

        co_return co_await AsyncParsedCall<std::string>(detail::OpcodeController::SaveFile, std::move(writer), [](detail::PacketReader& reader) {
            return ReadRequiredString(reader, "path");
        });
    }

    asio::awaitable<DevToolsMethodResult> WindowExecuteDevToolsMethodAsync(
        int identifier,
        std::string method_name,
        std::optional<std::string> json) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(method_name);
        if (json) {
            writer.WriteSizePrefixedString(*json);
        }

        co_return co_await AsyncParsedCall<DevToolsMethodResult>(
            detail::OpcodeController::WindowExecuteDevToolsMethod,
            std::move(writer),
            [](detail::PacketReader& reader) {
                DevToolsMethodResult result;
                result.success = ReadRequired<bool>(reader, "success");
                const auto size = ReadRequired<std::uint32_t>(reader, "resultSize");
                result.data = reader.ReadBytes(size);
                return result;
            });
    }

    asio::awaitable<void> WindowSetTitleAsync(int identifier, std::string title) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(title);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetTitle, std::move(writer));
    }

    asio::awaitable<void> WindowSetIconAsync(int identifier, std::string icon_path) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(icon_path);
        co_await AsyncVoidCall(detail::OpcodeController::WindowSetIcon, std::move(writer));
    }

    asio::awaitable<void> WindowAddUrlToProxyAsync(int identifier, std::string url) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddUrlToProxy, identifier, std::move(url));
    }

    asio::awaitable<void> WindowRemoveUrlToProxyAsync(int identifier, std::string url) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveUrlToProxy, identifier, std::move(url));
    }

    asio::awaitable<void> WindowAddDomainToProxyAsync(int identifier, std::string domain) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddDomainToProxy, identifier, std::move(domain));
    }

    asio::awaitable<void> WindowRemoveDomainToProxyAsync(int identifier, std::string domain) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveDomainToProxy, identifier, std::move(domain));
    }

    asio::awaitable<void> WindowAddUrlToModifyAsync(int identifier, std::string url) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddUrlToModify, identifier, std::move(url));
    }

    asio::awaitable<void> WindowRemoveUrlToModifyAsync(int identifier, std::string url) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveUrlToModify, identifier, std::move(url));
    }

    asio::awaitable<void> WindowAddDevToolsEventMethod(int identifier, std::string method) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowAddDevToolsEventMethod, identifier, std::move(method));
    }

    asio::awaitable<void> WindowRemoveDevToolsEventMethod(int identifier, std::string method) {
        co_await AsyncWindowStringCall(detail::OpcodeController::WindowRemoveDevToolsEventMethod, identifier, std::move(method));
    }

    void Dispose() {
        Shutdown(false);
    }

private:
    struct PendingRequest {
        std::function<void(std::exception_ptr, std::vector<std::uint8_t>)> completion;
    };

    struct OutgoingStreamState {
        std::shared_ptr<ByteStream> stream;
        std::atomic<bool> canceled = false;
    };

    struct WindowRecord {
        int identifier = 0;
        std::shared_ptr<JustCefWindow> window;
        std::shared_ptr<WindowShared> shared;
    };

    std::optional<WindowRecord> GetWindowRecord(int identifier) const {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        const auto iterator = std::find_if(windows_.begin(), windows_.end(), [identifier](const WindowRecord& record) {
            return record.identifier == identifier;
        });
        if (iterator == windows_.end()) {
            return std::nullopt;
        }
        return *iterator;
    }

    std::optional<WindowRecord> RemoveWindowRecord(int identifier) {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        const auto iterator = std::find_if(windows_.begin(), windows_.end(), [identifier](const WindowRecord& record) {
            return record.identifier == identifier;
        });
        if (iterator == windows_.end()) {
            return std::nullopt;
        }

        WindowRecord removed = std::move(*iterator);
        windows_.erase(iterator);
        return removed;
    }

    template <typename CompletionToken>
    typename asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr, std::vector<std::uint8_t>)>::return_type
    AsyncRawCall(detail::OpcodeController opcode, detail::PacketWriter writer, CompletionToken&& token) {
        return asio::async_initiate<CompletionToken, void(std::exception_ptr, std::vector<std::uint8_t>)>(
            [this, opcode, writer = std::move(writer)](auto handler) mutable {
                using Handler = std::decay_t<decltype(handler)>;

                auto handler_ptr = std::make_shared<Handler>(std::move(handler));
                auto handler_executor = asio::get_associated_executor(*handler_ptr, executor_);

                BeginCall(
                    opcode,
                    writer,
                    [handler_ptr, handler_executor](std::exception_ptr exception, std::vector<std::uint8_t> body) mutable {
                        asio::dispatch(handler_executor, [handler_ptr, exception, body = std::move(body)]() mutable {
                            auto completion_handler = std::move(*handler_ptr);
                            completion_handler(exception, std::move(body));
                        });
                    });
            },
            token);
    }

    template <typename T, typename Parser>
    asio::awaitable<T> AsyncParsedCall(detail::OpcodeController opcode, detail::PacketWriter writer, Parser parser) {
        detail::PacketReader reader(co_await AsyncRawCall(opcode, std::move(writer), asio::use_awaitable));
        co_return parser(reader);
    }

    asio::awaitable<void> AsyncVoidCall(detail::OpcodeController opcode, detail::PacketWriter writer) {
        (void)co_await AsyncRawCall(opcode, std::move(writer), asio::use_awaitable);
        co_return;
    }

    asio::awaitable<void> AsyncWindowIdentifierCall(detail::OpcodeController opcode, int identifier) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        co_await AsyncVoidCall(opcode, std::move(writer));
    }

    asio::awaitable<void> AsyncWindowStringCall(detail::OpcodeController opcode, int identifier, std::string value) {
        detail::PacketWriter writer;
        writer.Write<std::int32_t>(identifier);
        writer.WriteSizePrefixedString(value);
        co_await AsyncVoidCall(opcode, std::move(writer));
    }

    void EnsureStarted() const {
        if (!started_.load()) {
            throw std::runtime_error("Process should be started.");
        }

        if (shutdown_.load()) {
            throw std::runtime_error("Process is no longer running.");
        }
    }

    void BeginCall(
        detail::OpcodeController opcode,
        const detail::PacketWriter& writer,
        std::function<void(std::exception_ptr, std::vector<std::uint8_t>)> completion) {
        EnsureStarted();

        const std::uint32_t request_id = ++request_id_counter_;
        {
            std::lock_guard<std::mutex> lock(pending_requests_mutex_);
            pending_requests_[request_id] = PendingRequest{.completion = std::move(completion)};
        }

        try {
            SendPacket(detail::PacketType::Request, static_cast<std::uint8_t>(opcode), request_id, writer.Buffer());
        } catch (...) {
            PendingRequest pending;
            {
                std::lock_guard<std::mutex> lock(pending_requests_mutex_);
                const auto iterator = pending_requests_.find(request_id);
                if (iterator != pending_requests_.end()) {
                    pending = std::move(iterator->second);
                    pending_requests_.erase(iterator);
                }
            }
            if (pending.completion) {
                pending.completion(std::current_exception(), {});
            }
        }
    }

    void Notify(detail::OpcodeControllerNotification opcode) {
        EnsureStarted();
        SendPacket(detail::PacketType::Notification, static_cast<std::uint8_t>(opcode), 0, {});
    }

    void SendPacket(
        detail::PacketType packet_type,
        std::uint8_t opcode,
        std::uint32_t request_id,
        const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> packet(detail::kPacketHeaderSize + body.size());
        const std::uint32_t size = static_cast<std::uint32_t>(packet.size() - sizeof(std::uint32_t));
        std::memcpy(packet.data(), &size, sizeof(size));
        std::memcpy(packet.data() + 4, &request_id, sizeof(request_id));
        packet[8] = static_cast<std::uint8_t>(packet_type);
        packet[9] = opcode;
        if (!body.empty()) {
            std::memcpy(packet.data() + detail::kPacketHeaderSize, body.data(), body.size());
        }

        std::lock_guard<std::mutex> lock(write_mutex_);
        WriteAll(packet.data(), packet.size());
    }

    bool ReadExactly(void* buffer, std::size_t size) {
        std::size_t total = 0;
        while (total < size) {
#ifdef _WIN32
            DWORD read = 0;
            if (!ReadFile(read_handle_, static_cast<char*>(buffer) + total, static_cast<DWORD>(size - total), &read, nullptr) || read == 0) {
                return false;
            }
            total += read;
#else
            const ssize_t read = ::read(read_handle_, static_cast<char*>(buffer) + total, size - total);
            if (read == 0) {
                return false;
            }
            if (read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            total += static_cast<std::size_t>(read);
#endif
        }

        return true;
    }

    void WriteAll(const void* buffer, std::size_t size) {
        std::size_t total = 0;
        while (total < size) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(write_handle_, static_cast<const char*>(buffer) + total, static_cast<DWORD>(size - total), &written, nullptr) || written == 0) {
                throw std::runtime_error("Failed to write IPC packet.");
            }
            total += written;
#else
            const ssize_t written = ::write(write_handle_, static_cast<const char*>(buffer) + total, size - total);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("Failed to write IPC packet.");
            }
            if (written == 0) {
                throw std::runtime_error("Failed to write IPC packet.");
            }
            total += static_cast<std::size_t>(written);
#endif
        }
    }

    void ReceiveLoop() {
        Logger::Info("JustCefProcess", "Receive loop started.");

        try {
            std::array<std::uint8_t, detail::kPacketHeaderSize> header_bytes{};
            while (!shutdown_.load()) {
                if (!ReadExactly(header_bytes.data(), header_bytes.size())) {
                    break;
                }

                detail::PacketHeader header{};
                std::memcpy(&header.size, header_bytes.data(), sizeof(header.size));
                std::memcpy(&header.request_id, header_bytes.data() + 4, sizeof(header.request_id));
                header.packet_type = static_cast<detail::PacketType>(header_bytes[8]);
                header.opcode = header_bytes[9];

                const std::size_t body_size = header.size + sizeof(std::uint32_t) - detail::kPacketHeaderSize;
                if (body_size > detail::kMaxIpcSize) {
                    throw std::runtime_error("Invalid packet size received from justcefnative.");
                }

                std::vector<std::uint8_t> body(body_size);
                if (body_size > 0 && !ReadExactly(body.data(), body.size())) {
                    throw std::runtime_error("Failed to read the full IPC packet body.");
                }

                if (header.packet_type == detail::PacketType::Response) {
                    PendingRequest pending;
                    {
                        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
                        const auto iterator = pending_requests_.find(header.request_id);
                        if (iterator != pending_requests_.end()) {
                            pending = std::move(iterator->second);
                            pending_requests_.erase(iterator);
                        }
                    }

                    if (pending.completion) {
                        pending.completion(nullptr, std::move(body));
                    } else {
                        Logger::Error(
                            "JustCefProcess",
                            "Received a packet response for a request that no longer has an awaiter.");
                    }
                } else if (header.packet_type == detail::PacketType::Request) {
                    asio::co_spawn(
                        executor_,
                        [this, opcode = static_cast<detail::OpcodeClient>(header.opcode), request_id = header.request_id, body = std::move(body)]() mutable -> asio::awaitable<void> {
                            co_await HandleIncomingRequest(opcode, request_id, std::move(body));
                        },
                        asio::detached);
                } else if (header.packet_type == detail::PacketType::Notification) {
                    asio::dispatch(executor_, [this, opcode = static_cast<detail::OpcodeClientNotification>(header.opcode), body = std::move(body)]() mutable {
                        detail::PacketReader reader(std::move(body));
                        HandleNotification(opcode, reader);
                    });
                } else {
                    throw std::runtime_error("Received an unknown IPC packet type.");
                }
            }
        } catch (...) {
            Logger::Error("JustCefProcess", "An exception occurred in the IPC receive loop.", std::current_exception());
        }

        Logger::Info("JustCefProcess", "Receive loop stopped.");
        Shutdown(true);
    }

    asio::awaitable<void> HandleIncomingRequest(detail::OpcodeClient opcode, std::uint32_t request_id, std::vector<std::uint8_t> body) {
        try {
            detail::PacketReader reader(std::move(body));
            detail::PacketWriter writer;

            switch (opcode) {
                case detail::OpcodeClient::Ping:
                    break;
                case detail::OpcodeClient::Print:
                    if (const auto text = reader.ReadString(reader.RemainingSize())) {
                        Logger::Info("JustCefProcess", *text);
                    }
                    break;
                case detail::OpcodeClient::Echo:
                    writer.WriteBytes(reader.ReadBytes(reader.RemainingSize()));
                    break;
                case detail::OpcodeClient::WindowProxyRequest:
                    co_await HandleWindowProxyRequest(reader, writer);
                    break;
                case detail::OpcodeClient::WindowModifyRequest:
                    co_await HandleWindowModifyRequest(reader, writer);
                    break;
                case detail::OpcodeClient::StreamClose: {
                    const auto identifier = ReadRequired<std::uint32_t>(reader, "streamIdentifier");
                    std::shared_ptr<OutgoingStreamState> stream_state;
                    {
                        std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
                        const auto iterator = outgoing_streams_.find(identifier);
                        if (iterator != outgoing_streams_.end()) {
                            stream_state = iterator->second;
                        }
                    }

                    if (stream_state) {
                        stream_state->canceled = true;
                        if (stream_state->stream) {
                            stream_state->stream->Close();
                        }
                    }
                    break;
                }
                default:
                    Logger::Warning("JustCefProcess", "Received an unhandled client opcode.");
                    break;
            }

            SendPacket(detail::PacketType::Response, static_cast<std::uint8_t>(opcode), request_id, writer.Buffer());
        } catch (...) {
            Logger::Error("JustCefProcess", "Exception occurred while processing IPC request.", std::current_exception());
            try {
                SendPacket(detail::PacketType::Response, static_cast<std::uint8_t>(opcode), request_id, {});
            } catch (...) {
            }
        }
        co_return;
    }

    asio::awaitable<void> HandleWindowProxyRequest(detail::PacketReader& reader, detail::PacketWriter& writer) {
        const ParsedWindowRequest parsed = ReadWindowRequest(reader);
        const auto record = GetWindowRecord(parsed.identifier);
        if (!record || !record->window || !record->shared) {
            co_return;
        }

        RequestProxy request_proxy;
        {
            std::lock_guard<std::mutex> lock(record->shared->request_mutex);
            request_proxy = record->shared->request_proxy;
        }

        if (!request_proxy) {
            co_return;
        }

        std::optional<IPCResponse> response;
        try {
            response = co_await request_proxy(*record->window, parsed.request);
        } catch (...) {
            Logger::Error("JustCefWindow", "Exception occurred while processing request proxy.", std::current_exception());
            try {
                asio::co_spawn(executor_, record->window->CloseAsync(true), asio::detached);
            } catch (...) {
            }

            response = IPCResponse{
                .status_code = 404,
                .status_text = "Not Found",
                .headers = HeaderMap{},
                .body_stream = nullptr,
            };
        }

        if (!response) {
            co_return;
        }

        const HeaderMap filtered_headers = FilterResponseHeaders(response->headers);

        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(response->status_code));
        writer.WriteSizePrefixedString(response->status_text);
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(filtered_headers.size()));
        for (const auto& [key, values] : filtered_headers) {
            std::string joined;
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index > 0) {
                    joined += ", ";
                }
                joined += values[index];
            }

            writer.WriteSizePrefixedString(key);
            writer.WriteSizePrefixedString(joined);
        }

        if (!response->body_stream) {
            writer.Write<std::uint8_t>(0);
            co_return;
        }

        const auto content_length = ParseContentLength(filtered_headers);
        if (content_length && *content_length < static_cast<std::uint64_t>(detail::kMaxIpcSize - writer.Size())) {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(*content_length));
            std::size_t total = 0;
            while (total < buffer.size()) {
                const std::size_t read = response->body_stream->Read(buffer.data() + total, buffer.size() - total);
                if (read == 0) {
                    break;
                }
                total += read;
            }
            response->body_stream->Close();

            writer.Write<std::uint8_t>(1);
            writer.Write<std::uint32_t>(static_cast<std::uint32_t>(total));
            writer.WriteBytes(buffer.data(), total);
            co_return;
        }

        writer.Write<std::uint8_t>(2);
        co_await HandleLargeOrChunkedContent(response->body_stream, writer, content_length);
    }

    asio::awaitable<void> HandleLargeOrChunkedContent(
        std::shared_ptr<ByteStream> stream,
        detail::PacketWriter& writer,
        std::optional<std::uint64_t> content_length) {
        const std::uint32_t stream_identifier = ++stream_identifier_counter_;
        auto outgoing_state = std::make_shared<OutgoingStreamState>();
        outgoing_state->stream = std::move(stream);
        {
            std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
            outgoing_streams_[stream_identifier] = outgoing_state;
        }

        detail::PacketWriter open_writer;
        open_writer.Write<std::uint32_t>(stream_identifier);
        co_await StreamOpenAsync(stream_identifier);
        writer.Write<std::uint32_t>(stream_identifier);

        asio::co_spawn(executor_, [this, stream_identifier, outgoing_state, content_length]() -> asio::awaitable<void> {
            try {
                std::array<std::uint8_t, 65536> buffer{};
                std::uint64_t total_read = 0;

                while (!outgoing_state->canceled.load()) {
                    std::size_t request_size = buffer.size();
                    if (content_length) {
                        if (total_read >= *content_length) {
                            break;
                        }
                        request_size = static_cast<std::size_t>(std::min<std::uint64_t>(request_size, *content_length - total_read));
                    }

                    const std::size_t bytes_read = outgoing_state->stream->Read(buffer.data(), request_size);
                    if (bytes_read == 0) {
                        break;
                    }

                    if (outgoing_state->canceled.load()) {
                        break;
                    }

                    if (!co_await StreamDataAsync(
                            stream_identifier,
                            std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes_read)))) {
                        throw std::runtime_error("Stream closed.");
                    }

                    total_read += bytes_read;
                }
            } catch (...) {
                Logger::Error("JustCefProcess", "Failed to stream response body.", std::current_exception());
            }

            try {
                if (outgoing_state->stream) {
                    outgoing_state->stream->Close();
                }
            } catch (...) {
            }

            try {
                co_await StreamCloseAsync(stream_identifier);
            } catch (...) {
            }

            std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
            outgoing_streams_.erase(stream_identifier);
            co_return;
        }, asio::detached);
        co_return;
    }

    asio::awaitable<void> HandleWindowModifyRequest(detail::PacketReader& reader, detail::PacketWriter& writer) {
        const ParsedWindowRequest parsed = ReadWindowRequest(reader);
        const auto record = GetWindowRecord(parsed.identifier);
        if (!record || !record->window || !record->shared) {
            co_return;
        }

        RequestModifier request_modifier;
        {
            std::lock_guard<std::mutex> lock(record->shared->request_mutex);
            request_modifier = record->shared->request_modifier;
        }

        std::optional<IPCRequest> modified_request;
        try {
            if (request_modifier) {
                modified_request = co_await request_modifier(*record->window, parsed.request);
            } else {
                modified_request = parsed.request;
            }
        } catch (...) {
            Logger::Error("JustCefWindow", "Exception occurred while processing modify request.", std::current_exception());
            try {
                asio::co_spawn(executor_, record->window->CloseAsync(true), asio::detached);
            } catch (...) {
            }
            modified_request = parsed.request;
        }

        if (!modified_request) {
            co_return;
        }

        SerializeModifyRequest(writer, *modified_request);
        co_return;
    }

    void HandleNotification(detail::OpcodeClientNotification opcode, detail::PacketReader& reader) {
        switch (opcode) {
            case detail::OpcodeClientNotification::Exit:
                Logger::Info("JustCefProcess", "CEF process is exiting.");
                Shutdown(false);
                break;
            case detail::OpcodeClientNotification::Ready:
                Logger::Info("JustCefProcess", "Client is ready.");
                ready_signal_.SignalSuccess();
                break;
            case detail::OpcodeClientNotification::WindowOpened:
                Logger::Info("JustCefProcess", "Window opened: " + std::to_string(ReadRequired<std::int32_t>(reader, "identifier")));
                break;
            case detail::OpcodeClientNotification::WindowClosed: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                SignalWindowClosed(RemoveWindowRecord(identifier));
                break;
            }
            case detail::OpcodeClientNotification::WindowFocused: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                if (auto window = GetWindow(identifier)) {
                    window->OnFocused.Emit();
                }
                break;
            }
            case detail::OpcodeClientNotification::WindowUnfocused: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                if (auto window = GetWindow(identifier)) {
                    window->OnUnfocused.Emit();
                }
                break;
            }
            case detail::OpcodeClientNotification::WindowFullscreenChanged: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                const bool fullscreen = ReadRequired<bool>(reader, "fullscreen");
                if (auto window = GetWindow(identifier)) {
                    window->OnFullscreenChanged.Emit(fullscreen);
                }
                break;
            }
            case detail::OpcodeClientNotification::WindowLoadStart: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                const auto url = reader.ReadSizePrefixedString();
                if (auto window = GetWindow(identifier)) {
                    window->OnLoadStart.Emit(url);
                }
                break;
            }
            case detail::OpcodeClientNotification::WindowLoadEnd: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                const auto url = reader.ReadSizePrefixedString();
                if (auto window = GetWindow(identifier)) {
                    window->OnLoadEnd.Emit(url);
                }
                break;
            }
            case detail::OpcodeClientNotification::WindowLoadError: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                const int error_code = ReadRequired<std::int32_t>(reader, "errorCode");
                const auto error_text = reader.ReadSizePrefixedString();
                const auto failed_url = reader.ReadSizePrefixedString();
                if (auto window = GetWindow(identifier)) {
                    window->OnLoadError.Emit(error_code, error_text, failed_url);
                }
                break;
            }
            case detail::OpcodeClientNotification::WindowDevToolsEvent: {
                const int identifier = ReadRequired<std::int32_t>(reader, "identifier");
                const auto method = reader.ReadSizePrefixedString();
                const auto size = ReadRequired<std::int32_t>(reader, "paramsSize");
                if (size < 0) {
                    throw std::runtime_error("Negative devtools payload size.");
                }
                auto payload = reader.ReadBytes(static_cast<std::size_t>(size));
                if (auto window = GetWindow(identifier)) {
                    window->OnDevToolsEvent.Emit(method, std::move(payload));
                }
                break;
            }
            default:
                Logger::Info("JustCefProcess", "Received unhandled notification opcode.");
                break;
        }
    }

    void SignalWindowClosed(const std::optional<WindowRecord>& record) {
        if (!record || !record->shared) {
            return;
        }

        bool expected = false;
        if (!record->shared->close_signaled.compare_exchange_strong(expected, true)) {
            return;
        }

        record->shared->close_signal.SignalSuccess();
        if (record->window) {
            asio::dispatch(record->shared->executor, [window = record->window]() { window->OnClose.Emit(); });
        }
    }

    void CloseTransportHandles() {
#ifdef _WIN32
        if (read_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(read_handle_);
            read_handle_ = INVALID_HANDLE_VALUE;
        }
        if (write_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(write_handle_);
            write_handle_ = INVALID_HANDLE_VALUE;
        }
        if (process_handle_ != nullptr) {
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
        }
#else
        if (read_handle_ != -1) {
            ::close(read_handle_);
            read_handle_ = -1;
        }
        if (write_handle_ != -1) {
            ::close(write_handle_);
            write_handle_ = -1;
        }
#endif
    }

    void Shutdown(bool from_receive_thread) {
        bool expected = false;
        if (!shutdown_.compare_exchange_strong(expected, true)) {
            if (!from_receive_thread && receive_thread_.joinable()) {
                receive_thread_.join();
            }
            return;
        }

        CloseTransportHandles();

        if (!from_receive_thread && receive_thread_.joinable()) {
            receive_thread_.join();
        }

        if (!ready_signal_.IsSignaled()) {
            ready_signal_.SignalFailure(std::make_exception_ptr(std::runtime_error("Process disposed before ready.")));
        }

        std::vector<PendingRequest> pending_to_fail;
        {
            std::lock_guard<std::mutex> lock(pending_requests_mutex_);
            pending_to_fail.reserve(pending_requests_.size());
            for (auto& [_, pending] : pending_requests_) {
                pending_to_fail.push_back(std::move(pending));
            }
            pending_requests_.clear();
        }

        const auto shutdown_exception =
            std::make_exception_ptr(std::runtime_error("Process disposed while awaiting IPC response."));
        for (auto& pending : pending_to_fail) {
            if (pending.completion) {
                pending.completion(shutdown_exception, {});
            }
        }

        {
            std::lock_guard<std::mutex> lock(outgoing_streams_mutex_);
            for (auto& [_, stream] : outgoing_streams_) {
                stream->canceled = true;
                if (stream->stream) {
                    stream->stream->Close();
                }
            }
            outgoing_streams_.clear();
        }

        std::vector<WindowRecord> windows_to_close;
        {
            std::lock_guard<std::mutex> lock(windows_mutex_);
            windows_to_close.swap(windows_);
        }

        for (const auto& record : windows_to_close) {
            SignalWindowClosed(record);
        }

        exit_signal_.SignalSuccess();
    }

    asio::any_io_executor executor_;
    detail::AsyncSignal ready_signal_;
    detail::AsyncSignal exit_signal_;
    std::atomic<bool> started_ = false;
    std::atomic<bool> shutdown_ = false;
    StartOptions start_options_;
    std::atomic<std::uint32_t> request_id_counter_ = 0;
    std::atomic<std::uint32_t> stream_identifier_counter_ = 0;
    mutable std::mutex windows_mutex_;
    std::vector<WindowRecord> windows_;
    std::mutex pending_requests_mutex_;
    std::unordered_map<std::uint32_t, PendingRequest> pending_requests_;
    std::mutex outgoing_streams_mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<OutgoingStreamState>> outgoing_streams_;
    std::mutex write_mutex_;
    std::thread receive_thread_;

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

JustCefProcess::JustCefProcess() : impl_(std::make_shared<JustCefProcessImpl>(asio::system_executor())) {}

JustCefProcess::JustCefProcess(asio::any_io_executor executor)
    : impl_(std::make_shared<JustCefProcessImpl>(std::move(executor))) {}

JustCefProcess::~JustCefProcess() = default;

void JustCefProcess::Start(const std::string& args) {
    Start(StartOptions{.arguments = args});
}

void JustCefProcess::Start(const StartOptions& options) {
    impl_->Start(options);
}

bool JustCefProcess::HasExited() const {
    return impl_->HasExited();
}

std::vector<std::shared_ptr<JustCefWindow>> JustCefProcess::Windows() const {
    return impl_->Windows();
}

std::shared_ptr<JustCefWindow> JustCefProcess::GetWindow(int identifier) const {
    return impl_->GetWindow(identifier);
}

void JustCefProcess::WaitForExit() const {
    impl_->WaitForExit();
}

asio::awaitable<void> JustCefProcess::WaitForExitAsync() const {
    return impl_->WaitForExitAsync();
}

void JustCefProcess::WaitForReady() const {
    impl_->WaitForReady();
}

asio::awaitable<void> JustCefProcess::WaitForReadyAsync() const {
    return impl_->WaitForReadyAsync();
}

asio::awaitable<void> JustCefProcess::EchoAsync(std::vector<std::uint8_t> data) {
    return impl_->EchoAsync(std::move(data));
}

asio::awaitable<void> JustCefProcess::PingAsync() {
    return impl_->PingAsync();
}

asio::awaitable<void> JustCefProcess::PrintAsync(std::string message) {
    return impl_->PrintAsync(std::move(message));
}

asio::awaitable<std::shared_ptr<JustCefWindow>> JustCefProcess::CreateWindowAsync(const WindowCreateOptions& options) {
    return impl_->CreateWindowAsync(options);
}

asio::awaitable<std::shared_ptr<JustCefWindow>> JustCefProcess::CreateWindowAsync(
    std::string url,
    int minimum_width,
    int minimum_height,
    int preferred_width,
    int preferred_height,
    bool fullscreen,
    bool context_menu_enable,
    bool shown,
    bool developer_tools_enabled,
    bool resizable,
    bool frameless,
    bool centered,
    bool proxy_requests,
    bool log_console,
    RequestProxy request_proxy,
    bool modify_requests,
    RequestModifier request_modifier,
    bool modify_request_body,
    std::optional<std::string> title,
    std::optional<std::string> icon_path,
    std::optional<std::string> app_id) {
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
    });
}

asio::awaitable<void> JustCefProcess::NotifyExitAsync() {
    return impl_->NotifyExitAsync();
}

asio::awaitable<void> JustCefProcess::StreamOpenAsync(std::uint32_t identifier) {
    return impl_->StreamOpenAsync(identifier);
}

asio::awaitable<bool> JustCefProcess::StreamDataAsync(std::uint32_t identifier, std::vector<std::uint8_t> data) {
    return impl_->StreamDataAsync(identifier, std::move(data));
}

asio::awaitable<void> JustCefProcess::StreamCloseAsync(std::uint32_t identifier) {
    return impl_->StreamCloseAsync(identifier);
}

asio::awaitable<std::vector<std::string>> JustCefProcess::PickFileAsync(bool multiple, std::vector<FileFilter> filters) {
    return impl_->PickFileAsync(multiple, std::move(filters));
}

asio::awaitable<std::string> JustCefProcess::PickDirectoryAsync() {
    return impl_->PickDirectoryAsync();
}

asio::awaitable<std::string> JustCefProcess::SaveFileAsync(std::string default_name, std::vector<FileFilter> filters) {
    return impl_->SaveFileAsync(std::move(default_name), std::move(filters));
}

void JustCefProcess::Dispose() {
    impl_->Dispose();
}

std::vector<std::filesystem::path> JustCefProcess::GenerateSearchPaths() {
    return BuildSearchPaths();
}

std::filesystem::path JustCefProcess::ResolveNativeExecutablePath(
    const std::optional<std::filesystem::path>& native_executable_path) {
    if (native_executable_path) {
        const auto resolved = std::filesystem::absolute(*native_executable_path);
        if (!std::filesystem::exists(resolved)) {
            throw std::runtime_error("Failed to find justcefnative at '" + resolved.string() + "'.");
        }
        return resolved;
    }

    for (const auto& candidate : GenerateSearchPaths()) {
        if (std::filesystem::exists(candidate)) {
            return std::filesystem::absolute(candidate);
        }
    }

    throw std::runtime_error("Failed to find justcefnative.");
}

}  // namespace justcef
